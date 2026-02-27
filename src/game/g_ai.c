/*
 * g_ai.c - Monster AI system
 *
 * Provides basic NPC behavior: idle, alert, chase, attack, pain, death.
 * SoF monsters are humanoid soldiers with hitscan weapons.
 *
 * AI state machine:
 *   IDLE -> (sight player) -> ALERT -> CHASE -> (in range) -> ATTACK
 *   Any state -> (take damage) -> PAIN -> (resume previous)
 *   Any state -> (health <= 0) -> DEAD
 */

#include "g_local.h"

#define FRAMETIME   0.1f    /* 10 Hz game frame */

/* Sound constants now in g_local.h */

/* Particle/light effects from renderer (unified binary) */
extern void R_ParticleEffect(vec3_t org, vec3_t dir, int type, int count);
extern void R_AddDlight(vec3_t origin, float r, float g, float b,
                         float intensity, float duration);
extern void R_AddTracer(vec3_t start, vec3_t end, float r, float g, float b);
extern void SCR_AddDamageDirection(float angle);

/* Monster sound indices — precached in monster_start */
static int snd_monster_pain1;
static int snd_monster_pain2;
static int snd_monster_die;
static int snd_monster_sight;
static int snd_monster_fire;
static qboolean monster_sounds_cached;

/* Notify player of damage direction from an attacker */
static void AI_DamageDirectionToPlayer(edict_t *player, vec3_t source)
{
    float dx, dy, yaw, view_yaw, angle;
    if (!player || !player->client) return;
    dx = source[0] - player->s.origin[0];
    dy = source[1] - player->s.origin[1];
    yaw = atan2f(dy, dx) * 180.0f / 3.14159265f;
    view_yaw = player->client->viewangles[1];
    angle = yaw - view_yaw + 180.0f;
    while (angle < 0) angle += 360.0f;
    while (angle >= 360.0f) angle -= 360.0f;
    SCR_AddDamageDirection(angle);
}

/* ==========================================================================
   AI Constants
   ========================================================================== */

#define AI_STAND_GROUND     0x0001
#define AI_TEMP_STAND_GROUND 0x0002
#define AI_SOUND_TARGET     0x0004
#define AI_LOST_SIGHT       0x0008
#define AI_PURSUIT_LAST_SEEN 0x0010
#define AI_PURSUE_NEXT      0x0020
#define AI_PURSUE_TEMP      0x0040

typedef enum {
    AI_STATE_IDLE,
    AI_STATE_ALERT,
    AI_STATE_CHASE,
    AI_STATE_ATTACK,
    AI_STATE_PAIN,
    AI_STATE_DEAD
} ai_state_t;

/* Sight range and attack range */
#define AI_SIGHT_RANGE      1024.0f
#define AI_ATTACK_RANGE     512.0f
#define AI_MELEE_RANGE      80.0f
#define AI_CHASE_SPEED      200.0f
#define AI_PAIN_TIME        0.5f
#define AI_ATTACK_INTERVAL  1.0f

/* ==========================================================================
   AI Utility Functions
   ========================================================================== */

/*
 * AI_Visible - Can this entity see the target?
 * Traces a line from eyes to target, returns true if unobstructed.
 */
static qboolean AI_Visible(edict_t *self, edict_t *target)
{
    vec3_t start, end;
    trace_t tr;

    VectorCopy(self->s.origin, start);
    start[2] += 20;    /* approximate eye height */

    VectorCopy(target->s.origin, end);
    end[2] += 20;

    tr = gi.trace(start, NULL, NULL, end, self, MASK_OPAQUE);
    return (tr.fraction == 1.0f || tr.ent == target);
}

/*
 * AI_Range - Get distance to target
 */
static float AI_Range(edict_t *self, edict_t *target)
{
    vec3_t diff;
    VectorSubtract(target->s.origin, self->s.origin, diff);
    return VectorLength(diff);
}

/*
 * AI_FaceEnemy - Turn toward enemy
 */
static void AI_FaceEnemy(edict_t *self)
{
    vec3_t diff;
    float ideal_yaw;

    if (!self->enemy)
        return;

    VectorSubtract(self->enemy->s.origin, self->s.origin, diff);
    ideal_yaw = (float)(atan2(diff[1], diff[0]) * 180.0 / 3.14159265);
    self->ideal_yaw = ideal_yaw;

    /* Smooth turn toward ideal yaw */
    {
        float current = self->s.angles[1];
        float move = ideal_yaw - current;

        /* Normalize to -180..180 */
        while (move > 180) move -= 360;
        while (move < -180) move += 360;

        if (move > self->yaw_speed)
            move = self->yaw_speed;
        else if (move < -self->yaw_speed)
            move = -self->yaw_speed;

        self->s.angles[1] = current + move;
    }
}

