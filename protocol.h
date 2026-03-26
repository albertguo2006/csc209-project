#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define MAX_PLAYERS      4
#define MAX_ACTIONS      16
#define PLAYER_NAME_LEN  32
#define ACTION_NAME_LEN  16
#define ACTION_DESC_LEN  48
#define STARTING_HP      100
#define ROUND_TIMER_SECS 30
#define NO_TARGET        0xFF

/* =========================================================================
 * Message opcodes (1 byte, first byte of every message)
 * ========================================================================= */

typedef enum {
    /* Client -> Server */
    MSG_JOIN          = 0x01,  /* Player sends their name to join */
    MSG_ACTION        = 0x02,  /* Player submits their chosen action */
    MSG_START_GAME    = 0x03,  /* Host requests game start */

    /* Server -> Client */
    MSG_LOBBY_UPDATE  = 0x10,  /* Broadcast: current lobby state */
    MSG_JOIN_ACK      = 0x19,  /* Unicast: confirm join, tell client their id + host status */
    MSG_GAME_START    = 0x11,  /* Broadcast: game is starting, send roster + action list */
    MSG_ROUND_START   = 0x12,  /* Broadcast: new round, current HP snapshot */
    MSG_TIMER_TICK    = 0x13,  /* Broadcast: seconds remaining in round */
    MSG_ROUND_RESULT  = 0x14,  /* Broadcast: all resolved actions + new HP values */
    MSG_PLAYER_ELIM   = 0x15,  /* Broadcast: a player was eliminated */
    MSG_GAME_OVER     = 0x16,  /* Broadcast: game ended, winner announced */
    MSG_ERROR         = 0x17,  /* Unicast: invalid action, re-prompt that player */
    MSG_WAIT          = 0x18,  /* Unicast: action accepted, waiting for others */
} MsgType;

/* =========================================================================
 * Shared sub-structures
 * ========================================================================= */

/* Snapshot of one player's public state, sent in several message types */
typedef struct {
    uint8_t  player_id;
    uint8_t  hp;            /* 0 means eliminated */
    uint8_t  alive;
    char     name[PLAYER_NAME_LEN];
} PlayerInfo;

/* One entry in the action menu sent to clients at game start */
typedef struct {
    uint8_t  action_id;
    uint8_t  requires_target;
    char     name[ACTION_NAME_LEN];
    char     desc[ACTION_DESC_LEN];
} ActionDef;

/* One resolved action, included in MSG_ROUND_RESULT */
typedef struct {
    uint8_t  actor_id;
    uint8_t  target_id;    /* NO_TARGET if untargeted */
    uint8_t  action_id;
    int8_t   hp_delta;     /* Change applied to target (negative = damage) */
} ActionResult;

/* =========================================================================
 * Client -> Server messages
 * ========================================================================= */

/*
 * MSG_JOIN
 * Direction : Client -> Server
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent immediately after connect. Server registers the player
 *             with the given name. If the lobby is full or the game has
 *             already started the server closes the connection.
 * Error     : If write() returns < sizeof(JoinMsg) the server closes the fd.
 */
typedef struct {
    uint8_t msg_type;               /* MSG_JOIN */
    char    name[PLAYER_NAME_LEN];
} JoinMsg;

/*
 * MSG_ACTION
 * Direction : Client -> Server
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent once per round during ACTION_COLLECTION phase. action_id
 *             must be a valid index from the ActionDef list sent at game
 *             start. target_id must be a living opponent's player_id when
 *             requires_target == 1, or NO_TARGET otherwise.
 * Error     : Invalid action_id or target_id causes MSG_ERROR response and
 *             the server waits for a corrected submission from this client.
 */
typedef struct {
    uint8_t msg_type;               /* MSG_ACTION */
    uint8_t action_id;
    uint8_t target_id;
    uint8_t padding;
} ActionMsg;

/* =========================================================================
 * Server -> Client messages
 * ========================================================================= */

/*
 * MSG_LOBBY_UPDATE
 * Direction : Server -> all connected clients
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent whenever a player joins or leaves the lobby. Clients
 *             should redraw the waiting room list.
 */
typedef struct {
    uint8_t    msg_type;            /* MSG_LOBBY_UPDATE */
    uint8_t    player_count;
    uint8_t    host_id;             /* player_id of the host */
    uint8_t    padding;
    PlayerInfo players[MAX_PLAYERS];
} LobbyUpdateMsg;

