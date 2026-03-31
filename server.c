#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

#include "protocol.h"
#include "game.h"

#ifndef PORT
#define PORT 4242
#endif

#define MIN_PLAYERS     2
#define RESOLVE_DELAY_US 2000000  /* 2.0 seconds between events */

/* =========================================================================
 * Utility: write entire buffer
 * ========================================================================= */

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t rem = len;
    while (rem > 0) {
        ssize_t n = write(fd, p, rem);
        if (n <= 0) return -1;
        p   += n;
        rem -= (size_t)n;
    }
    return 0;
}

/* =========================================================================
 * Timer / resolve state
 * ========================================================================= */

static time_t         last_tick = 0;
static struct timeval  resolve_last_send;

/* Forward declarations */
static void disconnect_player(GameState *gs, int player_id);
static void send_game_start(GameState *gs);
static void begin_round(GameState *gs);
static void finish_resolution(GameState *gs);

/* =========================================================================
 * Broadcast helpers
 * ========================================================================= */

static void broadcast(GameState *gs, const void *msg, size_t len)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].fd != -1) {
            if (write_all(gs->players[i].fd, msg, len) < 0)
                disconnect_player(gs, i);
        }
    }
}

static void fill_player_info(GameState *gs, PlayerInfo *pinfo)
{
    memset(pinfo, 0, sizeof(PlayerInfo) * MAX_PLAYERS);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &gs->players[i];
        pinfo[i].player_id = p->id;
        pinfo[i].hp = (uint8_t)(p->hp < 0 ? 0 : (p->hp > 255 ? 255 : p->hp));
        pinfo[i].alive = (uint8_t)p->alive;
        pinfo[i].char_type = (uint8_t)p->char_type;
        pinfo[i].speed = (uint8_t)game_get_current_speed(p);
        pinfo[i].max_hp = (uint8_t)(p->max_hp > 255 ? 255 : p->max_hp);
        pinfo[i].char_id = (p->char_id >= 0) ? (uint8_t)p->char_id : NO_CHARACTER;
        pinfo[i].padding = 0;
        strncpy(pinfo[i].name, p->name, PLAYER_NAME_LEN - 1);
    }
}

static void send_lobby_update(GameState *gs)
{
    LobbyUpdateMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type     = MSG_LOBBY_UPDATE;
    m.player_count = (uint8_t)gs->player_count;
    m.host_id      = (gs->host_id >= 0) ? (uint8_t)gs->host_id : 0xFF;
    fill_player_info(gs, m.players);
    broadcast(gs, &m, sizeof(m));
}

static int send_char_list(GameState *gs, int fd)
{
    CharListMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type   = MSG_CHAR_LIST;
    m.char_count = NUM_CHARACTERS;
    game_build_char_list(gs, m.chars);
    return write_all(fd, &m, sizeof(m));
}

static void send_game_start(GameState *gs)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].fd == -1) continue;
        Player *p = &gs->players[i];

        GameStartMsg m;
        memset(&m, 0, sizeof(m));
        m.msg_type     = MSG_GAME_START;
        m.your_id      = (uint8_t)i;
        m.player_count = (uint8_t)gs->player_count;

        int act_count = 0;
        game_get_char_actions(p->char_id, m.actions, &act_count);
        m.action_count = (uint8_t)act_count;

        fill_player_info(gs, m.players);
        if (write_all(p->fd, &m, sizeof(m)) < 0)
            disconnect_player(gs, i);
    }
}

static void send_round_start(GameState *gs)
{
    RoundStartMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type     = MSG_ROUND_START;
    m.round_num    = (uint8_t)gs->round_num;
    m.timer_secs   = ROUND_TIMER_SECS;
    m.player_count = (uint8_t)gs->player_count;
    fill_player_info(gs, m.players);
    broadcast(gs, &m, sizeof(m));
}

static void send_timer_tick(GameState *gs, int seconds_left)
{
    TimerTickMsg m;
    m.msg_type     = MSG_TIMER_TICK;
    m.seconds_left = (uint8_t)seconds_left;
    m.padding[0]   = 0;
    m.padding[1]   = 0;
    broadcast(gs, &m, sizeof(m));
}

static void send_round_event(GameState *gs, const char *text)
{
    RoundEventMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type = MSG_ROUND_EVENT;
    strncpy(m.text, text, EVENT_TEXT_LEN - 1);
    broadcast(gs, &m, sizeof(m));
}