/*
 * AI_MoveToward - Walk toward a target position
 */
static void AI_MoveToward(edict_t *self, vec3_t target, float speed)
{
    vec3_t dir;
    float dist;

    VectorSubtract(target, self->s.origin, dir);
    dir[2] = 0;    /* don't change vertical velocity for walk */
    dist = VectorLength(dir);

    if (dist < 1.0f)
        return;

    VectorScale(dir, 1.0f / dist, dir);
    self->velocity[0] = dir[0] * speed;
    self->velocity[1] = dir[1] * speed;
}

/*
 * AI_FindTarget - Look for a player to target
 */
static qboolean AI_FindTarget(edict_t *self)
{
    edict_t *player;
    float dist;

    /* Only target player entity (edict[1]) */
    player = &self->owner->chain[1];  /* can't access globals from here easily */

    /* Get player through game export edicts array */
    {
        extern game_export_t globals;
        player = &globals.edicts[1];
    }

    if (!player || !player->inuse || !player->client)
        return qfalse;

    if (player->health <= 0)
        return qfalse;

    /* Check range */
    dist = AI_Range(self, player);
    if (dist > AI_SIGHT_RANGE)
        return qfalse;

    /* Check line of sight */
    if (!AI_Visible(self, player))
        return qfalse;

    self->enemy = player;
    return qtrue;
}

/* ==========================================================================
   AI State Handlers
   ========================================================================== */

/* Helper: find entity by targetname (same as g_spawn.c version) */
static edict_t *AI_FindByTargetname(const char *targetname)
{
    extern game_export_t globals;
    int i;
    if (!targetname || !targetname[0]) return NULL;
    for (i = 0; i < globals.max_edicts; i++) {
        edict_t *e = &globals.edicts[i];
        if (e->inuse && e->targetname && Q_stricmp(e->targetname, targetname) == 0)
            return e;
    }
    return NULL;
}

static void ai_think_idle(edict_t *self)
{
    /* Look for player */
    if (AI_FindTarget(self)) {
        /* Sight sound */
        if (snd_monster_sight)
            gi.sound(self, CHAN_VOICE, snd_monster_sight, 1.0f, ATTN_NORM, 0);
        /* Transition to alert */
        self->count = AI_STATE_ALERT;
        self->nextthink = level.time + 0.5f;   /* brief alert pause */
        return;
    }

    /* Patrol: if monster has a target, follow path_corner chain */
    if (self->target && !self->enemy) {
        edict_t *goal = self->chain;  /* current patrol waypoint */

        if (!goal) {
            /* Find initial path_corner */
            goal = AI_FindByTargetname(self->target);
            self->chain = goal;
        }

        if (goal) {
            vec3_t diff;
            VectorSubtract(goal->s.origin, self->s.origin, diff);
            diff[2] = 0;

            if (VectorLength(diff) < 32.0f) {
                /* Reached waypoint, move to next */
                if (goal->target) {
                    edict_t *next = AI_FindByTargetname(goal->target);
                    if (next) {
                        self->chain = next;
                        goal = next;
                    } else {
                        /* No next corner — loop back to first */
                        self->chain = AI_FindByTargetname(self->target);
                    }
                } else {
                    /* Last corner — loop back */
                    self->chain = AI_FindByTargetname(self->target);
                }
            }

            /* Walk toward current waypoint */
            if (goal) {
                float patrol_speed = self->speed * 0.4f;  /* slower patrol */
                AI_MoveToward(self, goal->s.origin, patrol_speed);

                /* Face movement direction */
                {
                    float yaw = (float)(atan2(diff[1], diff[0]) * 180.0 / 3.14159265);
                    self->s.angles[1] = yaw;
                }
            }
        }
    }

    self->nextthink = level.time + 0.5f;
}

static void ai_think_alert(edict_t *self)
{
    if (!self->enemy || !self->enemy->inuse || self->enemy->health <= 0) {
        self->enemy = NULL;
        self->count = AI_STATE_IDLE;
        self->nextthink = level.time + 0.5f;
        return;
    }

    AI_FaceEnemy(self);

    /* After a brief alert, start chasing */
    self->count = AI_STATE_CHASE;
    self->nextthink = level.time + FRAMETIME;
}