/*
 * MSG_GAME_START
 * Direction : Server -> all clients
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent once when the game begins. Contains the full player
 *             roster, each player's assigned ID, and the action menu.
 *             your_id tells each client which player_id belongs to them.
 */
typedef struct {
    uint8_t    msg_type;            /* MSG_GAME_START */
    uint8_t    your_id;
    uint8_t    player_count;
    uint8_t    action_count;
    PlayerInfo players[MAX_PLAYERS];
    ActionDef  actions[MAX_ACTIONS];
} GameStartMsg;

/*
 * MSG_ROUND_START
 * Direction : Server -> all clients
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent at the beginning of each round. Contains the current HP
 *             snapshot and round number. Clients should prompt the player
 *             to choose an action.
 */
typedef struct {
    uint8_t    msg_type;            /* MSG_ROUND_START */
    uint8_t    round_num;
    uint8_t    timer_secs;
    uint8_t    padding;
    PlayerInfo players[MAX_PLAYERS];
} RoundStartMsg;

/*
 * MSG_TIMER_TICK
 * Direction : Server -> all clients
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent once per second during ACTION_COLLECTION. seconds_left
 *             counts down from ROUND_TIMER_SECS to 0. When 0 the server
 *             auto-submits idle for any player who has not yet acted.
 */
typedef struct {
    uint8_t msg_type;               /* MSG_TIMER_TICK */
    uint8_t seconds_left;
    uint8_t padding[2];
} TimerTickMsg;

/*
 * MSG_ROUND_RESULT
 * Direction : Server -> all clients
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent after all actions are resolved. Lists every action taken
 *             and the resulting HP change. result_count is the number of
 *             valid entries in results[]. Clients should display a narrative
 *             and update their local HP display.
 */
typedef struct {
    uint8_t      msg_type;          /* MSG_ROUND_RESULT */
    uint8_t      result_count;
    uint8_t      padding[2];
    ActionResult results[MAX_PLAYERS];
    PlayerInfo   players[MAX_PLAYERS]; /* HP snapshot after resolution */
} RoundResultMsg;

/*
 * MSG_PLAYER_ELIM
 * Direction : Server -> all clients
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent when a player's HP reaches 0. name is included for
 *             display. Clients should mark that player as eliminated.
 */
typedef struct {
    uint8_t msg_type;               /* MSG_PLAYER_ELIM */
    uint8_t player_id;
    uint8_t padding[2];
    char    name[PLAYER_NAME_LEN];
} PlayerElimMsg;

/*
 * MSG_GAME_OVER
 * Direction : Server -> all clients
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent when the game ends (one survivor or a tie). is_tie
 *             is 1 if multiple players reached 0 HP simultaneously.
 *             winner_id is only meaningful when is_tie == 0.
 */
typedef struct {
    uint8_t msg_type;               /* MSG_GAME_OVER */
    uint8_t is_tie;
    uint8_t winner_id;
    uint8_t padding;
    char    winner_name[PLAYER_NAME_LEN];
} GameOverMsg;

/*
 * MSG_ERROR
 * Direction : Server -> one client (unicast)
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent when the client submits an invalid action. The client
 *             must re-submit a valid MSG_ACTION. The server does not
 *             advance until a valid action is received.
 */
typedef struct {
    uint8_t msg_type;               /* MSG_ERROR */
    uint8_t padding[3];
    char    message[64];
} ErrorMsg;

/*
 * MSG_WAIT
 * Direction : Server -> one client (unicast)
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent to confirm a valid action was received. The client
 *             should display a "waiting for others..." message.
 */
typedef struct {
    uint8_t msg_type;               /* MSG_WAIT */
    uint8_t padding[3];
} WaitMsg;

/*
 * MSG_JOIN_ACK
 * Direction : Server -> one client (unicast)
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent right after a successful MSG_JOIN. Tells the client
 *             their assigned slot id and whether they are the host.
 */
typedef struct {
    uint8_t msg_type;               /* MSG_JOIN_ACK */
    uint8_t your_id;
    uint8_t is_host;
    uint8_t padding;
} JoinAckMsg;

/*
 * MSG_START_GAME
 * Direction : Client -> Server
 * Encoding  : Fixed-width struct, single write()
 * Semantics : Sent by the host to start the game. Server rejects if sender
 *             is not the host or if fewer than MIN_PLAYERS are in the lobby.
 */
typedef struct {
    uint8_t msg_type;               /* MSG_START_GAME */
    uint8_t padding[3];
} StartGameMsg;

#endif /* PROTOCOL_H */
