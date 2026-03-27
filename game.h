#ifndef GAME_H
#define GAME_H

#include "protocol.h"

#define MAX_EVENTS 16

/* =========================================================================
 * Game phases
 * ========================================================================= */

typedef enum {
    STATE_LOBBY,
    STATE_ACTION_COLLECTION,
    STATE_RESOLVING,            /* Drip-feeding events to clients */
    STATE_GAME_OVER,
} GamePhase;

/* =========================================================================
 * Status effects per player
 * ========================================================================= */

typedef struct {
    /* Gradient Descent: stacking attack reduction (permanent) */
    int attack_debuff_pct;

    /* Bioinformatics Synthesis: status immunity */
    int status_immune_turns;

    /* CR/NCR */
    int invulnerable;
    int crncr_cooldown;             /* 1 = can't use CR/NCR this round */

    /* Drug Mixing: forced skip */
    int skip_next_turn;

    /* Poison */
    int poison_turns;
    int poison_damage;

    /* Undefined Behaviour: scrambled next move */
    int scrambled;

    /* Dangling Pointer: forced miss */
    int forced_miss;

    /* Buck-a-Beer: permanent defense reduction */
    int defense_debuff_pct;

    /* Evasion (Notwithstanding / Shuttle Buses) */
    int evasion_chance;
    int evasion_turns;

    /* Shuttle Buses: heal on dodge */
    int heal_on_dodge;

    /* Fare Inspector: healing blocked */
    int healing_blocked_turns;

    /* Deduct Marks: healing reduction */
    int healing_reduction_pct;
    int healing_reduction_turns;

    /* Office Hours: shield */
    int shield_hp;
    int shield_next_round;

    /* Love Thy Neighbour */
    int love_damage_red_turns;
    int love_no_damage_turns;

    /* Turn the Other Cheek */
    int damage_to_healing;
    int toc_cooldown;               /* 1 = can't use this round */

    /* Divine Intervention */
    int divine_mark;
    int di_cooldown;                /* 1 = can't use this round */

    /* Karen: Autotester streak */
    int autotester_streak;
    int autotester_last_target;     /* player id or -1 */

    /* Piazza Endorsement */
    int piazza_endorsed;

    /* Academic Integrity Check */
    int academic_integrity;

    /* Man Page Reading: permanent buffs */
    int damage_boost_pct;
    int permanent_dmg_reduction_pct;

    /* Check the Syllabus (TA) */
    int counter_active;
    int counter_used;

    /* Extension Denied tracking */
    int healed_last_turn;
    int healed_this_turn;

    /* Slow Zone: speed reduction (permanent, stacking) */
    int speed_reduction_pct;
} StatusEffects;

/* =========================================================================
 * Per-player server-side state
 * ========================================================================= */

typedef struct {
    int      fd;
    uint8_t  id;
    char     name[PLAYER_NAME_LEN];
    int      hp;
    int      max_hp;
    int      alive;
    int      base_speed;
    int      char_id;               /* 0-7, or -1 if none selected */
    int      char_type;

    /* Current round's submitted action (-1 = not yet submitted) */
    int      pending_action_id;
    int      pending_target_id;

    StatusEffects status;

    /* Read buffer for partial message reassembly */
    uint8_t  rbuf[256];
    int      rbuf_len;
} Player;

/* =========================================================================
 * Global game state
 * ========================================================================= */

typedef struct {
    Player    players[MAX_PLAYERS];
    int       player_count;
    int       alive_count;
    GamePhase phase;
    int       round_num;
    int       timer_secs_left;
    int       host_id;

    /* Character selection tracking */
    uint8_t   char_taken[NUM_CHARACTERS];

    /* Resolution event queue (filled by game_resolve_round) */
    char      resolve_events[MAX_EVENTS][EVENT_TEXT_LEN];
    int       resolve_event_count;
    int       resolve_event_current;
} GameState;

/* =========================================================================
 * Function declarations
 * ========================================================================= */

void game_init(GameState *gs);
int  game_add_player(GameState *gs, int fd, const char *name);
void game_remove_player(GameState *gs, int player_id);
int  game_all_actions_in(const GameState *gs);
int  game_assign_character(GameState *gs, int player_id, int char_id);

/* Resolve all pending actions sequentially by speed.
 * Populates gs->resolve_events[] with narrative text.
 * Returns number of events. */
int  game_resolve_round(GameState *gs);

int  game_check_end(const GameState *gs, int *winner_id);
void game_clear_actions(GameState *gs);
void game_build_player_info(const GameState *gs, PlayerInfo *out, int *count);

/* Tick status effects at start of round (poison, expiry, etc.).
 * Writes narrative of any tick effects into tick_text. */
void game_tick_effects(GameState *gs, char *tick_text, int text_len);

/* Build human-readable status text for one player. */
void game_build_status_text(const GameState *gs, int player_id,
                            char *out, int out_len);

/* Fill CharEntry array for character selection screen. */
void game_build_char_list(const GameState *gs, CharEntry *out);

/* Get player's current speed (base - reductions). */
int  game_get_current_speed(const Player *p);

/* Get action defs for a character (for GameStartMsg). */
void game_get_char_actions(int char_id, ActionDef *out, int *count);

/* Validate if a player can use a specific action this round. */
int  game_validate_action(const GameState *gs, int player_id, int action_id,
                          int target_id, char *err, int err_len);

#endif /* GAME_H */
