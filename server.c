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
#include <time.h>

#include "protocol.h"
#include "game.h"

#ifndef PORT
#define PORT 4242
#endif

#define MIN_PLAYERS 2

/* =========================================================================
 * Utility: write an entire buffer, returning -1 on failure
 * ========================================================================= */

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0) return -1;
        p         += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* =========================================================================
 * Broadcast helpers
 * ========================================================================= */

static void broadcast(GameState *gs, const void *msg, size_t len)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].fd != -1) {
            if (write_all(gs->players[i].fd, msg, len) < 0) {
                /* Will be caught on next select() read */
            }
        }
    }
}

static void send_lobby_update(GameState *gs)
{
    LobbyUpdateMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type     = MSG_LOBBY_UPDATE;
    m.player_count = (uint8_t)gs->player_count;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        m.players[i].player_id = gs->players[i].id;
        m.players[i].hp        = (uint8_t)gs->players[i].hp;
        m.players[i].alive     = (uint8_t)gs->players[i].alive;
        strncpy(m.players[i].name, gs->players[i].name, PLAYER_NAME_LEN - 1);
    }
    broadcast(gs, &m, sizeof(m));
}

static void send_game_start(GameState *gs)
{
    /* Send personalised GameStartMsg to each player (your_id differs) */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].fd == -1) continue;

        GameStartMsg m;
        memset(&m, 0, sizeof(m));
        m.msg_type     = MSG_GAME_START;
        m.your_id      = (uint8_t)i;
        m.player_count = (uint8_t)gs->player_count;
        m.action_count = (uint8_t)gs->action_count;

        for (int j = 0; j < MAX_PLAYERS; j++) {
            m.players[j].player_id = gs->players[j].id;
            m.players[j].hp        = (uint8_t)gs->players[j].hp;
            m.players[j].alive     = (uint8_t)gs->players[j].alive;
            strncpy(m.players[j].name, gs->players[j].name, PLAYER_NAME_LEN - 1);
        }
        for (int j = 0; j < gs->action_count; j++) {
            m.actions[j] = gs->action_defs[j];
        }

        write_all(gs->players[i].fd, &m, sizeof(m));
    }
}

static void send_round_start(GameState *gs)
{
    RoundStartMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type   = MSG_ROUND_START;
    m.round_num  = (uint8_t)gs->round_num;
    m.timer_secs = ROUND_TIMER_SECS;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        m.players[i].player_id = gs->players[i].id;
        m.players[i].hp        = (uint8_t)(gs->players[i].hp < 0 ? 0 : gs->players[i].hp);
        m.players[i].alive     = (uint8_t)gs->players[i].alive;
        strncpy(m.players[i].name, gs->players[i].name, PLAYER_NAME_LEN - 1);
    }
    broadcast(gs, &m, sizeof(m));
}

static void send_timer_tick(GameState *gs, int seconds_left)
{
    TimerTickMsg m;
    m.msg_type    = MSG_TIMER_TICK;
    m.seconds_left = (uint8_t)seconds_left;
    m.padding[0]  = 0;
    m.padding[1]  = 0;
    broadcast(gs, &m, sizeof(m));
}

static void send_round_result(GameState *gs, ActionResult *results, int count)
{
    RoundResultMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type     = MSG_ROUND_RESULT;
    m.result_count = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        m.results[i] = results[i];
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        m.players[i].player_id = gs->players[i].id;
        m.players[i].hp        = (uint8_t)(gs->players[i].hp < 0 ? 0 : gs->players[i].hp);
        m.players[i].alive     = (uint8_t)gs->players[i].alive;
        strncpy(m.players[i].name, gs->players[i].name, PLAYER_NAME_LEN - 1);
    }
    broadcast(gs, &m, sizeof(m));
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
    if (winner_id >= 0) {
        strncpy(m.winner_name, gs->players[winner_id].name, PLAYER_NAME_LEN - 1);
    }
    broadcast(gs, &m, sizeof(m));
}

static void send_error(int fd, const char *msg)
{
    ErrorMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type = MSG_ERROR;
    strncpy(m.message, msg, sizeof(m.message) - 1);
    write_all(fd, &m, sizeof(m));
}

static void send_wait(int fd)
{
    WaitMsg m;
    m.msg_type  = MSG_WAIT;
    m.padding[0] = 0;
    m.padding[1] = 0;
    m.padding[2] = 0;
    write_all(fd, &m, sizeof(m));
}

/* =========================================================================
 * Disconnect a player cleanly
 * ========================================================================= */

