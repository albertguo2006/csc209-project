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
#include <fcntl.h>
#include <signal.h>

#include "protocol.h"
#include "game.h"

#ifndef PORT
#define PORT 4242
#endif

#define MIN_PLAYERS     2
#define MAX_ROOMS       64
#define RESOLVE_DELAY_US 2000000  /* 2.0 seconds between events */

/* =========================================================================
 * Non-blocking I/O helpers
 * ========================================================================= */

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Queue data into a player's write buffer.  Returns -1 if the buffer
 * is full (caller should disconnect the player). */
static int queue_write(Player *p, const void *buf, size_t len)
{
    if (p->fd == -1) return -1;
    if (p->wbuf_len + (int)len > (int)sizeof(p->wbuf)) return -1;
    memcpy(p->wbuf + p->wbuf_len, buf, len);
    p->wbuf_len += (int)len;
    return 0;
}

/* Try to flush as much of the write buffer as the kernel will accept.
 * Returns  0 on success or EAGAIN (partial flush),
 *         -1 on fatal error (caller should disconnect). */
static int flush_wbuf(Player *p)
{
    while (p->wbuf_len > 0) {
        ssize_t n = write(p->fd, p->wbuf, (size_t)p->wbuf_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (n == 0) return -1;
        int remaining = p->wbuf_len - (int)n;
        if (remaining > 0)
            memmove(p->wbuf, p->wbuf + n, (size_t)remaining);
        p->wbuf_len = remaining;
    }
    return 0;
}

/* Blocking write for fds that have no Player slot (e.g. rejected connections) */
static int write_direct(int fd, const void *buf, size_t len)
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

static time_t         *last_tick        = NULL;
static struct timeval  *resolve_last_send = NULL;

/* =========================================================================
 * Pending connections (clients choosing a room before joining)
 * ========================================================================= */

#define MAX_PENDING 64

typedef struct {
    int     fd;
    uint8_t rbuf[256];
    int     rbuf_len;
    uint8_t wbuf[8192];
    int     wbuf_len;
} PendingConn;

static PendingConn *pending_conns = NULL;
static int          num_rooms_g   = 0;

static int queue_write_pending(PendingConn *pc, const void *buf, size_t len)
{
    if (pc->fd == -1) return -1;
    if (pc->wbuf_len + (int)len > (int)sizeof(pc->wbuf)) return -1;
    memcpy(pc->wbuf + pc->wbuf_len, buf, len);
    pc->wbuf_len += (int)len;
    return 0;
}

static int flush_wbuf_pending(PendingConn *pc)
{
    while (pc->wbuf_len > 0) {
        ssize_t n = write(pc->fd, pc->wbuf, (size_t)pc->wbuf_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (n == 0) return -1;
        int remaining = pc->wbuf_len - (int)n;
        if (remaining > 0) memmove(pc->wbuf, pc->wbuf + n, (size_t)remaining);
        pc->wbuf_len = remaining;
    }
    return 0;
}

static void disconnect_pending(PendingConn *pc)
{
    if (pc->fd == -1) return;
    flush_wbuf_pending(pc);
    close(pc->fd);
    pc->fd = -1;
    pc->rbuf_len = 0;
    pc->wbuf_len = 0;
}

/* =========================================================================
 * Spectator I/O helpers
 * ========================================================================= */

static int queue_write_spectator(Spectator *sp, const void *buf, size_t len)
{
    if (sp->fd == -1) return -1;
    if (sp->wbuf_len + (int)len > (int)sizeof(sp->wbuf)) return -1;
    memcpy(sp->wbuf + sp->wbuf_len, buf, len);
    sp->wbuf_len += (int)len;
    return 0;
}

static int flush_wbuf_spectator(Spectator *sp)
{
    while (sp->wbuf_len > 0) {
        ssize_t n = write(sp->fd, sp->wbuf, (size_t)sp->wbuf_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (n == 0) return -1;
        int remaining = sp->wbuf_len - (int)n;
        if (remaining > 0) memmove(sp->wbuf, sp->wbuf + n, (size_t)remaining);
        sp->wbuf_len = remaining;
    }
    return 0;
}

static void disconnect_spectator(GameState *gs, int spec_id)
{
    Spectator *sp = &gs->spectators[spec_id];
    if (sp->fd == -1) return;
    printf("[server] Spectator %s disconnected from room %d.\n",
           sp->name, gs->room_idx);
    flush_wbuf_spectator(sp);
    close(sp->fd);
    sp->fd = -1;
    sp->rbuf_len = 0;
    sp->wbuf_len = 0;
    gs->spectator_count--;
}

/* Forward declarations */
static void disconnect_player(GameState *gs, int player_id);
static void send_game_start(GameState *gs);
static void begin_round(GameState *gs);
static void finish_resolution(GameState *gs);
static void promote_spectators(GameState *gs);
static void handle_join_room(GameState *rooms, int num_rooms, PendingConn *pc,
                             const JoinRoomMsg *msg);
static void process_pending_buffer(PendingConn *pc, GameState *rooms,
                                   int num_rooms);

/* =========================================================================
 * Broadcast helpers
 * ========================================================================= */

static void broadcast(GameState *gs, const void *msg, size_t len)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].fd != -1) {
            if (queue_write(&gs->players[i], msg, len) < 0)
                disconnect_player(gs, i);
        }
    }
}

