#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include "game.h"

/* =========================================================================
 * Character & move definitions (static data)
 * ========================================================================= */

typedef void (*MoveFn)(Player *actor, Player *target, GameState *gs,
                       char *narr, int nlen);

typedef struct {
    const char *name;
    int   type;
    int   base_hp;
    int   base_speed;
    int   num_moves;
    struct {
        const char *name;
        const char *desc;
        int requires_target;    /* 0=self/AoE, 1=enemy, 2=any player */
        int is_defensive;       /* 1 = pre-applied before attacks */
        int is_priority;        /* 1 = always goes first */
    } moves[MOVES_PER_CHAR];
} CharacterDef;

static const CharacterDef char_defs[NUM_CHARACTERS] = {
    /* 0: Albert Guo */
    { "Albert Guo", TYPE_STUDENT, 100, 10, 4, {
        { "Gradient Descent", "Moderate dmg, permanently -10% atk (stacks)", 1, 0, 0 },
        { "Bioinformatics",   "Heal 30% max HP, status immunity 2 turns",   0, 0, 0 },
        { "Overfit Model",    "Massive unblockable dmg, costs 15% own HP",   1, 0, 0 },
        { "Data Pipeline",    "Steal 20 HP (ignores type resistances)",      1, 0, 0 },
    }},
    /* 1: Kaitlyn Zhu */
    { "Kaitlyn Zhu", TYPE_STUDENT, 100, 9, 4, {
        { "Stab",           "Inflict 30 damage",                             1, 0, 0 },
        { "CR/NCR",         "Invulnerable 1 turn (no back-to-back)",         0, 1, 0 },
        { "Drug Mixing",    "Random: 40 dmg / skip turn / poison / nothing", 1, 0, 0 },
        { "Pizza & Fries",  "Damage = ceil(2.25 x target speed)",            1, 0, 0 },
    }},
    /* 2: Yibin Wang */
    { "Yibin Wang", TYPE_STUDENT, 100, 12, 3, {
        { "Undef Behaviour",  "Moderate dmg, scramble target next move",     1, 0, 0 },
        { "Dangling Pointer", "Target next attack misses, 50% redirect",     1, 0, 0 },
        { "Null Dereference", "Execute if target <25% HP, else 20 dmg",      1, 0, 0 },
        { "", "", 0, 0, 0 },
    }},
    /* 3: Doug Ford */
    { "Doug Ford", TYPE_POLITICIAN, 150, 5, 4, {
        { "OSAP Cuts",         "Heavy dmg vs staff, half vs students",       1, 0, 0 },
        { "Notwithstanding",   "Clear debuffs, 50% evasion this round",      0, 1, 0 },
        { "Filibuster",        "15 AoE dmg to all (always first)",           0, 0, 1 },
        { "Buck-a-Beer",       "Heal 40 HP, permanently -15% defense",       0, 0, 0 },
    }},
    /* 4: TTC */
    { "TTC", TYPE_INFRASTRUCTURE, 120, 6, 4, {
        { "Planned Trackwork", "Moderate damage (always first)",             1, 0, 1 },
        { "Shuttle Buses",     "50% evasion 2 turns, heal 10 on dodge",      0, 1, 0 },
        { "Fare Inspector",    "20% target HP dmg, block healing 1 turn",    1, 0, 0 },
        { "Slow Zone",         "Reduce target speed by 25%",                 1, 0, 0 },
    }},
    /* 5: The Bitter TA */
    { "The Bitter TA", TYPE_TEACHING_STAFF, 100, 8, 4, {
        { "Deduct Marks",    "Moderate dmg (bypass shields), -50% heal 2t",  1, 0, 0 },
        { "Check Syllabus",  "Counter first attacker for 30 dmg",            0, 1, 0 },
        { "Extension Denied","Double dmg if target healed last turn",        1, 0, 0 },
        { "Office Hours",    "Heal 15 HP, gain 25 shield next round",        0, 0, 0 },
    }},
    /* 6: Jesus Christ */
    { "Jesus Christ", TYPE_DIVINE, 150, 20, 4, {
        { "Table Flip",       "15 AoE dmg, clear ALL field effects",         0, 0, 0 },
        { "Turn Other Cheek", "50% dmg taken becomes healing (no b2b)",      0, 1, 0 },
        { "Divine Intervene", "Protect target from death (no b2b)",          2, 1, 0 },
        { "Love Thy Neighb.", "-30% incoming 2 turns, can't deal dmg",       0, 1, 0 },
    }},
    /* 7: Karen Reid */
    { "Karen Reid", TYPE_TEACHING_STAFF, 100, 9, 4, {
        { "The Autotester",   "Escalating dmg (15/30/45) on same target",    1, 0, 0 },
        { "Piazza Endorsed",  "Heal 20 HP, next attack can't miss",          0, 0, 0 },
        { "Academic Integ.",  "Nullify special effects, reflect 25 dmg",     0, 1, 0 },
        { "Man Page Reading", "Permanent -15% dmg taken, +15% dmg dealt",    0, 0, 0 },
    }},
};

/* =========================================================================
 * Forward declarations for all move functions
 * ========================================================================= */

