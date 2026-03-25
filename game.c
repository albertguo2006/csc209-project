#include <string.h>
#include <stdio.h>
#include "game.h"

/* =========================================================================
 * Action implementations
 * Each function receives the actor, the target (may be NULL for untargeted
 * actions), and the full game state.  It applies its effect directly to
 * player hp fields and returns the hp_delta applied to the target.
 * ========================================================================= */

static int do_attack(Player *actor, Player *target, GameState *gs)
{
    (void)gs;
    (void)actor;
    if (target == NULL) return 0;

    /* Base attack: 20 damage, halved if target chose defend (action_id 1) */
    int dmg = 20;
    if (target->pending_action_id == 1) { /* DEFEND */
        dmg = 10;
    }
    target->hp -= dmg;
    if (target->hp < 0) target->hp = 0;
    return -dmg;
}

static int do_defend(Player *actor, Player *target, GameState *gs)
{
    /* Effect is passive (handled in do_attack). Nothing to do here. */
    (void)actor;
    (void)target;
    (void)gs;
    return 0;
}

static int do_heal(Player *actor, Player *target, GameState *gs)
{
    (void)target;
    (void)gs;
    int gain = 15;
    actor->hp += gain;
    if (actor->hp > STARTING_HP) actor->hp = STARTING_HP;
    return gain;
}

/* =========================================================================
 * Action function pointer table — extend here to add new actions
 * ========================================================================= */

typedef int (*ActionFn)(Player *actor, Player *target, GameState *gs);

typedef struct {
    ActionFn fn;
} ActionImpl;

static ActionImpl action_impls[MAX_ACTIONS];

/* =========================================================================
 * game_init
 * ========================================================================= */

void game_init(GameState *gs)
{
    memset(gs, 0, sizeof(*gs));

    for (int i = 0; i < MAX_PLAYERS; i++) {
        gs->players[i].fd    = -1;
        gs->players[i].id    = (uint8_t)i;
        gs->players[i].alive = 0;
        gs->players[i].pending_action_id = -1;
        gs->players[i].pending_target_id = NO_TARGET;
    }

    gs->phase      = STATE_LOBBY;
    gs->round_num  = 0;
    gs->alive_count = 0;

    /* --- Define the MVP action table --- */
    int n = 0;

    gs->action_defs[n].action_id        = 0;
    gs->action_defs[n].requires_target  = 1;
    strncpy(gs->action_defs[n].name, "Attack",  ACTION_NAME_LEN - 1);
    strncpy(gs->action_defs[n].desc, "Deal 20 damage (10 if target defends)", ACTION_DESC_LEN - 1);
    action_impls[n].fn = do_attack;
    n++;

    gs->action_defs[n].action_id        = 1;
    gs->action_defs[n].requires_target  = 0;
    strncpy(gs->action_defs[n].name, "Defend",  ACTION_NAME_LEN - 1);
    strncpy(gs->action_defs[n].desc, "Halve incoming damage this round", ACTION_DESC_LEN - 1);
    action_impls[n].fn = do_defend;
    n++;

    gs->action_defs[n].action_id        = 2;
    gs->action_defs[n].requires_target  = 0;
    strncpy(gs->action_defs[n].name, "Heal",    ACTION_NAME_LEN - 1);
    strncpy(gs->action_defs[n].desc, "Restore 15 HP (max 100)", ACTION_DESC_LEN - 1);
    action_impls[n].fn = do_heal;
    n++;

    gs->action_count = n;
}

/* =========================================================================
 * game_add_player
 * ========================================================================= */

int game_add_player(GameState *gs, int fd, const char *name)
{
    if (gs->player_count >= MAX_PLAYERS) return -1;

    /* Find an empty slot */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].fd == -1) {
            gs->players[i].fd    = fd;
            gs->players[i].hp    = STARTING_HP;
            gs->players[i].alive = 1;
            gs->players[i].pending_action_id = -1;
            gs->players[i].pending_target_id = NO_TARGET;
            gs->players[i].rbuf_len = 0;
            strncpy(gs->players[i].name, name, PLAYER_NAME_LEN - 1);
            gs->players[i].name[PLAYER_NAME_LEN - 1] = '\0';

            gs->player_count++;
            gs->alive_count++;
            return i;
        }
    }
    return -1;
}

/* =========================================================================
 * game_remove_player
 * ========================================================================= */