static void disconnect_player(GameState *gs, int player_id)
{
    Player *p = &gs->players[player_id];
    if (p->fd == -1) return;

    printf("[server] Player %d (%s) disconnected.\n", player_id, p->name);
    close(p->fd);
    p->fd = -1;

    if (gs->phase == STATE_LOBBY) {
        p->alive = 0;
        gs->player_count--;
        gs->alive_count--;
        send_lobby_update(gs);
    } else {
        /* In-game: treat as elimination */
        int was_alive = p->alive;
        game_remove_player(gs, player_id);
        if (was_alive) {
            send_player_elim(gs, player_id);
        }
    }
}

/* =========================================================================
 * Handle an incoming MSG_JOIN
 * ========================================================================= */

static void handle_join(GameState *gs, int fd, const JoinMsg *msg)
{
    if (gs->phase != STATE_LOBBY) {
        /* Game already started — reject */
        send_error(fd, "Game already in progress.");
        close(fd);
        return;
    }

    char name[PLAYER_NAME_LEN];
    strncpy(name, msg->name, PLAYER_NAME_LEN - 1);
    name[PLAYER_NAME_LEN - 1] = '\0';
    /* Strip non-printable chars */
    for (int i = 0; name[i]; i++) {
        if (name[i] < 0x20 || name[i] > 0x7E) name[i] = '?';
    }

    int id = game_add_player(gs, fd, name);
    if (id < 0) {
        send_error(fd, "Lobby is full.");
        close(fd);
        return;
    }
    printf("[server] Player %d joined: %s\n", id, name);
    send_lobby_update(gs);
}

/* =========================================================================
 * Handle an incoming MSG_ACTION
 * ========================================================================= */

static void handle_action(GameState *gs, int player_id, const ActionMsg *msg)
{
    Player *p = &gs->players[player_id];

    if (gs->phase != STATE_ACTION_COLLECTION) {
        send_error(p->fd, "Not currently accepting actions.");
        return;
    }
    if (p->pending_action_id != -1) {
        send_error(p->fd, "You already submitted an action this round.");
        return;
    }

    int aid = msg->action_id;
    if (aid < 0 || aid >= gs->action_count) {
        send_error(p->fd, "Invalid action ID.");
        return;
    }

    /* Validate target */
    if (gs->action_defs[aid].requires_target) {
        int tid = msg->target_id;
        if (tid == NO_TARGET || tid >= MAX_PLAYERS
                || !gs->players[tid].alive || tid == player_id) {
            send_error(p->fd, "Invalid or missing target.");
            return;
        }
        p->pending_target_id = tid;
    } else {
        p->pending_target_id = NO_TARGET;
    }

    p->pending_action_id = aid;
    send_wait(p->fd);
    printf("[server] Player %d (%s) chose action %d target %d\n",
           player_id, p->name, aid, p->pending_target_id);
}

/* =========================================================================
 * Process buffered bytes for one player, dispatching complete messages
 * ========================================================================= */

static void process_player_buffer(GameState *gs, int player_id)
{
    Player *p = &gs->players[player_id];

    while (p->rbuf_len > 0) {
        uint8_t opcode = p->rbuf[0];
        size_t  needed = 0;

        switch (opcode) {
            case MSG_JOIN:   needed = sizeof(JoinMsg);   break;
            case MSG_ACTION: needed = sizeof(ActionMsg); break;
            default:
                /* Unknown opcode — drop connection */
                fprintf(stderr, "[server] Unknown opcode 0x%02x from player %d\n",
                        opcode, player_id);
                disconnect_player(gs, player_id);
                return;
        }

        if ((size_t)p->rbuf_len < needed) {
            break; /* Need more bytes */
        }

        /* Dispatch */
        switch (opcode) {
            case MSG_JOIN:
                handle_join(gs, p->fd, (const JoinMsg *)p->rbuf);
                break;
            case MSG_ACTION:
                handle_action(gs, player_id, (const ActionMsg *)p->rbuf);
                break;
        }

        /* Consume the message from the buffer */
        int remaining = p->rbuf_len - (int)needed;
        if (remaining > 0) {
            memmove(p->rbuf, p->rbuf + needed, (size_t)remaining);
        }
        p->rbuf_len = remaining;
    }
}

/* =========================================================================
 * Start a new round
 * ========================================================================= */

static void begin_round(GameState *gs)
{
    gs->round_num++;
    gs->phase            = STATE_ACTION_COLLECTION;
    gs->timer_secs_left  = ROUND_TIMER_SECS;
    game_clear_actions(gs);
    printf("[server] --- Round %d ---\n", gs->round_num);
    send_round_start(gs);
}