static void move_gradient_descent(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_bioinformatics(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_overfit_model(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_data_pipeline(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_stab(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_crncr(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_drug_mixing(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_pizza_fries(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_undef_behaviour(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_dangling_pointer(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_null_deref(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_osap_cuts(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_notwithstanding(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_filibuster(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_buck_a_beer(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_planned_trackwork(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_shuttle_buses(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_fare_inspector(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_slow_zone(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_deduct_marks(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_check_syllabus(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_extension_denied(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_office_hours(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_table_flip(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_turn_other_cheek(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_divine_intervention(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_love_thy_neighbour(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_autotester(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_piazza_endorsed(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_academic_integrity(Player *a, Player *t, GameState *gs, char *n, int nl);
static void move_man_page_reading(Player *a, Player *t, GameState *gs, char *n, int nl);

/* Function pointer table [character_id][move_id] */
static MoveFn move_fns[NUM_CHARACTERS][MOVES_PER_CHAR] = {
    { move_gradient_descent, move_bioinformatics, move_overfit_model, move_data_pipeline },
    { move_stab, move_crncr, move_drug_mixing, move_pizza_fries },
    { move_undef_behaviour, move_dangling_pointer, move_null_deref, NULL },
    { move_osap_cuts, move_notwithstanding, move_filibuster, move_buck_a_beer },
    { move_planned_trackwork, move_shuttle_buses, move_fare_inspector, move_slow_zone },
    { move_deduct_marks, move_check_syllabus, move_extension_denied, move_office_hours },
    { move_table_flip, move_turn_other_cheek, move_divine_intervention, move_love_thy_neighbour },
    { move_autotester, move_piazza_endorsed, move_academic_integrity, move_man_page_reading },
};

/* =========================================================================
 * Utility: safe narrative append
 * ========================================================================= */

static void narr_append(char *narr, int nlen, const char *fmt, ...)
{
    int used = (int)strlen(narr);
    if (used >= nlen - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(narr + used, nlen - used, fmt, ap);
    va_end(ap);
}

/* =========================================================================
 * Type effectiveness
 * ========================================================================= */

static float get_type_effectiveness(int atk_type, int def_type)
{
    if (atk_type == TYPE_POLITICIAN &&
        (def_type == TYPE_TEACHING_STAFF || def_type == TYPE_INFRASTRUCTURE))
        return 1.5f;
    if (atk_type == TYPE_TEACHING_STAFF && def_type == TYPE_STUDENT)
        return 1.5f;
    if (atk_type == TYPE_INFRASTRUCTURE && def_type == TYPE_STUDENT)
        return 1.5f;
    if (atk_type == TYPE_STUDENT && def_type == TYPE_POLITICIAN)
        return 1.5f;
    return 1.0f;
}

/* =========================================================================
 * Current speed (with reductions)
 * ========================================================================= */

int game_get_current_speed(const Player *p)
{
    int speed = p->base_speed;
    if (p->status.speed_reduction_pct > 0)
        speed = speed * (100 - p->status.speed_reduction_pct) / 100;
    if (speed < 1) speed = 1;
    return speed;
}

/* =========================================================================
 * Damage calculation flags
 * ========================================================================= */

#define DMG_UNBLOCKABLE    (1 << 0)
#define DMG_BYPASS_BUFFS   (1 << 1)
#define DMG_IGNORE_TYPE    (1 << 2)
#define DMG_HAS_SPECIAL    (1 << 3)

/*
 * apply_damage: central damage pipeline.
 * Handles type effectiveness, attacker/defender modifiers, all defensive
 * checks (counter, invulnerability, evasion, shield, damage reduction,
 * Turn the Other Cheek, Divine mark).
 * Appends narrative text.  Returns actual damage dealt.
 */
static int apply_damage(Player *attacker, Player *target, GameState *gs,
                        int base_dmg, int flags, char *narr, int nlen)
{
    (void)gs;
    if (!target || !target->alive) return 0;

    /* --- Type effectiveness --- */
    float type_eff = 1.0f;
    if (!(flags & DMG_IGNORE_TYPE))
        type_eff = get_type_effectiveness(attacker->char_type, target->char_type);
    if (type_eff > 1.0f)
        narr_append(narr, nlen, "It's super effective! ");

    int dmg = (int)(base_dmg * type_eff);

    /* --- Attacker modifiers --- */
    if (attacker->status.attack_debuff_pct > 0)
        dmg = dmg * (100 - attacker->status.attack_debuff_pct) / 100;
    if (attacker->status.damage_boost_pct > 0)
        dmg = dmg * (100 + attacker->status.damage_boost_pct) / 100;

    /* --- Can bypass defenses? --- */
    int bypass = (flags & DMG_UNBLOCKABLE) ||
                 attacker->status.piazza_endorsed;

    /* --- Forced miss (Dangling Pointer) --- */
    if (!bypass && attacker->status.forced_miss) {
        attacker->status.forced_miss = 0;
        narr_append(narr, nlen, "The attack misses! (Dangling Pointer) ");
        return 0;
    }

    /* --- Academic Integrity Check (Karen) --- */
    if (target->status.academic_integrity && (flags & DMG_HAS_SPECIAL) && !bypass) {
        narr_append(narr, nlen, "Academic Integrity Check! Special effect nullified, "
                    "%s takes 25 damage! ", attacker->name);
        attacker->hp -= 25;
        if (attacker->hp < 0) attacker->hp = 0;
        flags &= ~DMG_HAS_SPECIAL;
    }

    /* --- Check the Syllabus (TA counter) --- */
    if (!bypass && target->status.counter_active && !target->status.counter_used) {
        target->status.counter_used = 1;
        narr_append(narr, nlen, "Check the Syllabus! %s counters for 30 damage! ",
                    target->name);
        attacker->hp -= 30;
        if (attacker->hp <= 0) {
            attacker->hp = 0;
            narr_append(narr, nlen, "%s was eliminated by the counter! ", attacker->name);
            return 0;
        }
        narr_append(narr, nlen, "The attack is redirected! ");
        return 0;
    }

    /* --- Invulnerability --- */
    if (!bypass && target->status.invulnerable) {
        narr_append(narr, nlen, "%s is invulnerable! No damage! ", target->name);
        return 0;
    }

    /* --- Evasion --- */
    if (!bypass) {
        if (target->status.evasion_chance > 0 &&
            (rand() % 100) < target->status.evasion_chance) {
            narr_append(narr, nlen, "%s dodged! ", target->name);
            if (target->status.heal_on_dodge) {
                target->hp += 10;
                if (target->hp > target->max_hp) target->hp = target->max_hp;
                narr_append(narr, nlen, "(Shuttle Buses heals 10 HP) ");
            }
            return 0;
        }
    }

    /* --- Shield --- */
    if (!bypass && !(flags & DMG_BYPASS_BUFFS) && target->status.shield_hp > 0) {
        if (dmg <= target->status.shield_hp) {
            target->status.shield_hp -= dmg;
            narr_append(narr, nlen, "Shield absorbs all %d damage! ", dmg);
            return 0;
        }
        int absorbed = target->status.shield_hp;
        dmg -= absorbed;
        target->status.shield_hp = 0;
        narr_append(narr, nlen, "Shield absorbs %d damage! ", absorbed);
    }

    /* --- Love Thy Neighbour reduction --- */
    if (target->status.love_damage_red_turns > 0)
        dmg = dmg * 70 / 100;

    /* --- Man Page permanent reduction --- */
    if (target->status.permanent_dmg_reduction_pct > 0)
        dmg = dmg * (100 - target->status.permanent_dmg_reduction_pct) / 100;

    /* --- Buck-a-Beer defense debuff (target takes MORE damage) --- */
    if (target->status.defense_debuff_pct > 0)
        dmg = dmg * (100 + target->status.defense_debuff_pct) / 100;

    if (dmg < 1) dmg = 1;

    /* --- Turn the Other Cheek --- */
    int toc_heal = 0;
    if (target->status.damage_to_healing) {
        toc_heal = dmg / 2;
        dmg -= toc_heal;
        if (dmg < 0) dmg = 0;
    }

    /* --- Apply damage --- */
    target->hp -= dmg;

    if (toc_heal > 0) {
        target->hp += toc_heal;
        if (target->hp > target->max_hp) target->hp = target->max_hp;
    }

    /* --- Divine mark: prevent death --- */
    if (target->hp <= 0 && target->status.divine_mark) {
        target->hp = 1;
        narr_append(narr, nlen, "%s lost %d HP but Divine Intervention saves them at 1 HP!",
                    target->name, dmg);
        if (toc_heal > 0)
            narr_append(narr, nlen, " (healed %d via Turn the Other Cheek)", toc_heal);
        return dmg;
    }

    if (target->hp < 0) target->hp = 0;

    /* --- Narrate result --- */
    narr_append(narr, nlen, "%s lost %d HP", target->name, dmg);
    if (toc_heal > 0)
        narr_append(narr, nlen, " (healed %d via Turn the Other Cheek)", toc_heal);
    if (target->hp <= 0)
        narr_append(narr, nlen, " and was eliminated!");

    return dmg;
}

/*
 * apply_healing: heals a player, respecting blocked/reduction.
 * Returns actual healing done.
 */
static int apply_healing(Player *p, int amount, char *narr, int nlen)
{
    if (p->status.healing_blocked_turns > 0) {
        narr_append(narr, nlen, " Healing was blocked!");
        return 0;
    }
    if (p->status.healing_reduction_pct > 0)
        amount = amount * (100 - p->status.healing_reduction_pct) / 100;
    if (amount < 1) amount = 1;
    int old = p->hp;
    p->hp += amount;
    if (p->hp > p->max_hp) p->hp = p->max_hp;
    int actual = p->hp - old;
    if (actual > 0)
        narr_append(narr, nlen, " Restored %d HP.", actual);
    p->status.healed_this_turn = 1;
    return actual;
}

/* =========================================================================
 * Individual move implementations
 * ========================================================================= */

/* --- Albert Guo (0) --- */

static void move_gradient_descent(Player *a, Player *t, GameState *gs,
                                  char *n, int nl)
{
    narr_append(n, nl, "%s used Gradient Descent! ", a->name);
    int dmg = apply_damage(a, t, gs, 25, DMG_HAS_SPECIAL, n, nl);
    if (dmg > 0 && t && t->alive && t->status.status_immune_turns <= 0) {
        t->status.attack_debuff_pct += 10;
        if (t->status.attack_debuff_pct > 100) t->status.attack_debuff_pct = 100;
        narr_append(n, nl, " %s's attack reduced by 10%%!", t->name);
    }
}

static void move_bioinformatics(Player *a, Player *t, GameState *gs,
                                char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s used Bioinformatics Synthesis!", a->name);
    int heal = a->max_hp * 30 / 100;
    apply_healing(a, heal, n, nl);
    if (a->status.status_immune_turns < 2)
        a->status.status_immune_turns = 2;
    narr_append(n, nl, " Status immunity for 2 turns!");
}

static void move_overfit_model(Player *a, Player *t, GameState *gs,
                               char *n, int nl)
{
    narr_append(n, nl, "%s used Overfit Model! ", a->name);
    int self_dmg = a->hp * 15 / 100;
    if (self_dmg < 1) self_dmg = 1;
    a->hp -= self_dmg;
    narr_append(n, nl, "(-%d HP self-damage) ", self_dmg);
    if (a->hp <= 0) {
        a->hp = 0;
        narr_append(n, nl, "%s knocked themselves out!", a->name);
        return;
    }
    apply_damage(a, t, gs, 40, DMG_UNBLOCKABLE, n, nl);
}

static void move_data_pipeline(Player *a, Player *t, GameState *gs,
                               char *n, int nl)
{
    narr_append(n, nl, "%s used Data Pipeline! ", a->name);
    int dmg = apply_damage(a, t, gs, 20, DMG_IGNORE_TYPE, n, nl);
    if (dmg > 0) {
        a->hp += dmg;
        /* Data Pipeline can exceed max HP */
        narr_append(n, nl, " %s steals %d HP!", a->name, dmg);
    }
}

/* --- Kaitlyn Zhu (1) --- */

static void move_stab(Player *a, Player *t, GameState *gs,
                      char *n, int nl)
{
    narr_append(n, nl, "%s used Stab! ", a->name);
    apply_damage(a, t, gs, 30, 0, n, nl);
}

static void move_crncr(Player *a, Player *t, GameState *gs,
                       char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s used CR/NCR! Invulnerable this round!", a->name);
}

static void move_drug_mixing(Player *a, Player *t, GameState *gs,
                             char *n, int nl)
{
    narr_append(n, nl, "%s used Drug Mixing on %s! ", a->name,
                (t && t->alive) ? t->name : "nobody");
    if (!t || !t->alive) return;

    int outcome = rand() % 4;
    switch (outcome) {
    case 0:
        narr_append(n, nl, "Bad trip! ");
        apply_damage(a, t, gs, 40, DMG_HAS_SPECIAL, n, nl);
        break;
    case 1:
        if (t->status.status_immune_turns <= 0) {
            t->status.skip_next_turn = 1;
            narr_append(n, nl, "%s will skip their next turn!", t->name);
        } else {
            narr_append(n, nl, "%s is immune to the stun!", t->name);
        }
        break;
    case 2:
        if (t->status.status_immune_turns <= 0) {
            t->status.poison_turns = 3;
            t->status.poison_damage = 15;
            narr_append(n, nl, "%s was poisoned! (15 dmg/turn for 3 turns)", t->name);
        } else {
            narr_append(n, nl, "%s is immune to poison!", t->name);
        }
        break;
    case 3:
        narr_append(n, nl, "But nothing happened!");
        break;
    }
}

static void move_pizza_fries(Player *a, Player *t, GameState *gs,
                             char *n, int nl)
{
    narr_append(n, nl, "%s gives poor skiing lessons! ", a->name);
    if (!t || !t->alive) return;
    int target_speed = game_get_current_speed(t);
    int dmg = (int)ceil(2.25 * target_speed);
    apply_damage(a, t, gs, dmg, 0, n, nl);
}

/* --- Yibin Wang (2) --- */

static void move_undef_behaviour(Player *a, Player *t, GameState *gs,
                                 char *n, int nl)
{
    narr_append(n, nl, "%s used Undefined Behaviour! ", a->name);
    int dmg = apply_damage(a, t, gs, 25, DMG_HAS_SPECIAL, n, nl);
    if (dmg > 0 && t && t->alive && t->status.status_immune_turns <= 0) {
        t->status.scrambled = 1;
        narr_append(n, nl, " %s's next move will be random!", t->name);
    }
}

static void move_dangling_pointer(Player *a, Player *t, GameState *gs,
                                  char *n, int nl)
{
    (void)gs;
    narr_append(n, nl, "%s used Dangling Pointer on %s! ",
                a->name, (t && t->alive) ? t->name : "nobody");
    if (!t || !t->alive) return;
    if (t->status.status_immune_turns <= 0) {
        t->status.forced_miss = 1;
        narr_append(n, nl, "%s's next attack will miss!", t->name);
    } else {
        narr_append(n, nl, "But %s is immune!", t->name);
    }
}

static void move_null_deref(Player *a, Player *t, GameState *gs,
                            char *n, int nl)
{
    narr_append(n, nl, "%s used Null Dereference on %s! ",
                a->name, (t && t->alive) ? t->name : "nobody");
    if (!t || !t->alive) return;

    if (t->hp > 0 && t->hp <= t->max_hp / 4) {
        narr_append(n, nl, "SEGFAULT! ");
        if (t->status.invulnerable) {
            narr_append(n, nl, "%s is invulnerable! Execution failed!", t->name);
        } else if (t->status.divine_mark) {
            t->hp = 1;
            narr_append(n, nl, "Divine Intervention saves %s at 1 HP!", t->name);
        } else {
            t->hp = 0;
            narr_append(n, nl, "%s was executed! Eliminated!", t->name);
        }
    } else {
        apply_damage(a, t, gs, 20, 0, n, nl);
    }
}

/* --- Doug Ford (3) --- */

static void move_osap_cuts(Player *a, Player *t, GameState *gs,
                           char *n, int nl)
{
    narr_append(n, nl, "%s used OSAP Cuts! ", a->name);
    if (!t || !t->alive) return;
    int base = 35;
    if (t->char_type == TYPE_STUDENT) base = 17;
    apply_damage(a, t, gs, base, 0, n, nl);
}

static void move_notwithstanding(Player *a, Player *t, GameState *gs,
                                 char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s invoked the Notwithstanding Clause! "
                "Debuffs cleared, 50%% evasion this round!", a->name);
}

static void move_filibuster(Player *a, Player *t, GameState *gs,
                            char *n, int nl)
{
    (void)t;
    narr_append(n, nl, "%s used Filibuster!", a->name);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &gs->players[i];
        if (!p->alive || p->id == a->id) continue;
        narr_append(n, nl, "\n  ");
        apply_damage(a, p, gs, 15, 0, n, nl);
    }
}

static void move_buck_a_beer(Player *a, Player *t, GameState *gs,
                             char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s opens a Buck-a-Beer!", a->name);
    apply_healing(a, 40, n, nl);
    a->status.defense_debuff_pct += 15;
    narr_append(n, nl, " Defense permanently reduced by 15%%!");
}

/* --- TTC (4) --- */

static void move_planned_trackwork(Player *a, Player *t, GameState *gs,
                                   char *n, int nl)
{
    narr_append(n, nl, "%s used Planned Trackwork! ", a->name);
    apply_damage(a, t, gs, 25, 0, n, nl);
}

static void move_shuttle_buses(Player *a, Player *t, GameState *gs,
                               char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s deployed Shuttle Buses! "
                "50%% evasion for 2 turns, heals 10 on dodge!", a->name);
}

static void move_fare_inspector(Player *a, Player *t, GameState *gs,
                                char *n, int nl)
{
    narr_append(n, nl, "%s sent the Fare Inspector! ", a->name);
    if (!t || !t->alive) return;
    int base = t->hp * 20 / 100;
    if (base < 1) base = 1;
    int dmg = apply_damage(a, t, gs, base, DMG_HAS_SPECIAL | DMG_IGNORE_TYPE, n, nl);
    if (dmg > 0 && t->alive && t->status.status_immune_turns <= 0) {
        t->status.healing_blocked_turns = 1;
        narr_append(n, nl, " %s can't heal next turn!", t->name);
    }
}

static void move_slow_zone(Player *a, Player *t, GameState *gs,
                           char *n, int nl)
{
    (void)gs;
    narr_append(n, nl, "%s used Slow Zone on %s! ",
                a->name, (t && t->alive) ? t->name : "nobody");
    if (!t || !t->alive) return;
    if (t->status.status_immune_turns <= 0) {
        t->status.speed_reduction_pct += 25;
        if (t->status.speed_reduction_pct > 90)
            t->status.speed_reduction_pct = 90;
        narr_append(n, nl, "%s's speed reduced by 25%%! (now %d)",
                    t->name, game_get_current_speed(t));
    } else {
        narr_append(n, nl, "But %s is immune!", t->name);
    }
}

/* --- The Bitter TA (5) --- */

static void move_deduct_marks(Player *a, Player *t, GameState *gs,
                              char *n, int nl)
{
    narr_append(n, nl, "%s used Deduct Marks! ", a->name);
    int dmg = apply_damage(a, t, gs, 25, DMG_BYPASS_BUFFS | DMG_HAS_SPECIAL, n, nl);
    if (dmg > 0 && t && t->alive && t->status.status_immune_turns <= 0) {
        t->status.healing_reduction_pct = 50;
        t->status.healing_reduction_turns = 2;
        narr_append(n, nl, " %s's healing halved for 2 turns!", t->name);
    }
}

static void move_check_syllabus(Player *a, Player *t, GameState *gs,
                                char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s readies Check the Syllabus! "
                "Ready to counter the first attacker!", a->name);
}

static void move_extension_denied(Player *a, Player *t, GameState *gs,
                                  char *n, int nl)
{
    narr_append(n, nl, "%s used Extension Denied! ", a->name);
    if (!t || !t->alive) return;
    int base = 25;
    if (t->status.healed_last_turn) {
        base = 50;
        narr_append(n, nl, "Double damage! (target healed last turn) ");
    }
    apply_damage(a, t, gs, base, 0, n, nl);
}

static void move_office_hours(Player *a, Player *t, GameState *gs,
                              char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s holds Office Hours!", a->name);
    apply_healing(a, 15, n, nl);
    a->status.shield_next_round = 25;
    narr_append(n, nl, " Will gain a 25 HP shield next round!");
}

/* --- Jesus Christ (6) --- */

static void move_table_flip(Player *a, Player *t, GameState *gs,
                            char *n, int nl)
{
    (void)t;
    narr_append(n, nl, "%s flipped the table! All effects cleared!", a->name);
    /* Clear ALL status effects from ALL players (except permanent buffs?) */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &gs->players[i];
        if (!p->alive) continue;
        /* Preserve permanent stats, clear everything else */
        int saved_atk_debuff = p->status.attack_debuff_pct;
        int saved_def_debuff = p->status.defense_debuff_pct;
        int saved_dmg_boost  = p->status.damage_boost_pct;
        int saved_dmg_red    = p->status.permanent_dmg_reduction_pct;
        int saved_speed_red  = p->status.speed_reduction_pct;
        int saved_autotester = p->status.autotester_streak;
        int saved_auto_tgt   = p->status.autotester_last_target;
        memset(&p->status, 0, sizeof(StatusEffects));
        p->status.attack_debuff_pct = saved_atk_debuff;
        p->status.defense_debuff_pct = saved_def_debuff;
        p->status.damage_boost_pct = saved_dmg_boost;
        p->status.permanent_dmg_reduction_pct = saved_dmg_red;
        p->status.speed_reduction_pct = saved_speed_red;
        p->status.autotester_streak = saved_autotester;
        p->status.autotester_last_target = saved_auto_tgt;
    }
    /* AoE 15 damage */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &gs->players[i];
        if (!p->alive || p->id == a->id) continue;
        narr_append(n, nl, "\n  ");
        /* All effects just cleared so damage is straightforward */
        float eff = get_type_effectiveness(a->char_type, p->char_type);
        int dmg = (int)(15 * eff);
        if (a->status.attack_debuff_pct > 0)
            dmg = dmg * (100 - a->status.attack_debuff_pct) / 100;
        if (a->status.damage_boost_pct > 0)
            dmg = dmg * (100 + a->status.damage_boost_pct) / 100;
        if (p->status.defense_debuff_pct > 0)
            dmg = dmg * (100 + p->status.defense_debuff_pct) / 100;
        if (dmg < 1) dmg = 1;
        p->hp -= dmg;
        if (p->hp < 0) p->hp = 0;
        if (eff > 1.0f)
            narr_append(n, nl, "It's super effective! ");
        narr_append(n, nl, "%s lost %d HP", p->name, dmg);
        if (p->hp <= 0)
            narr_append(n, nl, " - eliminated!");
    }
}

static void move_turn_other_cheek(Player *a, Player *t, GameState *gs,
                                  char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s turns the other cheek! "
                "50%% of damage taken becomes healing this round!", a->name);
}

static void move_divine_intervention(Player *a, Player *t, GameState *gs,
                                     char *n, int nl)
{
    (void)gs;
    const char *tname = (t && t->alive) ? t->name : a->name;
    narr_append(n, nl, "%s used Divine Intervention on %s! "
                "Protected from death this round!", a->name, tname);
}

static void move_love_thy_neighbour(Player *a, Player *t, GameState *gs,
                                    char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s used Love Thy Neighbour! "
                "-30%% incoming damage for 2 turns, but can't deal damage!",
                a->name);
}

/* --- Karen Reid (7) --- */

static void move_autotester(Player *a, Player *t, GameState *gs,
                            char *n, int nl)
{
    narr_append(n, nl, "%s runs The Autotester! ", a->name);
    if (!t || !t->alive) return;

    if (a->status.autotester_last_target == (int)t->id)
        a->status.autotester_streak++;
    else {
        a->status.autotester_streak = 1;
        a->status.autotester_last_target = (int)t->id;
    }

    int base = a->status.autotester_streak * 15;
    if (base > 45) base = 45;
    narr_append(n, nl, "Test %d: ", a->status.autotester_streak);
    int dmg = apply_damage(a, t, gs, base, 0, n, nl);
    if (dmg == 0) {
        a->status.autotester_streak = 0;
        a->status.autotester_last_target = -1;
    }
}

static void move_piazza_endorsed(Player *a, Player *t, GameState *gs,
                                 char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s used Piazza Endorsement!", a->name);
    apply_healing(a, 20, n, nl);
    a->status.piazza_endorsed = 1;
    narr_append(n, nl, " Next attack is guaranteed to hit!");
}

static void move_academic_integrity(Player *a, Player *t, GameState *gs,
                                    char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s activates Academic Integrity Check! "
                "Special effects will be reflected!", a->name);
}

static void move_man_page_reading(Player *a, Player *t, GameState *gs,
                                  char *n, int nl)
{
    (void)t; (void)gs;
    narr_append(n, nl, "%s reads the man pages!", a->name);
    a->status.permanent_dmg_reduction_pct += 15;
    a->status.damage_boost_pct += 15;
    narr_append(n, nl, " Permanently gained 15%% damage reduction and 15%% damage boost!");
}

/* =========================================================================
 * Pre-apply defensive moves (effects active for the whole round)
 * ========================================================================= */

static void pre_apply_defenses(GameState *gs)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &gs->players[i];
        if (!p->alive || p->pending_action_id < 0) continue;
        if (p->char_id < 0 || p->char_id >= NUM_CHARACTERS) continue;

        int cid = p->char_id;
        int mid = p->pending_action_id;
        if (mid >= char_defs[cid].num_moves) continue;
        if (!char_defs[cid].moves[mid].is_defensive) continue;

        switch (cid) {
        case 1: /* Kaitlyn - CR/NCR */
            if (mid == 1)
                p->status.invulnerable = 1;
            break;
        case 3: /* Doug - Notwithstanding Clause */
            if (mid == 1) {
                /* Clear debuffs */
                p->status.attack_debuff_pct = 0;
                p->status.poison_turns = 0;
                p->status.poison_damage = 0;
                p->status.scrambled = 0;
                p->status.forced_miss = 0;
                p->status.skip_next_turn = 0;
                p->status.speed_reduction_pct = 0;
                p->status.healing_reduction_pct = 0;
                p->status.healing_reduction_turns = 0;
                p->status.healing_blocked_turns = 0;
                p->status.defense_debuff_pct = 0;
                /* 50% evasion this round */
                p->status.evasion_chance = 50;
                p->status.evasion_turns = 1;
            }
            break;
        case 4: /* TTC - Shuttle Buses */
            if (mid == 1) {
                p->status.evasion_chance = 50;
                p->status.evasion_turns = 2;
                p->status.heal_on_dodge = 1;
            }
            break;
        case 5: /* TA - Check the Syllabus */
            if (mid == 1) {
                p->status.counter_active = 1;
                p->status.counter_used = 0;
            }
            break;
        case 6: /* Jesus */
            if (mid == 1) { /* Turn the Other Cheek */
                p->status.damage_to_healing = 1;
            }
            if (mid == 2) { /* Divine Intervention */
                int tid = p->pending_target_id;
                if (tid != NO_TARGET && tid >= 0 && tid < MAX_PLAYERS
                    && gs->players[tid].alive)
                    gs->players[tid].status.divine_mark = 1;
                else
                    p->status.divine_mark = 1;
            }
            if (mid == 3) { /* Love Thy Neighbour */
                p->status.love_damage_red_turns = 2;
                p->status.love_no_damage_turns = 2;
            }
            break;
        case 7: /* Karen - Academic Integrity Check */
            if (mid == 2)
                p->status.academic_integrity = 1;
            break;
        }
    }
}

/* =========================================================================
 * Update cooldowns after resolution
 * ========================================================================= */

static void update_cooldowns(GameState *gs)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &gs->players[i];
        if (!p->alive || p->char_id < 0) continue;

        /* CR/NCR cooldown */
        p->status.crncr_cooldown =
            (p->char_id == 1 && p->pending_action_id == 1) ? 1 : 0;
        /* Turn the Other Cheek cooldown */
        p->status.toc_cooldown =
            (p->char_id == 6 && p->pending_action_id == 1) ? 1 : 0;
        /* Divine Intervention cooldown */
        p->status.di_cooldown =
            (p->char_id == 6 && p->pending_action_id == 2) ? 1 : 0;
    }
}

/* =========================================================================
 * Resolution sort comparator
 * ========================================================================= */

typedef struct {
    int player_idx;
    int priority;       /* 0=always first, 1=defensive, 2=normal, 3=idle */
    int speed;
    int rng;            /* random tiebreak */
} ResolveSlot;

static int resolve_cmp(const void *a, const void *b)
{
    const ResolveSlot *sa = a;
    const ResolveSlot *sb = b;

    if (sa->priority != sb->priority)
        return sa->priority - sb->priority;
    if (sa->speed != sb->speed)
        return sb->speed - sa->speed;   /* higher speed first */
    return sa->rng - sb->rng;
}

/* =========================================================================
 * game_resolve_round
 * ========================================================================= */

int game_resolve_round(GameState *gs)
{
    /* Auto-idle any alive player who never submitted */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].alive && gs->players[i].pending_action_id == -1) {
            gs->players[i].pending_action_id = -2; /* idle */
            gs->players[i].pending_target_id = NO_TARGET;
        }
    }

    /* Pre-apply all defensive moves */
    pre_apply_defenses(gs);

    /* Build resolution order */
    ResolveSlot slots[MAX_PLAYERS];
    int slot_count = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &gs->players[i];
        if (!p->alive) continue;

        slots[slot_count].player_idx = i;
        slots[slot_count].rng = rand() % 10000;

        if (p->pending_action_id == -2) {
            /* idle */
            slots[slot_count].priority = 3;
            slots[slot_count].speed = 0;
        } else {
            int cid = p->char_id;
            int mid = p->pending_action_id;

            if (char_defs[cid].moves[mid].is_priority) {
                slots[slot_count].priority = 0;
                slots[slot_count].speed = p->base_speed;
            } else if (char_defs[cid].moves[mid].is_defensive) {
                slots[slot_count].priority = 1;
                slots[slot_count].speed = game_get_current_speed(p);
            } else {
                slots[slot_count].priority = 2;
                slots[slot_count].speed = game_get_current_speed(p);
            }
        }
        slot_count++;
    }

    qsort(slots, slot_count, sizeof(ResolveSlot), resolve_cmp);

    /* Resolve each move */
    int event_count = 0;

    for (int s = 0; s < slot_count && event_count < MAX_EVENTS; s++) {
        int pidx = slots[s].player_idx;
        Player *p = &gs->players[pidx];

        if (!p->alive) continue;

        int mid = p->pending_action_id;
        char *narr = gs->resolve_events[event_count];
        narr[0] = '\0';

        /* Idle */
        if (mid == -2) {
            narr_append(narr, EVENT_TEXT_LEN, "%s does nothing this turn.", p->name);
            event_count++;
            continue;
        }

        /* Forced skip */
        if (p->status.skip_next_turn) {
            p->status.skip_next_turn = 0;
            narr_append(narr, EVENT_TEXT_LEN,
                        "%s is stunned and can't move!", p->name);
            event_count++;
            continue;
        }

        /* Scrambled: random move override */
        if (p->status.scrambled) {
            p->status.scrambled = 0;
            int num_moves = char_defs[p->char_id].num_moves;
            mid = rand() % num_moves;
            p->pending_action_id = mid;
            if (char_defs[p->char_id].moves[mid].requires_target == 1 &&
                (p->pending_target_id == NO_TARGET ||
                 p->pending_target_id >= MAX_PLAYERS ||
                 !gs->players[p->pending_target_id].alive ||
                 (int)p->pending_target_id == pidx)) {
                /* Pick a random alive enemy */
                for (int j = 0; j < MAX_PLAYERS; j++) {
                    if (gs->players[j].alive && j != pidx) {
                        p->pending_target_id = j;
                        break;
                    }
                }
            }
        }

        /* Love Thy Neighbour restriction */
        if (p->status.love_no_damage_turns > 0) {
            int cid = p->char_id;
            if (char_defs[cid].moves[mid].requires_target == 1 &&
                !char_defs[cid].moves[mid].is_defensive) {
                narr_append(narr, EVENT_TEXT_LEN,
                            "%s can't use damaging moves (Love Thy Neighbour)! Turn wasted.",
                            p->name);
                event_count++;
                continue;
            }
        }

        /* Get target */
        Player *target = NULL;
        int tid = p->pending_target_id;
        if (tid != NO_TARGET && tid < MAX_PLAYERS)
            target = &gs->players[tid];
        if (target && !target->alive) target = NULL;

        /* Execute move */
        int cid = p->char_id;
        if (cid >= 0 && cid < NUM_CHARACTERS &&
            mid >= 0 && mid < MOVES_PER_CHAR && move_fns[cid][mid]) {
            move_fns[cid][mid](p, target, gs, narr, EVENT_TEXT_LEN);
        } else {
            narr_append(narr, EVENT_TEXT_LEN, "%s does nothing.", p->name);
        }

        /* Consume piazza endorsed on offensive moves */
        if (p->status.piazza_endorsed &&
            char_defs[cid].moves[mid].requires_target == 1) {
            p->status.piazza_endorsed = 0;
        }

        event_count++;
    }

    /* Update cooldowns */
    update_cooldowns(gs);

    /* Mark eliminated players */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].alive && gs->players[i].hp <= 0) {
            gs->players[i].alive = 0;
            gs->alive_count--;
        }
    }

    gs->resolve_event_count = event_count;
    gs->resolve_event_current = 0;
    return event_count;
}

/* =========================================================================
 * game_init
 * ========================================================================= */

void game_init(GameState *gs)
{
    memset(gs, 0, sizeof(*gs));

    for (int i = 0; i < MAX_PLAYERS; i++) {
        gs->players[i].fd = -1;
        gs->players[i].id = (uint8_t)i;
        gs->players[i].alive = 0;
        gs->players[i].char_id = -1;
        gs->players[i].pending_action_id = -1;
        gs->players[i].pending_target_id = NO_TARGET;
        gs->players[i].status.autotester_last_target = -1;
    }

    gs->phase = STATE_LOBBY;
    gs->round_num = 0;
    gs->alive_count = 0;
    gs->host_id = -1;

    memset(gs->char_taken, 0, sizeof(gs->char_taken));
}

/* =========================================================================
 * game_add_player
 * ========================================================================= */

int game_add_player(GameState *gs, int fd, const char *name)
{
    if (gs->player_count >= MAX_PLAYERS) return -1;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gs->players[i].fd == fd) {
            Player *p = &gs->players[i];
            p->fd = fd;
            p->alive = 1;
            p->char_id = -1;
            p->pending_action_id = -1;
            p->pending_target_id = NO_TARGET;
            memset(&p->status, 0, sizeof(StatusEffects));
            p->status.autotester_last_target = -1;
            strncpy(p->name, name, PLAYER_NAME_LEN - 1);
            p->name[PLAYER_NAME_LEN - 1] = '\0';

            gs->player_count++;
            gs->alive_count++;
            return i;
        }
    }
    return -1;
}

/* =========================================================================
 * game_assign_character
 * ========================================================================= */

int game_assign_character(GameState *gs, int player_id, int char_id)
{
    if (char_id < 0 || char_id >= NUM_CHARACTERS) return -1;
    if (gs->char_taken[char_id]) return -1;

    Player *p = &gs->players[player_id];

    /* Release previously selected character */
    if (p->char_id >= 0 && p->char_id < NUM_CHARACTERS)
        gs->char_taken[p->char_id] = 0;

    gs->char_taken[char_id] = 1;
    p->char_id = char_id;
    p->char_type = char_defs[char_id].type;
    p->max_hp = char_defs[char_id].base_hp;
    p->hp = p->max_hp;
    p->base_speed = char_defs[char_id].base_speed;

    /* Copy character name */
    strncpy(p->name, char_defs[char_id].name, PLAYER_NAME_LEN - 1);
    p->name[PLAYER_NAME_LEN - 1] = '\0';

    return 0;
}

/* =========================================================================
 * game_remove_player
 * ========================================================================= */

void game_remove_player(GameState *gs, int player_id)
{
    Player *p = &gs->players[player_id];
    if (p->fd == -1) return;

    if (p->char_id >= 0 && p->char_id < NUM_CHARACTERS)
        gs->char_taken[p->char_id] = 0;

    p->fd = -1;
    p->alive = 0;
    p->hp = 0;
    p->char_id = -1;
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
        if (p->alive && p->fd != -1 && p->pending_action_id == -1)
            return 0;
    }
    return 1;
}

/* =========================================================================
 * game_check_end
 * ========================================================================= */

int game_check_end(const GameState *gs, int *winner_id)
{
    *winner_id = -1;
    if (gs->alive_count == 0) return 2;
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
        out[n].hp = (uint8_t)(p->hp < 0 ? 0 : (p->hp > 255 ? 255 : p->hp));
        out[n].alive = (uint8_t)p->alive;
        out[n].char_type = (uint8_t)p->char_type;
        out[n].speed = (uint8_t)game_get_current_speed(p);
        out[n].max_hp = (uint8_t)(p->max_hp > 255 ? 255 : p->max_hp);
        out[n].char_id = (p->char_id >= 0) ? (uint8_t)p->char_id : NO_CHARACTER;
        out[n].padding = 0;
        strncpy(out[n].name, p->name, PLAYER_NAME_LEN - 1);
        out[n].name[PLAYER_NAME_LEN - 1] = '\0';
        n++;
    }
    *count = n;
}

/* =========================================================================
 * game_tick_effects
 * ========================================================================= */

void game_tick_effects(GameState *gs, char *tick_text, int text_len)
{
    tick_text[0] = '\0';

    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &gs->players[i];
        if (!p->alive) continue;
        StatusEffects *s = &p->status;

        /* Clear single-round effects from last round */
        s->invulnerable = 0;
        s->damage_to_healing = 0;
        s->divine_mark = 0;
        s->counter_active = 0;
        s->counter_used = 0;
        s->academic_integrity = 0;
        s->shield_hp = 0;

        /* Apply shield from last round (Office Hours) */
        if (s->shield_next_round > 0) {
            s->shield_hp = s->shield_next_round;
            s->shield_next_round = 0;
            narr_append(tick_text, text_len,
                        "%s gains a %d HP shield!\n", p->name, s->shield_hp);
        }

        /* Poison tick */
        if (s->poison_turns > 0) {
            p->hp -= s->poison_damage;
            s->poison_turns--;
            narr_append(tick_text, text_len,
                        "%s takes %d poison damage! (%d HP, %d turns left)\n",
                        p->name, s->poison_damage,
                        p->hp < 0 ? 0 : p->hp, s->poison_turns);
            if (s->poison_turns == 0) s->poison_damage = 0;
        }

        /* Decrement turn-based effects */
        if (s->status_immune_turns > 0) s->status_immune_turns--;
        if (s->evasion_turns > 0) {
            s->evasion_turns--;
            if (s->evasion_turns == 0) {
                s->evasion_chance = 0;
                s->heal_on_dodge = 0;
            }
        }
        if (s->healing_blocked_turns > 0) s->healing_blocked_turns--;
        if (s->healing_reduction_turns > 0) {
            s->healing_reduction_turns--;
            if (s->healing_reduction_turns == 0) s->healing_reduction_pct = 0;
        }
        if (s->love_damage_red_turns > 0) s->love_damage_red_turns--;
        if (s->love_no_damage_turns > 0) s->love_no_damage_turns--;

        /* Track healed_last_turn for Extension Denied */
        s->healed_last_turn = s->healed_this_turn;
        s->healed_this_turn = 0;

        /* Check death from poison */
        if (p->hp <= 0) {
            p->hp = 0;
            p->alive = 0;
            gs->alive_count--;
            narr_append(tick_text, text_len,
                        "%s was eliminated by poison!\n", p->name);
        }
    }
}

/* =========================================================================
 * game_build_status_text
 * ========================================================================= */

void game_build_status_text(const GameState *gs, int player_id,
                            char *out, int out_len)
{
    const Player *p = &gs->players[player_id];
    const StatusEffects *s = &p->status;
    out[0] = '\0';

    if (s->poison_turns > 0)
        narr_append(out, out_len, "Poisoned (%d dmg, %d turns) ",
                    s->poison_damage, s->poison_turns);
    if (s->attack_debuff_pct > 0)
        narr_append(out, out_len, "Atk -%d%% ", s->attack_debuff_pct);
    if (s->defense_debuff_pct > 0)
        narr_append(out, out_len, "Def -%d%% ", s->defense_debuff_pct);
    if (s->speed_reduction_pct > 0)
        narr_append(out, out_len, "Speed -%d%% ", s->speed_reduction_pct);
    if (s->evasion_turns > 0)
        narr_append(out, out_len, "Evasion %d%% (%d turns) ",
                    s->evasion_chance, s->evasion_turns);
    if (s->shield_hp > 0)
        narr_append(out, out_len, "Shield %d HP ", s->shield_hp);
    if (s->status_immune_turns > 0)
        narr_append(out, out_len, "Status immune (%d turns) ",
                    s->status_immune_turns);
    if (s->healing_reduction_turns > 0)
        narr_append(out, out_len, "Healing -%d%% (%d turns) ",
                    s->healing_reduction_pct, s->healing_reduction_turns);
    if (s->healing_blocked_turns > 0)
        narr_append(out, out_len, "Healing BLOCKED (%d turns) ",
                    s->healing_blocked_turns);
    if (s->love_damage_red_turns > 0)
        narr_append(out, out_len, "Love Thy Neighbour (%d turns) ",
                    s->love_damage_red_turns);
    if (s->skip_next_turn)
        narr_append(out, out_len, "STUNNED next turn ");
    if (s->scrambled)
        narr_append(out, out_len, "Next move SCRAMBLED ");
    if (s->forced_miss)
        narr_append(out, out_len, "Next attack MISSES ");
    if (s->damage_boost_pct > 0)
        narr_append(out, out_len, "Dmg +%d%% ", s->damage_boost_pct);
    if (s->permanent_dmg_reduction_pct > 0)
        narr_append(out, out_len, "Dmg taken -%d%% ", s->permanent_dmg_reduction_pct);
    if (s->piazza_endorsed)
        narr_append(out, out_len, "Next attack GUARANTEED HIT ");
    if (s->crncr_cooldown)
        narr_append(out, out_len, "[CR/NCR on cooldown] ");
    if (s->toc_cooldown)
        narr_append(out, out_len, "[Turn Other Cheek on cooldown] ");
    if (s->di_cooldown)
        narr_append(out, out_len, "[Divine Intervention on cooldown] ");

    if (out[0] == '\0')
        narr_append(out, out_len, "No active effects.");
}

/* =========================================================================
 * game_build_char_list
 * ========================================================================= */

void game_build_char_list(const GameState *gs, CharEntry *out)
{
    for (int c = 0; c < NUM_CHARACTERS; c++) {
        const CharacterDef *cd = &char_defs[c];
        out[c].char_id = (uint8_t)c;
        out[c].char_type = (uint8_t)cd->type;
        out[c].hp = (uint8_t)cd->base_hp;
        out[c].speed = (uint8_t)cd->base_speed;
        out[c].num_moves = (uint8_t)cd->num_moves;
        out[c].taken = gs->char_taken[c];
        out[c].padding[0] = 0;
        out[c].padding[1] = 0;
        strncpy(out[c].name, cd->name, PLAYER_NAME_LEN - 1);
        out[c].name[PLAYER_NAME_LEN - 1] = '\0';

        memset(out[c].moves, 0, sizeof(out[c].moves));
        for (int m = 0; m < cd->num_moves; m++) {
            out[c].moves[m].action_id = (uint8_t)m;
            out[c].moves[m].requires_target = (uint8_t)cd->moves[m].requires_target;
            strncpy(out[c].moves[m].name, cd->moves[m].name, ACTION_NAME_LEN - 1);
            strncpy(out[c].moves[m].desc, cd->moves[m].desc, ACTION_DESC_LEN - 1);
        }
    }
}

/* =========================================================================
 * game_get_char_actions
 * ========================================================================= */

void game_get_char_actions(int char_id, ActionDef *out, int *count)
{
    if (char_id < 0 || char_id >= NUM_CHARACTERS) {
        *count = 0;
        return;
    }
    const CharacterDef *cd = &char_defs[char_id];
    *count = cd->num_moves;
    memset(out, 0, sizeof(ActionDef) * MOVES_PER_CHAR);
    for (int m = 0; m < cd->num_moves; m++) {
        out[m].action_id = (uint8_t)m;
        out[m].requires_target = (uint8_t)cd->moves[m].requires_target;
        strncpy(out[m].name, cd->moves[m].name, ACTION_NAME_LEN - 1);
        strncpy(out[m].desc, cd->moves[m].desc, ACTION_DESC_LEN - 1);
    }
}

/* =========================================================================
 * game_validate_action
 * ========================================================================= */

int game_validate_action(const GameState *gs, int player_id, int action_id,
                         int target_id, char *err, int err_len)
{
    const Player *p = &gs->players[player_id];
    int cid = p->char_id;

    if (cid < 0 || cid >= NUM_CHARACTERS) {
        snprintf(err, err_len, "No character selected.");
        return -1;
    }

    int num_moves = char_defs[cid].num_moves;
    if (action_id < 0 || action_id >= num_moves) {
        snprintf(err, err_len, "Invalid action (0-%d).", num_moves - 1);
        return -1;
    }

    /* Cooldown checks */
    if (cid == 1 && action_id == 1 && p->status.crncr_cooldown) {
        snprintf(err, err_len, "CR/NCR can't be used back-to-back!");
        return -1;
    }
    if (cid == 6 && action_id == 1 && p->status.toc_cooldown) {
        snprintf(err, err_len, "Turn the Other Cheek can't be used back-to-back!");
        return -1;
    }
    if (cid == 6 && action_id == 2 && p->status.di_cooldown) {
        snprintf(err, err_len, "Divine Intervention can't be used back-to-back!");
        return -1;
    }

    /* Target validation */
    int req = char_defs[cid].moves[action_id].requires_target;
    if (req == 1) {
        /* Must target alive enemy */
        if (target_id == NO_TARGET || target_id >= MAX_PLAYERS
            || !gs->players[target_id].alive || target_id == player_id) {
            snprintf(err, err_len, "Must target an alive opponent.");
            return -1;
        }
    } else if (req == 2) {
        /* Must target any alive player (including self) */
        if (target_id == NO_TARGET || target_id >= MAX_PLAYERS
            || !gs->players[target_id].alive) {
            snprintf(err, err_len, "Must target an alive player.");
            return -1;
        }
    }

    return 0;
}