/* Send to spectators only (game events they can watch) */
static void broadcast_spectators(GameState *gs, const void *msg, size_t len)
{
    for (int s = 0; s < MAX_SPECTATORS; s++) {
        if (gs->spectators[s].fd != -1) {
            if (queue_write_spectator(&gs->spectators[s], msg, len) < 0)
                disconnect_spectator(gs, s);
        }
    }
}

/* Send to both players and spectators */
static void broadcast_all(GameState *gs, const void *msg, size_t len)
{
    broadcast(gs, msg, len);
    broadcast_spectators(gs, msg, len);
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

static int send_char_list(GameState *gs, Player *p)
{
    CharListMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type   = MSG_CHAR_LIST;
    m.char_count = NUM_CHARACTERS;
    game_build_char_list(gs, m.chars);
    return queue_write(p, &m, sizeof(m));
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
        if (queue_write(p, &m, sizeof(m)) < 0)
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
    broadcast_all(gs, &m, sizeof(m));
}

static void send_timer_tick(GameState *gs, int seconds_left)
{
    TimerTickMsg m;
    m.msg_type     = MSG_TIMER_TICK;
    m.seconds_left = (uint8_t)seconds_left;
    m.padding[0]   = 0;
    m.padding[1]   = 0;
    broadcast_all(gs, &m, sizeof(m));
}

static void send_round_event(GameState *gs, const char *text)
{
    RoundEventMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type = MSG_ROUND_EVENT;
    strncpy(m.text, text, EVENT_TEXT_LEN - 1);
    broadcast_all(gs, &m, sizeof(m));
}

static int send_status_update(Player *p, const char *text)
{
    StatusUpdateMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type = MSG_STATUS_UPDATE;
    strncpy(m.text, text, STATUS_TEXT_LEN - 1);
    return queue_write(p, &m, sizeof(m));
}

static void send_player_elim(GameState *gs, int player_id)
{
    PlayerElimMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type  = MSG_PLAYER_ELIM;
    m.player_id = (uint8_t)player_id;
    strncpy(m.name, gs->players[player_id].name, PLAYER_NAME_LEN - 1);
    broadcast_all(gs, &m, sizeof(m));
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
    broadcast_all(gs, &m, sizeof(m));
}

static int send_error(Player *p, const char *msg)
{
    ErrorMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type = MSG_ERROR;
    strncpy(m.message, msg, sizeof(m.message) - 1);
    return queue_write(p, &m, sizeof(m));
}

static int send_wait(Player *p)
{
    WaitMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type = MSG_WAIT;
    return queue_write(p, &m, sizeof(m));
}

static int send_join_ack(Player *p, int your_id, int is_host, int room_id)
{
    JoinAckMsg m;
    m.msg_type = MSG_JOIN_ACK;
    m.your_id  = (uint8_t)your_id;
    m.is_host  = (uint8_t)is_host;
    m.room_id  = (uint8_t)room_id;
    return queue_write(p, &m, sizeof(m));
}

/* =========================================================================
 * Room list helpers
 * ========================================================================= */

static void build_room_list_msg(GameState *rooms, int num_rooms, RoomListMsg *m)
{
    memset(m, 0, sizeof(*m));
    m->msg_type   = MSG_ROOM_LIST;
    int count = (num_rooms < ROOM_LIST_MAX) ? num_rooms : ROOM_LIST_MAX;
    m->room_count = (uint8_t)count;
    for (int r = 0; r < count; r++) {
        m->rooms[r].room_id        = (uint8_t)r;
        m->rooms[r].player_count   = (uint8_t)rooms[r].player_count;
        m->rooms[r].phase          = (uint8_t)rooms[r].phase;
        m->rooms[r].spectator_count = (uint8_t)rooms[r].spectator_count;
    }
}

static void send_spectate_start(Spectator *sp, GameState *gs)
{
    SpectateStartMsg m;
    memset(&m, 0, sizeof(m));
    m.msg_type     = MSG_SPECTATE_START;
    m.player_count = (uint8_t)gs->player_count;
    m.round_num    = (uint8_t)gs->round_num;
    fill_player_info(gs, m.players);
    queue_write_spectator(sp, &m, sizeof(m));
}

/* =========================================================================
 * Promote spectators to lobby players after a game ends
 * ========================================================================= */

static void promote_spectators(GameState *gs)
{
    for (int s = 0; s < MAX_SPECTATORS; s++) {
        Spectator *sp = &gs->spectators[s];
        if (sp->fd == -1) continue;

        /* Find empty player slot */
        int slot = -1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (gs->players[i].fd == -1) { slot = i; break; }
        }

        if (slot < 0) {
            /* Room is full; send error and disconnect spectator */
            ErrorMsg em;
            memset(&em, 0, sizeof(em));
            em.msg_type = MSG_ERROR;
            strncpy(em.message, "Lobby is full; cannot join.",
                    sizeof(em.message) - 1);
            queue_write_spectator(sp, &em, sizeof(em));
            flush_wbuf_spectator(sp);
            close(sp->fd);
            sp->fd = -1;
            sp->rbuf_len = 0;
            sp->wbuf_len = 0;
            gs->spectator_count--;
            continue;
        }

        /* Transfer fd and buffers to player slot */
        Player *p = &gs->players[slot];
        p->fd       = sp->fd;
        p->rbuf_len = sp->rbuf_len;
        memcpy(p->rbuf, sp->rbuf, (size_t)sp->rbuf_len);
        p->wbuf_len = sp->wbuf_len;
        memcpy(p->wbuf, sp->wbuf, (size_t)sp->wbuf_len);

        char name[PLAYER_NAME_LEN];
        strncpy(name, sp->name, PLAYER_NAME_LEN - 1);
        name[PLAYER_NAME_LEN - 1] = '\0';

        /* Clear spectator slot */
        sp->fd = -1;
        sp->rbuf_len = 0;
        sp->wbuf_len = 0;
        gs->spectator_count--;

        printf("[server] Spectator %s promoted to player in room %d slot %d\n",
               name, gs->room_idx, slot);

        int id = game_add_player(gs, p->fd, name);
        if (id < 0) {
            close(p->fd);
            p->fd = -1;
            continue;
        }

        if (gs->host_id < 0) gs->host_id = id;

        if (send_join_ack(p, id, id == gs->host_id, gs->room_idx) < 0) {
            disconnect_player(gs, id);
            continue;
        }
        if (send_char_list(gs, p) < 0) {
            disconnect_player(gs, id);
            continue;
        }
    }
}