static void ai_think_chase(edict_t *self)
{
    float dist;

    if (!self->enemy || !self->enemy->inuse || self->enemy->health <= 0) {
        self->enemy = NULL;
        self->count = AI_STATE_IDLE;
        self->velocity[0] = self->velocity[1] = 0;
        self->nextthink = level.time + 0.5f;
        return;
    }

    /* Check if lost sight */
    if (!AI_Visible(self, self->enemy)) {
        /* Continue toward last known position for a bit */
        self->ai_flags |= AI_LOST_SIGHT;
    } else {
        self->ai_flags &= ~AI_LOST_SIGHT;
        VectorCopy(self->enemy->s.origin, self->move_origin);
    }

    AI_FaceEnemy(self);
    dist = AI_Range(self, self->enemy);

    /* If in attack range and have line of sight, attack */
    if (dist < AI_ATTACK_RANGE && AI_Visible(self, self->enemy)) {
        self->count = AI_STATE_ATTACK;
        self->velocity[0] = self->velocity[1] = 0;
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Chase toward enemy or last known position */
    AI_MoveToward(self, self->move_origin, AI_CHASE_SPEED);

    /* If lost sight and reached last known position, go idle */
    if (self->ai_flags & AI_LOST_SIGHT) {
        vec3_t diff;
        VectorSubtract(self->move_origin, self->s.origin, diff);
        if (VectorLength(diff) < 32.0f) {
            self->enemy = NULL;
            self->count = AI_STATE_IDLE;
            self->velocity[0] = self->velocity[1] = 0;
        }
    }

    self->nextthink = level.time + FRAMETIME;
}

static void ai_think_attack(edict_t *self)
{
    float dist;

    if (!self->enemy || !self->enemy->inuse || self->enemy->health <= 0) {
        self->enemy = NULL;
        self->count = AI_STATE_IDLE;
        self->nextthink = level.time + 0.5f;
        return;
    }

    AI_FaceEnemy(self);
    dist = AI_Range(self, self->enemy);

    /* If target moved out of range, chase */
    if (dist > AI_ATTACK_RANGE * 1.2f || !AI_Visible(self, self->enemy)) {
        self->count = AI_STATE_CHASE;
        VectorCopy(self->enemy->s.origin, self->move_origin);
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Melee attack if within close range */
    if (dist < AI_MELEE_RANGE && self->dmg_debounce_time <= level.time) {
        int melee_dmg = self->dmg ? (int)(self->dmg * 1.5f) : 15;
        vec3_t lunge_dir;

        /* Lunge toward target */
        VectorSubtract(self->enemy->s.origin, self->s.origin, lunge_dir);
        VectorNormalize(lunge_dir);
        self->velocity[0] = lunge_dir[0] * 150.0f;
        self->velocity[1] = lunge_dir[1] * 150.0f;

        if (self->enemy->takedamage && self->enemy->health > 0) {
            /* Spawn invuln check */
            if (self->enemy->client && self->enemy->client->invuln_time > level.time) {
                /* Blocked */
            } else {
                self->enemy->health -= melee_dmg;
                R_ParticleEffect(self->enemy->s.origin, lunge_dir, 1, 6);

                if (self->enemy->client) {
                    AI_DamageDirectionToPlayer(self->enemy, self->s.origin);
                    self->enemy->client->pers_health = self->enemy->health;
                    self->enemy->client->blend[0] = 1.0f;
                    self->enemy->client->blend[1] = 0.0f;
                    self->enemy->client->blend[2] = 0.0f;
                    self->enemy->client->blend[3] = 0.4f;
                }

                if (self->enemy->health <= 0 && !self->enemy->deadflag) {
                    self->enemy->deadflag = 1;
                    if (self->enemy->client)
                        self->enemy->client->ps.pm_type = PM_DEAD;
                }
            }
        }

        self->dmg_debounce_time = level.time + 0.8f;  /* melee cooldown */
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Fire hitscan attack at the player */
    if (self->dmg_debounce_time <= level.time) {
        vec3_t start, end, dir;
        trace_t tr;
        int damage = self->dmg ? self->dmg : 10;

        VectorCopy(self->s.origin, start);
        start[2] += 20;    /* eye height */

        VectorSubtract(self->enemy->s.origin, start, dir);
        /* Add some spread/inaccuracy */
        dir[0] += ((float)(rand() % 100) - 50) * 0.01f;
        dir[1] += ((float)(rand() % 100) - 50) * 0.01f;
        VectorNormalize(dir);
        VectorMA(start, 8192, dir, end);

        tr = gi.trace(start, NULL, NULL, end, self, MASK_SHOT);

        if (tr.fraction < 1.0f) {
            /* Bullet tracer */
            {
                vec3_t tracer_start;
                VectorMA(start, 16, dir, tracer_start);
                R_AddTracer(tracer_start, tr.endpos, 1.0f, 0.7f, 0.3f);
            }

            /* Muzzle flash particles, light, and sound */
            {
                vec3_t muzzle;
                VectorMA(start, 16, dir, muzzle);
                R_ParticleEffect(muzzle, dir, 3, 2);
                R_AddDlight(muzzle, 1.0f, 0.7f, 0.3f, 150.0f, 0.1f);
                if (snd_monster_fire)
                    gi.sound(self, CHAN_WEAPON, snd_monster_fire, 1.0f, ATTN_NORM, 0);
            }

            if (tr.ent && tr.ent->takedamage && tr.ent->health > 0) {
                /* Spawn invuln check */
                if (tr.ent->client && tr.ent->client->invuln_time > level.time) {
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 3, 4);
                } else {
                    tr.ent->health -= damage;
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 1, 4);

                    /* Apply damage blend and sync pers_health */
                    if (tr.ent->client) {
                        tr.ent->client->pers_health = tr.ent->health;
                        tr.ent->client->blend[0] = 1.0f;
                        tr.ent->client->blend[1] = 0.0f;
                        tr.ent->client->blend[2] = 0.0f;
                        tr.ent->client->blend[3] = 0.3f;
                        AI_DamageDirectionToPlayer(tr.ent, self->s.origin);
                    }

                    if (tr.ent->health <= 0 && tr.ent->deadflag == 0) {
                        tr.ent->deadflag = 1;
                        if (tr.ent->client)
                            tr.ent->client->ps.pm_type = PM_DEAD;
                    }
                }
            } else {
                /* Impact sparks on wall */
                R_ParticleEffect(tr.endpos, tr.plane.normal, 0, 4);
            }
        }

        self->dmg_debounce_time = level.time + AI_ATTACK_INTERVAL;
    }

    self->nextthink = level.time + FRAMETIME;
}

static void ai_think_pain(edict_t *self)
{
    /* Pain stun is over, return to chase */
    if (self->enemy && self->enemy->inuse && self->enemy->health > 0) {
        self->count = AI_STATE_CHASE;
    } else {
        self->count = AI_STATE_IDLE;
    }
    self->nextthink = level.time + FRAMETIME;
}

/* ==========================================================================
   Monster Think Dispatcher
   ========================================================================== */

void monster_think(edict_t *self)
{
    if (!self->inuse || self->health <= 0)
        return;

    switch (self->count) {  /* count field used as AI state */
    case AI_STATE_IDLE:     ai_think_idle(self); break;
    case AI_STATE_ALERT:    ai_think_alert(self); break;
    case AI_STATE_CHASE:    ai_think_chase(self); break;
    case AI_STATE_ATTACK:   ai_think_attack(self); break;
    case AI_STATE_PAIN:     ai_think_pain(self); break;
    default:                self->nextthink = level.time + 1.0f; break;
    }
}

/* ==========================================================================
   Monster Pain / Death Callbacks
   ========================================================================== */

void monster_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    (void)kick;

    if (self->health <= 0)
        return;

    /* Pain sound */
    {
        int snd = (rand() & 1) ? snd_monster_pain1 : snd_monster_pain2;
        if (snd)
            gi.sound(self, CHAN_VOICE, snd, 1.0f, ATTN_NORM, 0);
    }

    /* Brief pain stun — longer for heavy hits */
    self->count = AI_STATE_PAIN;
    self->velocity[0] = self->velocity[1] = 0;
    self->nextthink = level.time + (damage > 30 ? AI_PAIN_TIME * 2.0f : AI_PAIN_TIME);

    /* Knockback on heavy damage */
    if (damage > 30 && other) {
        vec3_t knockback;
        VectorSubtract(self->s.origin, other->s.origin, knockback);
        knockback[2] = 0;
        VectorNormalize(knockback);
        self->velocity[0] = knockback[0] * (float)damage * 3.0f;
        self->velocity[1] = knockback[1] * (float)damage * 3.0f;
        self->velocity[2] = 100.0f;  /* slight upward pop */
    }

    /* Spawn blood at impact */
    {
        vec3_t up = {0, 0, 1};
        R_ParticleEffect(self->s.origin, up, 1, 6 + damage / 5);
    }

    /* If no enemy yet, target attacker */
    if (!self->enemy && other && other->client) {
        self->enemy = other;
    }
}