static int send_status_update(int fd, const char *text)
{
    StatusUpdateMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type = MSG_STATUS_UPDATE;
    strncpy(m.text, text, STATUS_TEXT_LEN - 1);
    return write_all(fd, &m, sizeof(m));
}

static void send_player_elim(GameState *gs, int player_id)
{
    PlayerElimMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type  = MSG_PLAYER_ELIM;
    m.player_id = (uint8_t)player_id;
    strncpy(m.name, gs->players[player_id].name, PLAYER_NAME_LEN - 1);
    broadcast(gs, &m, sizeof(m));
}

static void send_game_over(GameState *gs, int is_tie, int winner_id)
{
    GameOverMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type  = MSG_GAME_OVER;
    m.is_tie    = (uint8_t)is_tie;
    m.winner_id = (winner_id >= 0) ? (uint8_t)winner_id : 0xFF;
    if (winner_id >= 0)
        strncpy(m.winner_name, gs->players[winner_id].name, PLAYER_NAME_LEN - 1);
    broadcast(gs, &m, sizeof(m));
}

static int send_error(int fd, const char *msg)
{
    ErrorMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type = MSG_ERROR;
    strncpy(m.message, msg, sizeof(m.message) - 1);
    return write_all(fd, &m, sizeof(m));
}

static int send_wait(int fd)
{
    WaitMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type = MSG_WAIT;
    return write_all(fd, &m, sizeof(m));
}

static int send_join_ack(int fd, int your_id, int is_host)
{
    JoinAckMsg m;
    m.msg_type = MSG_JOIN_ACK;
    m.your_id  = (uint8_t)your_id;
    m.is_host  = (uint8_t)is_host;
    m.padding  = 0;
    return write_all(fd, &m, sizeof(m));
}

/* =========================================================================
 * Disconnect a player
 * ========================================================================= */

static void disconnect_player(GameState *gs, int player_id)
{
    Player *p = &gs->players[player_id];
    if (p->fd == -1) return;

    printf("[server] Player %d (%s) disconnected.\n", player_id, p->name);
    close(p->fd);
    p->fd = -1;

    if (gs->phase == STATE_LOBBY) {
        if (p->char_id >= 0 && p->char_id < NUM_CHARACTERS)
            gs->char_taken[p->char_id] = 0;
        p->alive = 0;
        p->char_id = -1;
        gs->player_count--;
        gs->alive_count--;
        send_lobby_update(gs);
    } else {
        int was_alive = p->alive;
        game_remove_player(gs, player_id);
        if (was_alive)
            send_player_elim(gs, player_id);
    }
}

/* =========================================================================
 * Handle MSG_JOIN
 * ========================================================================= */

static void handle_join(GameState *gs, int fd, const JoinMsg *msg)
{
    if (gs->phase != STATE_LOBBY) {
        send_error(fd, "Game already in progress.");
        close(fd);
        return;
    }

    char name[PLAYER_NAME_LEN];
    strncpy(name, msg->name, PLAYER_NAME_LEN - 1);
    name[PLAYER_NAME_LEN - 1] = '\0';
    for (int i = 0; name[i]; i++)
        if (name[i] < 0x20 || name[i] > 0x7E) name[i] = '?';

    int id = game_add_player(gs, fd, name);
    if (id < 0) {
        send_error(fd, "Lobby is full.");
        close(fd);
        return;
    }

    if (gs->host_id < 0) gs->host_id = id;

    printf("[server] Player %d joined: %s%s\n", id, name,
           id == gs->host_id ? " (host)" : "");
    if (send_join_ack(fd, id, id == gs->host_id) < 0) {
        disconnect_player(gs, id);
        return;
    }
    if (send_char_list(gs, fd) < 0) {
        disconnect_player(gs, id);
        return;
    }
    send_lobby_update(gs);
}

/* =========================================================================
 * Handle MSG_CHAR_SELECT
 * ========================================================================= */