/* =========================================================================
 * Handle MSG_JOIN_ROOM from a pending connection
 * ========================================================================= */

static void handle_join_room(GameState *rooms, int num_rooms, PendingConn *pc,
                             const JoinRoomMsg *msg)
{
    /* Sanitize name */
    char name[PLAYER_NAME_LEN];
    strncpy(name, msg->name, PLAYER_NAME_LEN - 1);
    name[PLAYER_NAME_LEN - 1] = '\0';
    for (int i = 0; name[i]; i++)
        if (name[i] < 0x20 || name[i] > 0x7E) name[i] = '?';
    if (name[0] == '\0') strncpy(name, "Player", PLAYER_NAME_LEN - 1);

    int room_id = msg->room_id;

    /* AUTO_ROOM: find first room with no players (truly empty), else any lobby */
    if (room_id == AUTO_ROOM) {
        room_id = -1;
        for (int r = 0; r < num_rooms; r++) {
            if (rooms[r].player_count == 0 && rooms[r].phase == STATE_LOBBY) {
                room_id = r; break;
            }
        }
        if (room_id < 0) {
            for (int r = 0; r < num_rooms; r++) {
                if (rooms[r].phase == STATE_LOBBY &&
                    rooms[r].player_count < MAX_PLAYERS) {
                    room_id = r; break;
                }
            }
        }
        if (room_id < 0) {
            ErrorMsg em;
            memset(&em, 0, sizeof(em));
            em.msg_type = MSG_ERROR;
            strncpy(em.message, "No empty rooms available. Try joining an existing one.",
                    sizeof(em.message) - 1);
            queue_write_pending(pc, &em, sizeof(em));
            RoomListMsg rlm;
            build_room_list_msg(rooms, num_rooms, &rlm);
            queue_write_pending(pc, &rlm, sizeof(rlm));
            return;
        }
    }

    if (room_id < 0 || room_id >= num_rooms) {
        ErrorMsg em;
        memset(&em, 0, sizeof(em));
        em.msg_type = MSG_ERROR;
        strncpy(em.message, "Invalid room ID.", sizeof(em.message) - 1);
        queue_write_pending(pc, &em, sizeof(em));
        RoomListMsg rlm;
        build_room_list_msg(rooms, num_rooms, &rlm);
        queue_write_pending(pc, &rlm, sizeof(rlm));
        return;
    }

    GameState *gs = &rooms[room_id];

    if (gs->phase == STATE_LOBBY) {
        /* --- Join as a regular player --- */
        if (gs->player_count >= MAX_PLAYERS) {
            ErrorMsg em;
            memset(&em, 0, sizeof(em));
            em.msg_type = MSG_ERROR;
            strncpy(em.message, "Room is full.", sizeof(em.message) - 1);
            queue_write_pending(pc, &em, sizeof(em));
            RoomListMsg rlm;
            build_room_list_msg(rooms, num_rooms, &rlm);
            queue_write_pending(pc, &rlm, sizeof(rlm));
            return;
        }

        /* Find empty player slot */
        int slot = -1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (gs->players[i].fd == -1) { slot = i; break; }
        }
        if (slot < 0) {
            ErrorMsg em;
            memset(&em, 0, sizeof(em));
            em.msg_type = MSG_ERROR;
            strncpy(em.message, "Room is full.", sizeof(em.message) - 1);
            queue_write_pending(pc, &em, sizeof(em));
            return;
        }

        /* Transfer fd and buffers from pending to player slot */
        Player *p = &gs->players[slot];
        p->fd       = pc->fd;
        {
            int consumed  = (int)sizeof(JoinRoomMsg);
            int remaining = pc->rbuf_len - consumed;
            p->rbuf_len = (remaining > 0) ? remaining : 0;
            if (remaining > 0)
                memcpy(p->rbuf, pc->rbuf + consumed, (size_t)remaining);
        }
        p->wbuf_len = pc->wbuf_len;
        memcpy(p->wbuf, pc->wbuf, (size_t)pc->wbuf_len);
        p->alive    = 0;

        /* Release pending slot */
        pc->fd = -1;
        pc->rbuf_len = 0;
        pc->wbuf_len = 0;

        printf("[server] %s joining room %d slot %d as player\n",
               name, room_id, slot);

        int id = game_add_player(gs, p->fd, name);
        if (id < 0) {
            ErrorMsg em;
            memset(&em, 0, sizeof(em));
            em.msg_type = MSG_ERROR;
            strncpy(em.message, "Failed to join room.", sizeof(em.message) - 1);
            write_direct(p->fd, &em, sizeof(em));
            close(p->fd);
            p->fd = -1;
            return;
        }

        if (gs->host_id < 0) gs->host_id = id;

        if (send_join_ack(p, id, id == gs->host_id, gs->room_idx) < 0) {
            disconnect_player(gs, id); return;
        }
        if (send_char_list(gs, p) < 0) {
            disconnect_player(gs, id); return;
        }
        send_lobby_update(gs);

    } else {
        /* --- Game in progress: join as spectator --- */
        if (gs->spectator_count >= MAX_SPECTATORS) {
            ErrorMsg em;
            memset(&em, 0, sizeof(em));
            em.msg_type = MSG_ERROR;
            strncpy(em.message, "Too many spectators in this room.",
                    sizeof(em.message) - 1);
            queue_write_pending(pc, &em, sizeof(em));
            RoomListMsg rlm;
            build_room_list_msg(rooms, num_rooms, &rlm);
            queue_write_pending(pc, &rlm, sizeof(rlm));
            return;
        }

        /* Find empty spectator slot */
        int slot = -1;
        for (int s = 0; s < MAX_SPECTATORS; s++) {
            if (gs->spectators[s].fd == -1) { slot = s; break; }
        }
        if (slot < 0) {
            ErrorMsg em;
            memset(&em, 0, sizeof(em));
            em.msg_type = MSG_ERROR;
            strncpy(em.message, "Too many spectators in this room.",
                    sizeof(em.message) - 1);
            queue_write_pending(pc, &em, sizeof(em));
            return;
        }

        Spectator *sp = &gs->spectators[slot];
        sp->fd = pc->fd;
        strncpy(sp->name, name, PLAYER_NAME_LEN - 1);
        sp->name[PLAYER_NAME_LEN - 1] = '\0';
        {
            int consumed  = (int)sizeof(JoinRoomMsg);
            int remaining = pc->rbuf_len - consumed;
            sp->rbuf_len = (remaining > 0) ? remaining : 0;
            if (remaining > 0)
                memcpy(sp->rbuf, pc->rbuf + consumed, (size_t)remaining);
        }
        sp->wbuf_len = pc->wbuf_len;
        memcpy(sp->wbuf, pc->wbuf, (size_t)pc->wbuf_len);
        gs->spectator_count++;

        /* Release pending slot */
        pc->fd = -1;
        pc->rbuf_len = 0;
        pc->wbuf_len = 0;

        printf("[server] %s joined room %d as spectator (game in progress)\n",
               name, room_id);

        send_spectate_start(sp, gs);
    }
}