/* =========================================================================
 * Resolve round and advance state
 * ========================================================================= */

static void resolve_round(GameState *gs)
{
    gs->phase = STATE_CALCULATION;

    ActionResult results[MAX_PLAYERS];
    int count = game_resolve_round(gs, results, MAX_PLAYERS);

    /* Announce eliminations */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].hp <= 0 && !gs->players[i].alive
                && gs->players[i].fd != -1) {
            send_player_elim(gs, i);
        }
    }

    send_round_result(gs, results, count);

    int winner_id = -1;
    int end = game_check_end(gs, &winner_id);

    if (end == 0) {
        begin_round(gs);
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
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

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

    /* Track the last time we sent a timer tick */
    time_t last_tick = 0;

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

        /* Use 1-second timeout during action collection for timer ticks */
        struct timeval tv;
        struct timeval *tvp = NULL;
        if (gs.phase == STATE_ACTION_COLLECTION) {
            tv.tv_sec  = 1;
            tv.tv_usec = 0;
            tvp = &tv;
        }

        int ready = select(maxfd + 1, &rfds, NULL, NULL, tvp);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select"); exit(1);
        }

        /* --- Timer tick handling --- */
        if (gs.phase == STATE_ACTION_COLLECTION) {
            time_t now = time(NULL);
            if (ready == 0 || now != last_tick) {
                if (now != last_tick) {
                    last_tick = now;
                    gs.timer_secs_left--;
                    send_timer_tick(&gs, gs.timer_secs_left);
                    printf("[server] Timer: %d seconds left\n", gs.timer_secs_left);
                }
                if (gs.timer_secs_left <= 0) {
                    printf("[server] Time expired, resolving round.\n");
                    resolve_round(&gs);
                    last_tick = 0;
                    continue;
                }
            }
            if (ready == 0) continue;
        }

        /* --- Accept new connections --- */
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            int new_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addrlen);
            if (new_fd >= 0) {
                if (gs.phase != STATE_LOBBY || gs.player_count >= MAX_PLAYERS) {
                    send_error(new_fd, "Cannot join: lobby full or game started.");
                    close(new_fd);
                } else {
                    /*
                     * We don't know the player's name yet.
                     * Temporarily store the fd in a player slot
                     * so we can receive their MSG_JOIN.
                     * Assign to a free slot with an empty name.
                     */
                    int assigned = 0;
                    for (int i = 0; i < MAX_PLAYERS; i++) {
                        if (gs.players[i].fd == -1) {
                            gs.players[i].fd      = new_fd;
                            gs.players[i].alive   = 0;
                            gs.players[i].rbuf_len = 0;
                            memset(gs.players[i].name, 0, PLAYER_NAME_LEN);
                            assigned = 1;
                            printf("[server] New connection on fd %d (slot %d)\n", new_fd, i);
                            break;
                        }
                    }
                    if (!assigned) {
                        close(new_fd);
                    }
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

            if (gs.players[i].fd == -1) continue; /* was disconnected */
        }

        /* --- Auto-start game when minimum players ready --- */
        if (gs.phase == STATE_LOBBY && gs.player_count >= MIN_PLAYERS) {
            /* Check all slots have sent MSG_JOIN (name non-empty) */
            int ready_players = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (gs.players[i].fd != -1 && gs.players[i].name[0] != '\0') {
                    ready_players++;
                }
            }
            /* Only start if we've had no new connections for a moment —
             * use a simple heuristic: start when all current slots are named */
            int unnamed = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (gs.players[i].fd != -1 && gs.players[i].name[0] == '\0') {
                    unnamed++;
                }
            }
            if (ready_players >= MIN_PLAYERS && unnamed == 0) {
                printf("[server] Starting game with %d players.\n", ready_players);
                send_game_start(&gs);
                begin_round(&gs);
                last_tick = time(NULL);
            }
        }

        /* --- Check if all actions collected (no timer needed) --- */
        if (gs.phase == STATE_ACTION_COLLECTION && game_all_actions_in(&gs)) {
            printf("[server] All actions received, resolving round.\n");
            resolve_round(&gs);
            last_tick = 0;
        }

        /* --- Check if game is over and everyone disconnected --- */
        if (gs.phase == STATE_GAME_OVER) {
            int still_connected = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (gs.players[i].fd != -1) still_connected++;
            }
            if (still_connected == 0) {
                printf("[server] All players disconnected. Shutting down.\n");
                break;
            }
        }
    }

    close(listen_fd);
    return 0;
}
