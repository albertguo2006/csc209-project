#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define MAX_PLAYERS      8
#define NUM_CHARACTERS   8
#define MOVES_PER_CHAR   4
#define PLAYER_NAME_LEN  32
#define ACTION_NAME_LEN  32
#define ACTION_DESC_LEN  96
#define EVENT_TEXT_LEN   256
#define STATUS_TEXT_LEN  512
#define ROUND_TIMER_SECS 30
#define NO_TARGET        0xFF
#define NO_CHARACTER     0xFF

/* =========================================================================
 * Character types
 * ========================================================================= */

typedef enum {
    TYPE_STUDENT         = 0,
    TYPE_POLITICIAN      = 1,
    TYPE_INFRASTRUCTURE  = 2,
    TYPE_TEACHING_STAFF  = 3,
    TYPE_DIVINE          = 4,
} CharType;

static const char *const CHAR_TYPE_NAMES[] = {
    "Student", "Politician", "Infrastructure", "Teaching Staff", "Divine"
};

/* =========================================================================
 * Message opcodes
 * ========================================================================= */

typedef enum {
    /* Client -> Server */
    MSG_JOIN          = 0x01,
    MSG_ACTION        = 0x02,
    MSG_START_GAME    = 0x03,
    MSG_CHAR_SELECT   = 0x04,

    /* Server -> Client */
    MSG_LOBBY_UPDATE  = 0x10,
    MSG_GAME_START    = 0x11,
    MSG_ROUND_START   = 0x12,
    MSG_TIMER_TICK    = 0x13,
    MSG_ROUND_EVENT   = 0x14,   /* One narrative event during resolution */
    MSG_PLAYER_ELIM   = 0x15,
    MSG_GAME_OVER     = 0x16,
    MSG_ERROR         = 0x17,
    MSG_WAIT          = 0x18,
    MSG_JOIN_ACK      = 0x19,
    MSG_CHAR_LIST     = 0x1A,   /* Available characters for selection */
    MSG_STATUS_UPDATE = 0x1B,   /* Per-player status effects text */
} MsgType;

/* =========================================================================
 * Shared sub-structures
 * ========================================================================= */

typedef struct {
    uint8_t  player_id;
    uint8_t  hp;
    uint8_t  alive;
    uint8_t  char_type;
    uint8_t  speed;
    uint8_t  max_hp;
    uint8_t  char_id;           /* NO_CHARACTER if none selected */
    uint8_t  padding;
    char     name[PLAYER_NAME_LEN];
} PlayerInfo;

typedef struct {
    uint8_t  action_id;
    uint8_t  requires_target;   /* 0=no target, 1=target enemy, 2=target any */
    uint8_t  padding[2];
    char     name[ACTION_NAME_LEN];
    char     desc[ACTION_DESC_LEN];
} ActionDef;

/* Character entry for selection screen */
typedef struct {
    uint8_t   char_id;
    uint8_t   char_type;
    uint8_t   hp;
    uint8_t   speed;
    uint8_t   num_moves;
    uint8_t   taken;            /* 1 if already picked */
    uint8_t   padding[2];
    char      name[PLAYER_NAME_LEN];
    ActionDef moves[MOVES_PER_CHAR];
} CharEntry;

/* =========================================================================
 * Client -> Server messages
 * ========================================================================= */

typedef struct {
    uint8_t msg_type;           /* MSG_JOIN */
    char    name[PLAYER_NAME_LEN];
} JoinMsg;

typedef struct {
    uint8_t msg_type;           /* MSG_ACTION */
    uint8_t action_id;
    uint8_t target_id;
    uint8_t padding;
} ActionMsg;

typedef struct {
    uint8_t msg_type;           /* MSG_START_GAME */
    uint8_t padding[3];
} StartGameMsg;

typedef struct {
    uint8_t msg_type;           /* MSG_CHAR_SELECT */
    uint8_t char_id;
    uint8_t padding[2];
} CharSelectMsg;

/* =========================================================================
 * Server -> Client messages
 * ========================================================================= */

typedef struct {
    uint8_t    msg_type;        /* MSG_LOBBY_UPDATE */
    uint8_t    player_count;
    uint8_t    host_id;
    uint8_t    padding;
    PlayerInfo players[MAX_PLAYERS];
} LobbyUpdateMsg;

typedef struct {
    uint8_t    msg_type;        /* MSG_GAME_START */
    uint8_t    your_id;
    uint8_t    player_count;
    uint8_t    action_count;
    PlayerInfo players[MAX_PLAYERS];
    ActionDef  actions[MOVES_PER_CHAR];
} GameStartMsg;

typedef struct {
    uint8_t    msg_type;        /* MSG_ROUND_START */
    uint8_t    round_num;
    uint8_t    timer_secs;
    uint8_t    player_count;
    PlayerInfo players[MAX_PLAYERS];
} RoundStartMsg;

typedef struct {
    uint8_t msg_type;           /* MSG_TIMER_TICK */
    uint8_t seconds_left;
    uint8_t padding[2];
} TimerTickMsg;

typedef struct {
    uint8_t msg_type;           /* MSG_ROUND_EVENT */
    uint8_t padding[3];
    char    text[EVENT_TEXT_LEN];
} RoundEventMsg;

typedef struct {
    uint8_t msg_type;           /* MSG_PLAYER_ELIM */
    uint8_t player_id;
    uint8_t padding[2];
    char    name[PLAYER_NAME_LEN];
} PlayerElimMsg;

typedef struct {
    uint8_t msg_type;           /* MSG_GAME_OVER */
    uint8_t is_tie;
    uint8_t winner_id;
    uint8_t padding;
    char    winner_name[PLAYER_NAME_LEN];
} GameOverMsg;

typedef struct {
    uint8_t msg_type;           /* MSG_ERROR */
    uint8_t padding[3];
    char    message[64];
} ErrorMsg;

typedef struct {
    uint8_t msg_type;           /* MSG_WAIT */
    uint8_t padding[3];
} WaitMsg;

typedef struct {
    uint8_t msg_type;           /* MSG_JOIN_ACK */
    uint8_t your_id;
    uint8_t is_host;
    uint8_t padding;
} JoinAckMsg;

typedef struct {
    uint8_t   msg_type;         /* MSG_CHAR_LIST */
    uint8_t   char_count;
    uint8_t   padding[2];
    CharEntry chars[NUM_CHARACTERS];
} CharListMsg;

typedef struct {
    uint8_t msg_type;           /* MSG_STATUS_UPDATE */
    uint8_t padding[3];
    char    text[STATUS_TEXT_LEN];
} StatusUpdateMsg;

#endif /* PROTOCOL_H */