/* =========================================================================
 * Process buffer for a pending (pre-room) connection
 * ========================================================================= */

static void process_pending_buffer(PendingConn *pc, GameState *rooms,
                                   int num_rooms)
{
    while (pc->rbuf_len > 0) {
        uint8_t opcode = pc->rbuf[0];
        size_t  needed = 0;

        switch (opcode) {
        case MSG_JOIN_ROOM: needed = sizeof(JoinRoomMsg); break;
        default:
            fprintf(stderr,
                    "[server] Unknown opcode 0x%02x from pending conn fd %d\n",
                    opcode, pc->fd);
            disconnect_pending(pc);
            return;
        }

        if ((size_t)pc->rbuf_len < needed) break;

        if (opcode == MSG_JOIN_ROOM)
            handle_join_room(rooms, num_rooms, pc,
                             (const JoinRoomMsg *)pc->rbuf);

        if (pc->fd == -1) return; /* disconnected during handling */

        int remaining = pc->rbuf_len - (int)needed;
        if (remaining > 0)
            memmove(pc->rbuf, pc->rbuf + needed, (size_t)remaining);
        pc->rbuf_len = remaining;
    }
}

/* =========================================================================
 * Disconnect a player
 * ========================================================================= */

static void disconnect_player(GameState *gs, int player_id)
{
    Player *p = &gs->players[player_id];
    if (p->fd == -1) return;

    printf("[server] Player %d (%s) disconnected.\n", player_id, p->name);
    // close(p->fd);
    // p->fd = -1;

    // Safety check - if host disconnects, redirect host to remaining player
    if (player_id == gs->host_id){
    	gs->host_id = -1;
	for (int i=0; i < MAX_PLAYERS; i++) {
		if (i == player_id) continue;
		// Searching for existing, connected players
		if (gs->players[i].fd != -1) {
			gs->host_id = i;
			printf("[server] Player %d is the new host.\n", i);
			break;
		}
	}
    }
    int was_alive = p->alive;
    int saved_fd = p->fd; /* save before game_remove_player clears it */
    game_remove_player(gs, player_id);

    /* Restore fd so flush_wbuf and close operate on the real fd */
    p->fd = saved_fd;
    flush_wbuf(p);  /* best-effort drain before close */
    close(p->fd);
    p->fd = -1;
    p->wbuf_len = 0;

    if (gs->phase == STATE_LOBBY) {
	// Clean up the lobby
        // if (p->char_id >= 0 && p->char_id < NUM_CHARACTERS)
        //    gs->char_taken[p->char_id] = 0;

        // p->alive = 0;
        // p->char_id = -1;
        // gs->player_count--;
        // gs->alive_count--;
        
	// Update remaining players to lobby state
	send_lobby_update(gs);
    } else {
	// Automatic win if room only has one player left & redirect to lobby
        // int was_alive = p->alive;
        // game_remove_player(gs, player_id);

        if (was_alive){
            send_player_elim(gs, player_id);
		
            // Check if a player disconnects & end the game
	    int winner_id = -1;
	    int end = game_check_end(gs, &winner_id);
	    if (end != 0) {
		send_game_over(gs, end==2, winner_id);
		printf("[server] Game ended due to player disconnection. %s wins.\n",
				end == 2 ? "Tie" : gs->players[winner_id].name);

		gs->phase = STATE_LOBBY;
		gs->round_num = 0;

		for (int i=0; i < MAX_PLAYERS; i++) {
			if (gs->players[i].fd != -1) {
				gs->players[i].alive = 1;
				gs->players[i].pending_action_id = -1;
				gs->players[i].pending_target_id = NO_TARGET;
				if (gs->players[i].char_id >=0)
					gs->players[i].hp = gs->players[i].max_hp;
			}
		}
		/* Promote spectators to lobby players now that game has ended */
		promote_spectators(gs);
		send_lobby_update(gs);
	    }		/* Force player's screen to go back to lobby */
	}
    }
}