/* Corpse removal think — called after corpse delay expires */
static void monster_corpse_remove(edict_t *self)
{
    self->inuse = qfalse;
    gi.unlinkentity(self);
}

void monster_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                 int damage, vec3_t point)
{
    vec3_t up = {0, 0, 1};

    (void)inflictor; (void)attacker; (void)damage; (void)point;

    /* Death sound */
    if (snd_monster_die)
        gi.sound(self, CHAN_VOICE, snd_monster_die, 1.0f, ATTN_NORM, 0);

    /* Death explosion of blood */
    R_ParticleEffect(self->s.origin, up, 1, 24);

    /* Stop combat, leave corpse visible for a few seconds */
    self->takedamage = DAMAGE_NO;
    self->solid = SOLID_NOT;
    self->movetype = MOVETYPE_NONE;
    self->velocity[0] = self->velocity[1] = self->velocity[2] = 0;
    self->deadflag = 1;
    self->count = AI_STATE_DEAD;
    self->svflags |= SVF_DEADMONSTER;

    /* Schedule corpse removal after 10 seconds */
    self->think = monster_corpse_remove;
    self->nextthink = level.time + 10.0f;

    gi.linkentity(self);

    gi.dprintf("Monster killed: %s\n", self->classname ? self->classname : "unknown");
}