static void handle_char_select(GameState *gs, int player_id,
                               const CharSelectMsg *msg)
{
    Player *p = &gs->players[player_id];
    int cid = msg->char_id;

    if (gs->phase != STATE_LOBBY) {
        if (send_error(p->fd, "Can't change character now.") < 0)
            disconnect_player(gs, player_id);
        return;
    }

    if (game_assign_character(gs, player_id, cid) < 0) {
        if (send_error(p->fd, "Character unavailable or invalid.") < 0)
            disconnect_player(gs, player_id);
        return;
    }

    printf("[server] Player %d picked character %d (%s)\n",
           player_id, cid, p->name);

    /* Broadcast updated lobby and send updated char list to all */
    send_lobby_update(gs);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].fd != -1) {
            if (send_char_list(gs, gs->players[i].fd) < 0)
                disconnect_player(gs, i);
        }
    }
}

/* =========================================================================
 * Handle MSG_ACTION
 * ========================================================================= */

static void handle_action(GameState *gs, int player_id, const ActionMsg *msg)
{
    Player *p = &gs->players[player_id];

    if (gs->phase != STATE_ACTION_COLLECTION) {
        if (send_error(p->fd, "Not currently accepting actions.") < 0)
            disconnect_player(gs, player_id);
        return;
    }
    if (p->pending_action_id != -1) {
        if (send_error(p->fd, "You already submitted an action this round.") < 0)
            disconnect_player(gs, player_id);
        return;
    }

    int aid = msg->action_id;
    int tid = msg->target_id;

    char err[64];
    if (game_validate_action(gs, player_id, aid, tid, err, sizeof(err)) < 0) {
        if (send_error(p->fd, err) < 0)
            disconnect_player(gs, player_id);
        return;
    }

    int req = 0;
    if (p->char_id >= 0) {
        ActionDef actions[MOVES_PER_CHAR];
        int count = 0;
        game_get_char_actions(p->char_id, actions, &count);
        if (aid < count) req = actions[aid].requires_target;
    }

    p->pending_action_id = aid;
    p->pending_target_id = (req > 0) ? tid : NO_TARGET;

    if (send_wait(p->fd) < 0)
        disconnect_player(gs, player_id);
    printf("[server] Player %d (%s) chose action %d target %d\n",
           player_id, p->name, aid, p->pending_target_id);
}

/* =========================================================================
 * Handle MSG_START_GAME
 * ========================================================================= */

static void handle_start_game(GameState *gs, int player_id)
{
    if (player_id != gs->host_id) {
        if (send_error(gs->players[player_id].fd, "Only the host can start.") < 0)
            disconnect_player(gs, player_id);
        return;
    }
    if (gs->phase != STATE_LOBBY) {
        if (send_error(gs->players[player_id].fd, "Game already started.") < 0)
            disconnect_player(gs, player_id);
        return;
    }

    /* Check all players have selected characters */
    int ready = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].fd != -1 && gs->players[i].name[0] != '\0') {
            if (gs->players[i].char_id < 0) {
                if (send_error(gs->players[player_id].fd,
                           "All players must select a character first.") < 0)
                    disconnect_player(gs, player_id);
                return;
            }
            ready++;
        }
    }
    if (ready < MIN_PLAYERS) {
        if (send_error(gs->players[player_id].fd,
                   "Need at least 2 players to start.") < 0)
            disconnect_player(gs, player_id);
        return;
    }

    printf("[server] Host started the game with %d players.\n", ready);
    send_game_start(gs);
    begin_round(gs);
    last_tick = time(NULL);
}

/* =========================================================================
 * Process player buffer
 * ========================================================================= */

static void process_player_buffer(GameState *gs, int player_id)
{
    Player *p = &gs->players[player_id];

    while (p->rbuf_len > 0) {
        uint8_t opcode = p->rbuf[0];
        size_t needed = 0;

        switch (opcode) {
        case MSG_JOIN:        needed = sizeof(JoinMsg);       break;
        case MSG_ACTION:      needed = sizeof(ActionMsg);     break;
        case MSG_START_GAME:  needed = sizeof(StartGameMsg);  break;
        case MSG_CHAR_SELECT: needed = sizeof(CharSelectMsg); break;
        default:
            fprintf(stderr, "[server] Unknown opcode 0x%02x from player %d\n",
                    opcode, player_id);
            disconnect_player(gs, player_id);
            return;
        }

        if ((size_t)p->rbuf_len < needed) break;

        switch (opcode) {
        case MSG_JOIN:
            handle_join(gs, p->fd, (const JoinMsg *)p->rbuf);
            break;
        case MSG_ACTION:
            handle_action(gs, player_id, (const ActionMsg *)p->rbuf);
            break;
        case MSG_START_GAME:
            handle_start_game(gs, player_id);
            break;
        case MSG_CHAR_SELECT:
            handle_char_select(gs, player_id, (const CharSelectMsg *)p->rbuf);
            break;
        }

        int remaining = p->rbuf_len - (int)needed;
        if (remaining > 0)
            memmove(p->rbuf, p->rbuf + needed, (size_t)remaining);
        p->rbuf_len = remaining;
    }
}