/* =========================================================================
 * Handle MSG_JOIN
 * ========================================================================= */

static void handle_join(GameState *gs, int player_id, const JoinMsg *msg)
{
    Player *p = &gs->players[player_id];

    if (gs->phase != STATE_LOBBY) {
        send_error(p, "Game already in progress.");
        flush_wbuf(p);
        close(p->fd);
        p->fd = -1;
        p->wbuf_len = 0;
        return;
    }

    char name[PLAYER_NAME_LEN];
    strncpy(name, msg->name, PLAYER_NAME_LEN - 1);
    name[PLAYER_NAME_LEN - 1] = '\0';
    for (int i = 0; name[i]; i++)
        if (name[i] < 0x20 || name[i] > 0x7E) name[i] = '?';

    int id = game_add_player(gs, p->fd, name);
    if (id < 0) {
        send_error(p, "Lobby is full.");
        flush_wbuf(p);
        close(p->fd);
        p->fd = -1;
        p->wbuf_len = 0;
        return;
    }

    if (gs->host_id < 0) gs->host_id = id;

    printf("[server] Player %d joined: %s%s\n", id, name,
           id == gs->host_id ? " (host)" : "");
    if (send_join_ack(p, id, id == gs->host_id, gs->room_idx) < 0) {
        disconnect_player(gs, id);
        return;
    }
    if (send_char_list(gs, p) < 0) {
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
        if (send_error(p, "Can't change character now.") < 0)
            disconnect_player(gs, player_id);
        return;
    }

    if (game_assign_character(gs, player_id, cid) < 0) {
        if (send_error(p, "Character unavailable or invalid.") < 0)
            disconnect_player(gs, player_id);
        return;
    }

    printf("[server] Player %d picked character %d (%s)\n",
           player_id, cid, p->name);

    /* Broadcast updated lobby and send updated char list to all */
    send_lobby_update(gs);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].fd != -1) {
            if (send_char_list(gs, &gs->players[i]) < 0)
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
        if (send_error(p, "Not currently accepting actions.") < 0)
            disconnect_player(gs, player_id);
        return;
    }
    if (p->pending_action_id != -1) {
        if (send_error(p, "You already submitted an action this round.") < 0)
            disconnect_player(gs, player_id);
        return;
    }

    int aid = msg->action_id;
    int tid = msg->target_id;

    char err[64];
    if (game_validate_action(gs, player_id, aid, tid, err, sizeof(err)) < 0) {
        if (send_error(p, err) < 0)
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

    if (send_wait(p) < 0)
        disconnect_player(gs, player_id);
    printf("[server] Player %d (%s) chose action %d target %d\n",
           player_id, p->name, aid, p->pending_target_id);
}