/* ==========================================================================
   Monster Spawn Functions
   ========================================================================== */

/*
 * monster_start - Common initialization for all monster types
 */
static void monster_start(edict_t *ent, int health, int damage,
                           float speed)
{
    /* Precache monster sounds once */
    if (!monster_sounds_cached) {
        snd_monster_pain1 = gi.soundindex("npc/pain1.wav");
        snd_monster_pain2 = gi.soundindex("npc/pain2.wav");
        snd_monster_die = gi.soundindex("npc/die1.wav");
        snd_monster_sight = gi.soundindex("npc/sight1.wav");
        snd_monster_fire = gi.soundindex("weapons/mp5/fire.wav");
        monster_sounds_cached = qtrue;
    }

    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_STEP;
    ent->takedamage = DAMAGE_AIM;
    ent->health = health;
    ent->max_health = health;
    ent->dmg = damage;
    ent->speed = speed;
    ent->yaw_speed = 20.0f;
    ent->mass = 200;
    ent->gravity = 1.0f;

    /* Set bounding box (humanoid) */
    VectorSet(ent->mins, -16, -16, -24);
    VectorSet(ent->maxs, 16, 16, 32);

    ent->entity_type = ET_GENERAL;
    ent->clipmask = MASK_MONSTERSOLID;

    /* AI callbacks */
    ent->think = monster_think;
    ent->pain = monster_pain;
    ent->die = monster_die;
    ent->count = AI_STATE_IDLE;     /* initial AI state */
    ent->nextthink = level.time + 0.5f + ((float)(rand() % 100)) * 0.01f;

    gi.linkentity(ent);
}

/*
 * SP_monster_soldier - Basic humanoid soldier
 * The most common enemy type in SoF.
 */
void SP_monster_soldier(edict_t *ent, void *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    gi.dprintf("  Spawning soldier at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);

    ent->classname = "monster_soldier";
    monster_start(ent, 100, 10, AI_CHASE_SPEED);
}

/*
 * SP_monster_soldier_light - Light soldier variant (weaker)
 */
void SP_monster_soldier_light(edict_t *ent, void *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    gi.dprintf("  Spawning soldier_light at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);

    ent->classname = "monster_soldier_light";
    monster_start(ent, 60, 8, AI_CHASE_SPEED * 0.8f);
}

/*
 * SP_monster_soldier_ss - SS soldier variant (tougher)
 */
void SP_monster_soldier_ss(edict_t *ent, void *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    gi.dprintf("  Spawning soldier_ss at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);

    ent->classname = "monster_soldier_ss";
    monster_start(ent, 150, 15, AI_CHASE_SPEED * 1.2f);
}

/*
 * SP_monster_guard - Static guard, stands ground
 */
void SP_monster_guard(edict_t *ent, void *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    gi.dprintf("  Spawning guard at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);

    ent->classname = "monster_guard";
    ent->ai_flags |= AI_STAND_GROUND;
    monster_start(ent, 200, 20, AI_CHASE_SPEED * 0.6f);
}

/*
 * SP_monster_boss - Boss enemy (high HP, high damage)
 */
void SP_monster_boss(edict_t *ent, void *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    gi.dprintf("  Spawning boss at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);

    ent->classname = "monster_boss";
    monster_start(ent, 500, 30, AI_CHASE_SPEED * 0.5f);
    ent->yaw_speed = 15.0f;
}