void game_remove_player(GameState *gs, int player_id)
{
    Player *p = &gs->players[player_id];
    if (p->fd == -1) return;

    p->fd    = -1;
    p->alive = 0;
    p->hp    = 0;
    gs->player_count--;
    gs->alive_count--;
}

/* =========================================================================
 * game_all_actions_in
 * ========================================================================= */

int game_all_actions_in(const GameState *gs)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *p = &gs->players[i];
        if (p->alive && p->fd != -1 && p->pending_action_id == -1) {
            return 0;
        }
    }
    return 1;
}

/* =========================================================================
 * game_resolve_round
 *
 * Resolution order:
 *   1. Defend actions are passive (already flagged, handled in do_attack).
 *   2. Attacks are processed.
 *   3. Heals are processed.
 *   4. Mark eliminated players.
 * ========================================================================= */

int game_resolve_round(GameState *gs, ActionResult *results, int max_results)
{
    int count = 0;

    /* Auto-idle any alive player who never submitted (timer expired) */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].alive && gs->players[i].pending_action_id == -1) {
            gs->players[i].pending_action_id = 1; /* default: Defend */
            gs->players[i].pending_target_id = NO_TARGET;
        }
    }

    /* Process attacks first */
    for (int i = 0; i < MAX_PLAYERS && count < max_results; i++) {
        Player *actor = &gs->players[i];
        if (!actor->alive || actor->pending_action_id != 0) continue; /* not attacking */

        int tid = actor->pending_target_id;
        Player *target = (tid < MAX_PLAYERS && gs->players[tid].alive) ? &gs->players[tid] : NULL;

        int delta = action_impls[0].fn(actor, target, gs);

        results[count].actor_id  = (uint8_t)i;
        results[count].target_id = (target != NULL) ? (uint8_t)tid : NO_TARGET;
        results[count].action_id = 0;
        results[count].hp_delta  = (int8_t)delta;
        count++;
    }

    /* Process heals */
    for (int i = 0; i < MAX_PLAYERS && count < max_results; i++) {
        Player *actor = &gs->players[i];
        if (!actor->alive || actor->pending_action_id != 2) continue; /* not healing */

        int delta = action_impls[2].fn(actor, NULL, gs);

        results[count].actor_id  = (uint8_t)i;
        results[count].target_id = NO_TARGET;
        results[count].action_id = 2;
        results[count].hp_delta  = (int8_t)delta;
        count++;
    }

    /* Record defend actions (passive, delta = 0) */
    for (int i = 0; i < MAX_PLAYERS && count < max_results; i++) {
        Player *actor = &gs->players[i];
        if (!actor->alive || actor->pending_action_id != 1) continue;

        results[count].actor_id  = (uint8_t)i;
        results[count].target_id = NO_TARGET;
        results[count].action_id = 1;
        results[count].hp_delta  = 0;
        count++;
    }

    /* Mark players with 0 hp as eliminated */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].alive && gs->players[i].hp <= 0) {
            gs->players[i].alive = 0;
            gs->alive_count--;
        }
    }

    return count;
}

/* =========================================================================
 * game_check_end
 * ========================================================================= */

int game_check_end(const GameState *gs, int *winner_id)
{
    *winner_id = -1;

    if (gs->alive_count == 0) {
        return 2; /* tie — everyone died simultaneously */
    }
    if (gs->alive_count == 1) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (gs->players[i].alive) {
                *winner_id = i;
                return 1;
            }
        }
    }
    return 0;
}

/* =========================================================================
 * game_clear_actions
 * ========================================================================= */

void game_clear_actions(GameState *gs)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gs->players[i].pending_action_id = -1;
        gs->players[i].pending_target_id = NO_TARGET;
    }
}

/* =========================================================================
 * game_build_player_info
 * ========================================================================= */

void game_build_player_info(const GameState *gs, PlayerInfo *out, int *count)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *p = &gs->players[i];
        if (p->fd == -1 && !p->alive) continue;
        out[n].player_id = p->id;
        out[n].hp        = (uint8_t)(p->hp < 0 ? 0 : p->hp);
        out[n].alive     = (uint8_t)p->alive;
        strncpy(out[n].name, p->name, PLAYER_NAME_LEN - 1);
        out[n].name[PLAYER_NAME_LEN - 1] = '\0';
        n++;
    }
    *count = n;
}