/* =========================================================================
 * Handle MSG_START_GAME
 * ========================================================================= */

static void handle_start_game(GameState *gs, int player_id)
{
    Player *hp = &gs->players[player_id];
    if (player_id != gs->host_id) {
        if (send_error(hp, "Only the host can start.") < 0)
            disconnect_player(gs, player_id);
        return;
    }
    if (gs->phase != STATE_LOBBY) {
        if (send_error(hp, "Game already started.") < 0)
            disconnect_player(gs, player_id);
        return;
    }

    /* Check all players have selected characters */
    int ready = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].fd != -1 && gs->players[i].name[0] != '\0') {
            if (gs->players[i].char_id < 0) {
                if (send_error(hp,
                           "All players must select a character first.") < 0)
                    disconnect_player(gs, player_id);
                return;
            }
            ready++;
        }
    }
    if (ready < MIN_PLAYERS) {
        if (send_error(hp,
                   "Need at least 2 players to start.") < 0)
            disconnect_player(gs, player_id);
        return;
    }

    printf("[server] Host started the game with %d players.\n", ready);
    send_game_start(gs);
    begin_round(gs);
    last_tick[gs->room_idx] = time(NULL);
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
            handle_join(gs, player_id, (const JoinMsg *)p->rbuf);
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

	// reset game & update to lobby
        gs->phase = STATE_LOBBY;
        gs->round_num = 0;
        gs->alive_count = 0;

        for (int i = 0; i < MAX_PLAYERS; i++) {
            Player *curr = &gs->players[i];
            if (curr->fd != -1) {
                curr->alive = 1;
                gs->alive_count++;
                if (curr->char_id >= 0) {
                    curr->hp = curr->max_hp; // Reset HP to full
                }
                curr->pending_action_id = -1;
                curr->pending_target_id = NO_TARGET;
            }
        }

        /* Promote spectators to lobby players now that the game has ended */
        promote_spectators(gs);
        send_lobby_update(gs); // Refresh client screens
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
            if (send_status_update(p, status_text) < 0)
                disconnect_player(gs, i);
        }
    }
}

/* =========================================================================
 * Start resolution (enter STATE_RESOLVING)
 * ========================================================================= */