/* =========================================================================
 * Start a new round
 * ========================================================================= */

static void begin_round(GameState *gs)
{
    gs->round_num++;

    /* Tick status effects */
    char tick_text[EVENT_TEXT_LEN];
    game_tick_effects(gs, tick_text, sizeof(tick_text));

    /* Send tick events if any */
    if (tick_text[0] != '\0')
        send_round_event(gs, tick_text);

    /* Check if anyone died from ticks */
    int winner_id = -1;
    int end = game_check_end(gs, &winner_id);
    if (end != 0) {
        /* Send eliminations */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!gs->players[i].alive && gs->players[i].fd != -1
                && gs->players[i].hp <= 0)
                send_player_elim(gs, i);
        }
        gs->phase = STATE_GAME_OVER;
        send_game_over(gs, end == 2, winner_id);
        printf("[server] Game over from ticks. %s\n",
               end == 2 ? "Tie!" : gs->players[winner_id].name);
        return;
    }

    gs->phase = STATE_ACTION_COLLECTION;
    gs->timer_secs_left = ROUND_TIMER_SECS;
    game_clear_actions(gs);
    printf("[server] --- Round %d ---\n", gs->round_num);

    send_round_start(gs);

    /* Send status updates to each alive player */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &gs->players[i];
        if (p->alive && p->fd != -1) {
            char status_text[STATUS_TEXT_LEN];
            game_build_status_text(gs, i, status_text, sizeof(status_text));
            if (send_status_update(p->fd, status_text) < 0)
                disconnect_player(gs, i);
        }
    }
}

/* =========================================================================
 * Start resolution (enter STATE_RESOLVING)
 * ========================================================================= */

static void start_resolution(GameState *gs)
{
    gs->phase = STATE_RESOLVING;
    game_resolve_round(gs);
    gs->resolve_event_current = 0;
    gettimeofday(&resolve_last_send, NULL);

    /* Send first event immediately */
    if (gs->resolve_event_current < gs->resolve_event_count) {
        send_round_event(gs, gs->resolve_events[gs->resolve_event_current]);
        gs->resolve_event_current++;
        gettimeofday(&resolve_last_send, NULL);
        printf("[server] Event %d/%d sent\n",
               gs->resolve_event_current, gs->resolve_event_count);
    } else {
        finish_resolution(gs);
    }
}

/* =========================================================================
 * Finish resolution (after all events sent)
 * ========================================================================= */

