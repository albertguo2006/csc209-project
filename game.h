#ifndef GAME_H
#define GAME_H

#include "protocol.h"

/* =========================================================================
 * Game state enum
 * ========================================================================= */

typedef enum {
    STATE_LOBBY,             /* Waiting for players to connect */
    STATE_ACTION_COLLECTION, /* Waiting for all players to submit actions */
    STATE_CALCULATION,       /* Resolving actions, broadcasting results */
    STATE_GAME_OVER,         /* Game has ended */
} GamePhase;

/* =========================================================================
 * Per-player server-side state
 * ========================================================================= */

typedef struct {
    int      fd;                    /* Socket fd; -1 if slot empty */
    uint8_t  id;                    /* 0-based index */
    char     name[PLAYER_NAME_LEN];
    int      hp;
    int      alive;                 /* 1 if still in game */

    /* Current round's submitted action (-1 = not yet submitted) */
    int      pending_action_id;
    int      pending_target_id;

    /* Read buffer for partial message reassembly */
    uint8_t  rbuf[256];
    int      rbuf_len;
} Player;

/* =========================================================================
 * Global game state
 * ========================================================================= */

typedef struct {
    Player    players[MAX_PLAYERS];
    int       player_count;         /* Total connected players */
    int       alive_count;
    GamePhase phase;
    int       round_num;
    int       timer_secs_left;

    /* Action table (data-driven, extensible) */
    ActionDef action_defs[MAX_ACTIONS];
    int       action_count;
} GameState;

/* =========================================================================
 * Function declarations
 * ========================================================================= */

/* Initialise a fresh GameState */
void game_init(GameState *gs);

/* Add a player to the lobby. Returns assigned player_id or -1 if full. */
int  game_add_player(GameState *gs, int fd, const char *name);

/* Remove a player (disconnect / elimination). */
void game_remove_player(GameState *gs, int player_id);

/* Returns 1 if all alive players have submitted an action this round. */
int  game_all_actions_in(const GameState *gs);

/*
 * Resolve all pending actions and populate results[].
 * Returns the number of ActionResult entries written.
 */
int  game_resolve_round(GameState *gs, ActionResult *results, int max_results);

/*
 * Check end conditions after resolution.
 * Returns: 0 = game continues, 1 = one winner, 2 = tie
 * Sets *winner_id to the surviving player id (or -1 on tie).
 */
int  game_check_end(const GameState *gs, int *winner_id);

/* Reset per-round pending actions for all alive players. */
void game_clear_actions(GameState *gs);

/* Build a PlayerInfo array from current game state (for messages). */
void game_build_player_info(const GameState *gs, PlayerInfo *out, int *count);

#endif /* GAME_H */