static void start_resolution(GameState *gs, int r)
{
    gs->phase = STATE_RESOLVING;
    game_resolve_round(gs);
    gs->resolve_event_current = 0;
    gettimeofday(&resolve_last_send[r], NULL);

    /* Send first event immediately */
    if (gs->resolve_event_current < gs->resolve_event_count) {
        send_round_event(gs, gs->resolve_events[gs->resolve_event_current]);
        gs->resolve_event_current++;
        gettimeofday(&resolve_last_send[r], NULL);
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
        last_tick[gs->room_idx] = time(NULL);
    } else {
        gs->phase = STATE_GAME_OVER;
        send_game_over(gs, end == 2, winner_id);
        printf("[server] Game over. %s\n",
               end == 2 ? "Tie!" : gs->players[winner_id].name);
	// Reset the game & return to lobby
	gs->phase = STATE_LOBBY;
        gs->round_num = 0;
        gs->alive_count = 0; // Will be recalculated by players present

        for (int i = 0; i < MAX_PLAYERS; i++) {
            Player *curr = &gs->players[i];
            if (curr->fd != -1) {
                /* Make everyone "alive" again for the next game */
                curr->alive = 1;
                gs->alive_count++;

                /* Reset their HP based on the character they currently have */
                if (curr->char_id >= 0) {
                    curr->hp = curr->max_hp;
                }

                /* Clear their previous move choice */
                curr->pending_action_id = -1;
                curr->pending_target_id = NO_TARGET;
            }
        }
        /* Promote spectators to lobby players now that the game has ended */
        promote_spectators(gs);
	send_lobby_update(gs);
    }
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    int port = PORT;
    int num_rooms = 10;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            num_rooms = atoi(argv[++i]);
            if (num_rooms < 1 || num_rooms > MAX_ROOMS) {
                fprintf(stderr, "num_rooms must be between 1 and %d\n", MAX_ROOMS);
                return 1;
            }
        } else {
            fprintf(stderr, "Usage: %s [-p <port>] [-r <num_rooms>]\n", argv[0]);
            return 1;
        }
    }

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
    addr.sin_port        = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(listen_fd, MAX_PLAYERS) < 0) { perror("listen"); exit(1); }

    printf("[server] Listening on port %d with %d room(s)\n", port, num_rooms);

    last_tick = calloc((size_t)num_rooms, sizeof(*last_tick));
    resolve_last_send = calloc((size_t)num_rooms, sizeof(*resolve_last_send));
    GameState *rooms = malloc((size_t)num_rooms * sizeof(GameState));
    pending_conns = calloc(MAX_PENDING, sizeof(*pending_conns));
    if (!last_tick || !resolve_last_send || !rooms || !pending_conns) {
        perror("malloc"); exit(1);
    }
    num_rooms_g = num_rooms;
    for (int r = 0; r < num_rooms; r++) {
        game_init(&rooms[r]);
        rooms[r].room_idx = r;
    }
    for (int pc = 0; pc < MAX_PENDING; pc++)
        pending_conns[pc].fd = -1;

    for (;;) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        int maxfd = listen_fd;
        FD_SET(listen_fd, &rfds);

        for (int r = 0; r < num_rooms; r++) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                Player *pl = &rooms[r].players[i];
                if (pl->fd != -1) {
                    FD_SET(pl->fd, &rfds);
                    if (pl->wbuf_len > 0) FD_SET(pl->fd, &wfds);
                    if (pl->fd > maxfd) maxfd = pl->fd;
                }
            }
            for (int s = 0; s < MAX_SPECTATORS; s++) {
                Spectator *sp = &rooms[r].spectators[s];
                if (sp->fd != -1) {
                    FD_SET(sp->fd, &rfds);
                    if (sp->wbuf_len > 0) FD_SET(sp->fd, &wfds);
                    if (sp->fd > maxfd) maxfd = sp->fd;
                }
            }
        }
        for (int pc = 0; pc < MAX_PENDING; pc++) {
            if (pending_conns[pc].fd != -1) {
                FD_SET(pending_conns[pc].fd, &rfds);
                if (pending_conns[pc].wbuf_len > 0)
                    FD_SET(pending_conns[pc].fd, &wfds);
                if (pending_conns[pc].fd > maxfd) maxfd = pending_conns[pc].fd;
            }
        }

    /* Timeout logic */
        
	struct timeval tv;
	tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms
	struct timeval *tvp = &tv;

	for (int r = 0; r < num_rooms; r++) {
	GameState *gs = &rooms[r];
	
	// tv.tv_sec = 0;
	// tv.tv_usec = 100000;

        // struct timeval *tvp = &tv;

        if (gs->phase == STATE_ACTION_COLLECTION) {
            tv.tv_sec  = 1;
            tv.tv_usec = 0;
       
        } else if (gs->phase == STATE_RESOLVING) {
            /* Calculate time until next event */
            struct timeval now;
            gettimeofday(&now, NULL);
            long elapsed_us = (now.tv_sec - resolve_last_send[r].tv_sec) * 1000000L
                            + (now.tv_usec - resolve_last_send[r].tv_usec);
            long remaining_us = RESOLVE_DELAY_US - elapsed_us;
	    if (remaining_us < 0) remaining_us = 0;
            
	    struct timeval candidate;
	    candidate.tv_sec  = remaining_us / 1000000;
            candidate.tv_usec = remaining_us % 1000000;
            
	    if (candidate.tv_sec < tv.tv_sec ||
            (candidate.tv_sec == tv.tv_sec &&
             candidate.tv_usec < tv.tv_usec)) {
			    
	   	 tv = candidate;
	    }
        }
    }

        int ready = select(maxfd + 1, &rfds, &wfds, NULL, tvp);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select"); exit(1);
        }

        /* --- Flush writable player sockets --- */
        for (int r = 0; r < num_rooms; r++) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                Player *pl = &rooms[r].players[i];
                if (pl->fd != -1 && pl->wbuf_len > 0
                    && FD_ISSET(pl->fd, &wfds)) {
                    if (flush_wbuf(pl) < 0)
                        disconnect_player(&rooms[r], i);
                }
            }
            for (int s = 0; s < MAX_SPECTATORS; s++) {
                Spectator *sp = &rooms[r].spectators[s];
                if (sp->fd != -1 && sp->wbuf_len > 0
                    && FD_ISSET(sp->fd, &wfds)) {
                    if (flush_wbuf_spectator(sp) < 0)
                        disconnect_spectator(&rooms[r], s);
                }
            }
        }
        for (int pc = 0; pc < MAX_PENDING; pc++) {
            PendingConn *pconn = &pending_conns[pc];
            if (pconn->fd != -1 && pconn->wbuf_len > 0
                && FD_ISSET(pconn->fd, &wfds)) {
                if (flush_wbuf_pending(pconn) < 0)
                    disconnect_pending(pconn);
            }
        }

        /* --- Accept new connections -> put in pending, send room list --- */
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            int new_fd = accept(listen_fd,
                                (struct sockaddr *)&client_addr, &addrlen);
            if (new_fd >= 0) {
                set_nonblocking(new_fd);
                int found = 0;
                for (int pc = 0; pc < MAX_PENDING; pc++) {
                    if (pending_conns[pc].fd == -1) {
                        pending_conns[pc].fd       = new_fd;
                        pending_conns[pc].rbuf_len = 0;
                        pending_conns[pc].wbuf_len = 0;
                        printf("[server] New connection fd %d -> pending slot %d\n",
                               new_fd, pc);
                        /* Send room list so client can choose */
                        RoomListMsg rlm;
                        build_room_list_msg(rooms, num_rooms, &rlm);
                        queue_write_pending(&pending_conns[pc], &rlm, sizeof(rlm));
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    ErrorMsg em;
                    memset(&em, 0, sizeof(em));
                    em.msg_type = MSG_ERROR;
                    strncpy(em.message, "Server is full.",
                            sizeof(em.message) - 1);
                    write_direct(new_fd, &em, sizeof(em));
                    close(new_fd);
                }
            }
        }

        /* --- Handle readable pending connections --- */
        for (int pc = 0; pc < MAX_PENDING; pc++) {
            PendingConn *pconn = &pending_conns[pc];
            if (pconn->fd == -1 || !FD_ISSET(pconn->fd, &rfds)) continue;
            int space = (int)sizeof(pconn->rbuf) - pconn->rbuf_len;
            ssize_t n = read(pconn->fd, pconn->rbuf + pconn->rbuf_len,
                             (size_t)space);
            if (n <= 0) {
                disconnect_pending(pconn);
                continue;
            }
            pconn->rbuf_len += (int)n;
            process_pending_buffer(pconn, rooms, num_rooms);
        }
	
	// Add loop for tracking rooms
        for (int r = 0; r < num_rooms; r++) {
		GameState *gs = &rooms[r];	
	
	/* --- Timer tick (action collection) --- */
        if (gs->phase == STATE_ACTION_COLLECTION) {
            time_t now = time(NULL);
            if (ready == 0 || now != last_tick[r]) {
                if (now != last_tick[r]) {
                    last_tick[r] = now;
                    gs->timer_secs_left--;
                    send_timer_tick(gs, gs->timer_secs_left);
                    printf("[server] Room %d timer: %d seconds left\n",
                           r, gs->timer_secs_left);
                }
                if (gs->timer_secs_left <= 0) {
                    printf("[server] Room %d: Time expired, resolving round.\n", r);
                    start_resolution(gs, r);
                    continue;
                }
            }
            if (ready == 0) continue;
        }

        /* --- Resolve event dispatch --- */
        if (gs->phase == STATE_RESOLVING) {
            struct timeval now;
            gettimeofday(&now, NULL);
            long elapsed_us = (now.tv_sec - resolve_last_send[r].tv_sec) * 1000000L
                            + (now.tv_usec - resolve_last_send[r].tv_usec);

            if (elapsed_us >= RESOLVE_DELAY_US) {
                if (gs->resolve_event_current < gs->resolve_event_count) {
                    send_round_event(gs,
                        gs->resolve_events[gs->resolve_event_current]);
                    gs->resolve_event_current++;
                    gettimeofday(&resolve_last_send[r], NULL);
                    printf("[server] Room %d, Event %d/%d sent\n",
                           r, gs->resolve_event_current,
                           gs->resolve_event_count);
                } else {
                    finish_resolution(gs);
                    continue;
                }
            }
            if (ready == 0) continue;
        }
       
        /* --- Read from connected players --- */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            int fd = gs->players[i].fd;
            if (fd == -1 || !FD_ISSET(fd, &rfds)) continue;

            Player *p = &gs->players[i];
            int space = (int)(sizeof(p->rbuf)) - p->rbuf_len;
            ssize_t n = read(fd, p->rbuf + p->rbuf_len, (size_t)space);

            if (n < 0) {
                /* EAGAIN is normal for non-blocking sockets (e.g. fd just
                 * promoted from pending in this same select iteration) */
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                disconnect_player(gs, i);
                continue;
            }
            if (n == 0) {
                disconnect_player(gs, i);
                continue;
            }
            p->rbuf_len += (int)n;
            process_player_buffer(gs, i);
            if (gs->players[i].fd == -1) continue;
        }

        /* --- Read from spectators (drain; they don't send commands) --- */
        for (int s = 0; s < MAX_SPECTATORS; s++) {
            Spectator *sp = &gs->spectators[s];
            if (sp->fd == -1 || !FD_ISSET(sp->fd, &rfds)) continue;
            int space = (int)sizeof(sp->rbuf) - sp->rbuf_len;
            ssize_t n = read(sp->fd, sp->rbuf + sp->rbuf_len, (size_t)space);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                disconnect_spectator(gs, s);
                continue;
            }
            if (n == 0) {
                disconnect_spectator(gs, s);
                continue;
            }
            sp->rbuf_len += (int)n;
            sp->rbuf_len = 0; /* spectators don't send messages; just drain */
        }

        /* --- Check if all actions collected --- */
        if (gs->phase == STATE_ACTION_COLLECTION && game_all_actions_in(gs)) {
            printf("[server] Room %d: All actions received, resolving round.\n", r);
            start_resolution(gs, r);
        }

        /* --- Game over, all disconnected --- */
        if (gs->phase == STATE_GAME_OVER) {
            int still = 0;
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (gs->players[i].fd != -1) still++;

            if (still == 0 && gs->player_count > 0) {
                printf("[server] Room %d: All players disconnected. Resetting room...\n", r);
                game_init(gs);
            }
        }
      }
    }

    free(last_tick);
    free(resolve_last_send);
    free(rooms);
    free(pending_conns);
    close(listen_fd);
    return 0;
}