static void finish_resolution(GameState *gs)
{
    /* Send eliminations */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!gs->players[i].alive && gs->players[i].fd != -1
            && gs->players[i].hp <= 0)
            send_player_elim(gs, i);
    }

    int winner_id = -1;
    int end = game_check_end(gs, &winner_id);

    if (end == 0) {
        begin_round(gs);
        last_tick = time(NULL);
    } else {
        gs->phase = STATE_GAME_OVER;
        send_game_over(gs, end == 2, winner_id);
        printf("[server] Game over. %s\n",
               end == 2 ? "Tie!" : gs->players[winner_id].name);
    }
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void)
{
    srand((unsigned)time(NULL));

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(listen_fd, MAX_PLAYERS) < 0) { perror("listen"); exit(1); }

    printf("[server] Listening on port %d\n", PORT);

    GameState gs;
    game_init(&gs);

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = listen_fd;
        FD_SET(listen_fd, &rfds);

        for (int i = 0; i < MAX_PLAYERS; i++) {
            int fd = gs.players[i].fd;
            if (fd != -1) {
                FD_SET(fd, &rfds);
                if (fd > maxfd) maxfd = fd;
            }
        }

        /* Timeout logic */
        struct timeval tv;
        struct timeval *tvp = NULL;

        if (gs.phase == STATE_ACTION_COLLECTION) {
            tv.tv_sec  = 1;
            tv.tv_usec = 0;
            tvp = &tv;
        } else if (gs.phase == STATE_RESOLVING) {
            /* Calculate time until next event */
            struct timeval now;
            gettimeofday(&now, NULL);
            long elapsed_us = (now.tv_sec - resolve_last_send.tv_sec) * 1000000L
                            + (now.tv_usec - resolve_last_send.tv_usec);
            long remaining_us = RESOLVE_DELAY_US - elapsed_us;
            if (remaining_us < 0) remaining_us = 0;
            tv.tv_sec  = remaining_us / 1000000;
            tv.tv_usec = remaining_us % 1000000;
            tvp = &tv;
        }

        int ready = select(maxfd + 1, &rfds, NULL, NULL, tvp);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select"); exit(1);
        }

        /* --- Timer tick (action collection) --- */
        if (gs.phase == STATE_ACTION_COLLECTION) {
            time_t now = time(NULL);
            if (ready == 0 || now != last_tick) {
                if (now != last_tick) {
                    last_tick = now;
                    gs.timer_secs_left--;
                    send_timer_tick(&gs, gs.timer_secs_left);
                    printf("[server] Timer: %d seconds left\n",
                           gs.timer_secs_left);
                }
                if (gs.timer_secs_left <= 0) {
                    printf("[server] Time expired, resolving round.\n");
                    start_resolution(&gs);
                    continue;
                }
            }
            if (ready == 0) continue;
        }

        /* --- Resolve event dispatch --- */
        if (gs.phase == STATE_RESOLVING) {
            struct timeval now;
            gettimeofday(&now, NULL);
            long elapsed_us = (now.tv_sec - resolve_last_send.tv_sec) * 1000000L
                            + (now.tv_usec - resolve_last_send.tv_usec);

            if (elapsed_us >= RESOLVE_DELAY_US) {
                if (gs.resolve_event_current < gs.resolve_event_count) {
                    send_round_event(&gs,
                        gs.resolve_events[gs.resolve_event_current]);
                    gs.resolve_event_current++;
                    gettimeofday(&resolve_last_send, NULL);
                    printf("[server] Event %d/%d sent\n",
                           gs.resolve_event_current,
                           gs.resolve_event_count);
                } else {
                    finish_resolution(&gs);
                    continue;
                }
            }
            if (ready == 0) continue;
        }

        /* --- Accept new connections --- */
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            int new_fd = accept(listen_fd,
                                (struct sockaddr *)&client_addr, &addrlen);
            if (new_fd >= 0) {
                if (gs.phase != STATE_LOBBY ||
                    gs.player_count >= MAX_PLAYERS) {
                    send_error(new_fd, "Cannot join: lobby full or game started.");
                    close(new_fd);
                } else {
                    int assigned = 0;
                    for (int i = 0; i < MAX_PLAYERS; i++) {
                        if (gs.players[i].fd == -1) {
                            gs.players[i].fd       = new_fd;
                            gs.players[i].alive    = 0;
                            gs.players[i].rbuf_len = 0;
                            memset(gs.players[i].name, 0, PLAYER_NAME_LEN);
                            assigned = 1;
                            printf("[server] New connection fd %d (slot %d)\n",
                                   new_fd, i);
                            break;
                        }
                    }
                    if (!assigned) close(new_fd);
                }
            }
        }

        /* --- Read from connected players --- */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            int fd = gs.players[i].fd;
            if (fd == -1 || !FD_ISSET(fd, &rfds)) continue;

            Player *p = &gs.players[i];
            int space = (int)(sizeof(p->rbuf)) - p->rbuf_len;
            ssize_t n = read(fd, p->rbuf + p->rbuf_len, (size_t)space);

            if (n <= 0) {
                disconnect_player(&gs, i);
                continue;
            }
            p->rbuf_len += (int)n;
            process_player_buffer(&gs, i);
            if (gs.players[i].fd == -1) continue;
        }

        /* --- Check if all actions collected --- */
        if (gs.phase == STATE_ACTION_COLLECTION && game_all_actions_in(&gs)) {
            printf("[server] All actions received, resolving round.\n");
            start_resolution(&gs);
        }

        /* --- Game over, all disconnected --- */
        if (gs.phase == STATE_GAME_OVER) {
            int still = 0;
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (gs.players[i].fd != -1) still++;
            if (still == 0) {
                printf("[server] All players disconnected. Shutting down.\n");
                break;
            }
        }
    }

    close(listen_fd);
    return 0;
}
