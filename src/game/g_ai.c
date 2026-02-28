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
extern edict_t *G_AllocEdict(void);
extern void grenade_explode(edict_t *self);
extern edict_t *G_DropItem(vec3_t origin, const char *classname);

/* Forward declarations */
static void AI_FormationSpread(edict_t *self);
static void AI_MedicThink(edict_t *self);
void AI_AllyDied(vec3_t death_origin);

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

    /* View punch — camera flinches from hit direction */
    {
        float punch_pitch = ((float)(rand() % 60) - 30) * 0.1f;  /* random flinch */
        float punch_yaw = ((float)(rand() % 40) - 20) * 0.1f;
        /* Heavier flinch from the hit direction */
        float rad = angle * 3.14159265f / 180.0f;
        punch_yaw += sinf(rad) * 2.0f;
        punch_pitch += -1.5f;  /* always flinch slightly up */

        player->client->kick_angles[0] += punch_pitch;
        player->client->kick_angles[1] += punch_yaw;
    }
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
#define AI_STRAFE_SPEED     120.0f
#define AI_STRAFE_INTERVAL  1.5f   /* change strafe direction */
#define AI_COVER_HEALTH_PCT 0.5f   /* seek cover below 50% health */
#define AI_ALERT_RADIUS     768.0f /* radius to alert nearby monsters */
#define AI_GRENADE_RANGE    400.0f /* min range for grenade throw */
#define AI_GRENADE_COOLDOWN 8.0f   /* seconds between grenade throws */

/* MD2 animation frame ranges (standard Quake 2 humanoid) */
#define FRAME_STAND_START   0
#define FRAME_STAND_END     39
#define FRAME_RUN_START     40
#define FRAME_RUN_END       45
#define FRAME_ATTACK_START  46
#define FRAME_ATTACK_END    53
#define FRAME_PAIN1_START   54
#define FRAME_PAIN1_END     57
#define FRAME_PAIN2_START   58
#define FRAME_PAIN2_END     61
#define FRAME_DEATH1_START  178
#define FRAME_DEATH1_END    183

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

    /* Stealth system: crouching + slow movement reduces detection range */
    {
        float sight_range = AI_SIGHT_RANGE;
        float speed;
        vec3_t hvel;

        VectorCopy(player->velocity, hvel);
        hvel[2] = 0;
        speed = VectorLength(hvel);

        /* Crouching halves detection range */
        if (player->client->ps.pm_flags & PMF_DUCKED)
            sight_range *= 0.5f;

        /* Moving slowly (<80 units/s) further reduces range */
        if (speed < 80.0f)
            sight_range *= 0.6f;
        else if (speed < 150.0f)
            sight_range *= 0.8f;

        /* Sprinting increases detection range */
        if (player->client->sprinting)
            sight_range *= 1.3f;

        dist = AI_Range(self, player);
        if (dist > sight_range)
            return qfalse;
    }

    /* Check line of sight */
    if (!AI_Visible(self, player))
        return qfalse;

    self->enemy = player;
    return qtrue;
}

/* Forward declaration — defined after state handlers */
void monster_think(edict_t *self);

/*
 * AI_ThrowGrenade - Monster throws a grenade toward a target position
 * Uses a parabolic arc calculation to lob the grenade.
 */
static void AI_ThrowGrenade(edict_t *self, vec3_t target_pos)
{
    edict_t *gren;
    vec3_t start, dir;
    float dist, flight_time;

    gren = G_AllocEdict();
    if (!gren) return;

    /* Throw from chest height */
    VectorCopy(self->s.origin, start);
    start[2] += 20;

    VectorSubtract(target_pos, start, dir);
    dist = VectorLength(dir);
    VectorNormalize(dir);

    /* Calculate arc: flight_time based on distance */
    flight_time = dist / 400.0f;
    if (flight_time < 0.5f) flight_time = 0.5f;
    if (flight_time > 2.0f) flight_time = 2.0f;

    gren->classname = "ai_grenade";
    gren->owner = self;
    gren->solid = SOLID_BBOX;
    gren->movetype = MOVETYPE_BOUNCE;
    gren->clipmask = MASK_SHOT;
    VectorSet(gren->mins, -2, -2, -2);
    VectorSet(gren->maxs, 2, 2, 2);
    VectorCopy(start, gren->s.origin);

    /* Horizontal velocity toward target */
    gren->velocity[0] = dir[0] * 400.0f;
    gren->velocity[1] = dir[1] * 400.0f;
    /* Vertical: compensate for gravity during flight */
    gren->velocity[2] = (target_pos[2] - start[2]) / flight_time + 400.0f * flight_time;

    gren->dmg = 40;
    gren->dmg_radius = 180;
    gren->think = grenade_explode;
    gren->nextthink = level.time + flight_time + 0.5f;  /* fuse */

    gi.linkentity(gren);

    /* Throw sound */
    gi.sound(self, CHAN_WEAPON, gi.soundindex("weapons/grenade/throw.wav"), 1.0f, ATTN_NORM, 0);
}

/*
 * AI_AlertNearby - When a monster spots the player, alert nearby
 * monsters within AI_ALERT_RADIUS so they also enter combat.
 */
static void AI_AlertNearby(edict_t *self, edict_t *target)
{
    extern game_export_t globals;
    int i;
    for (i = 1; i < globals.num_edicts; i++) {
        edict_t *e = &globals.edicts[i];
        float dist;
        vec3_t diff;

        if (e == self || !e->inuse || e->health <= 0)
            continue;
        if (!e->think || e->think != monster_think)
            continue;
        if (e->enemy)       /* already in combat */
            continue;

        VectorSubtract(e->s.origin, self->s.origin, diff);
        dist = VectorLength(diff);
        if (dist > AI_ALERT_RADIUS)
            continue;

        /* Alert this monster */
        e->enemy = target;
        e->count = AI_STATE_ALERT;
        e->nextthink = level.time + 0.2f + ((float)(rand() % 50)) * 0.01f;
    }
}

/*
 * AI_TryStrafe - Compute lateral strafe velocity perpendicular to enemy
 */
static void AI_Strafe(edict_t *self, int strafe_dir)
{
    vec3_t forward, right;
    float yaw_rad;

    if (!self->enemy) return;

    yaw_rad = self->s.angles[1] * 3.14159265f / 180.0f;
    forward[0] = cosf(yaw_rad);
    forward[1] = sinf(yaw_rad);
    forward[2] = 0;
    right[0] = forward[1];
    right[1] = -forward[0];
    right[2] = 0;

    if (strafe_dir < 0) {
        right[0] = -right[0];
        right[1] = -right[1];
    }

    /* Check if strafe direction is walkable */
    {
        vec3_t dest;
        trace_t tr;
        VectorMA(self->s.origin, 48.0f, right, dest);
        tr = gi.trace(self->s.origin, self->mins, self->maxs, dest,
                      self, MASK_MONSTERSOLID);
        if (tr.fraction < 0.5f)
            return;  /* blocked, don't strafe */
    }

    self->velocity[0] = right[0] * AI_STRAFE_SPEED;
    self->velocity[1] = right[1] * AI_STRAFE_SPEED;
}

/*
 * AI_Dodge — Quick sidestep when player is aiming at this monster.
 * Traces from player's view to see if monster is in crosshair.
 */
static void AI_Dodge(edict_t *self)
{
    edict_t *player;
    vec3_t fwd, aim_end, to_me;
    float dot, dist;

    if (!self->enemy || !self->enemy->client)
        return;

    player = self->enemy;

    /* Check if player is aiming toward this monster */
    {
        float yaw_rad = player->client->viewangles[1] * 3.14159265f / 180.0f;
        float pitch_rad = player->client->viewangles[0] * 3.14159265f / 180.0f;
        fwd[0] = cosf(yaw_rad) * cosf(pitch_rad);
        fwd[1] = sinf(yaw_rad) * cosf(pitch_rad);
        fwd[2] = -sinf(pitch_rad);
    }

    VectorSubtract(self->s.origin, player->s.origin, to_me);
    dist = VectorLength(to_me);
    if (dist < 1.0f) return;

    VectorScale(to_me, 1.0f / dist, to_me);
    dot = DotProduct(fwd, to_me);

    /* Player aiming within ~15 degrees of this monster */
    if (dot > 0.96f && dist < 600.0f) {
        /* Dodge: quick strafe sideways */
        int dir = (rand() & 1) ? 1 : -1;
        AI_Strafe(self, dir);
        /* Extra speed burst for dodge */
        self->velocity[0] *= 1.8f;
        self->velocity[1] *= 1.8f;
    }
}

/*
 * AI_SeekCover - Try to move behind nearby geometry to break LOS
 * Tests 8 compass directions, picks one that blocks LOS to enemy.
 */
static qboolean AI_SeekCover(edict_t *self)
{
    int i;
    vec3_t test_pos, enemy_eye;
    float best_dist = 999999.0f;
    vec3_t best_pos;
    qboolean found = qfalse;

    if (!self->enemy) return qfalse;

    VectorCopy(self->enemy->s.origin, enemy_eye);
    enemy_eye[2] += 20;

    for (i = 0; i < 8; i++) {
        trace_t tr_move, tr_vis;
        float angle = (float)i * 45.0f * 3.14159265f / 180.0f;
        float dx = cosf(angle) * 128.0f;
        float dy = sinf(angle) * 128.0f;
        float dist;
        vec3_t diff;

        VectorCopy(self->s.origin, test_pos);
        test_pos[0] += dx;
        test_pos[1] += dy;

        /* Can we walk there? */
        tr_move = gi.trace(self->s.origin, self->mins, self->maxs,
                           test_pos, self, MASK_MONSTERSOLID);
        if (tr_move.fraction < 0.8f)
            continue;

        /* Would we be hidden from enemy there? */
        tr_vis = gi.trace(tr_move.endpos, NULL, NULL, enemy_eye,
                          self, MASK_OPAQUE);
        if (tr_vis.fraction >= 1.0f)
            continue;  /* still visible, not cover */

        /* Pick the closest cover spot */
        VectorSubtract(tr_move.endpos, self->s.origin, diff);
        dist = VectorLength(diff);
        if (dist < best_dist) {
            best_dist = dist;
            VectorCopy(tr_move.endpos, best_pos);
            found = qtrue;
        }
    }

    if (found) {
        VectorCopy(best_pos, self->move_origin);
        return qtrue;
    }
    return qfalse;
}

/*
 * AI_AvoidGrenade - Check for nearby grenades and dodge away from them.
 * Returns qtrue if the monster is dodging a grenade.
 */
static qboolean AI_AvoidGrenade(edict_t *self)
{
    extern game_export_t globals;
    int i;

    for (i = 1; i < globals.num_edicts; i++) {
        edict_t *e = &globals.edicts[i];
        vec3_t diff, away;
        float dist;

        if (!e->inuse || !e->classname)
            continue;
        if (Q_stricmp(e->classname, "ai_grenade") != 0 &&
            Q_stricmp(e->classname, "grenade") != 0)
            continue;

        VectorSubtract(e->s.origin, self->s.origin, diff);
        dist = VectorLength(diff);
        if (dist > 256.0f || dist < 1.0f)
            continue;

        /* Found a nearby grenade — run away from it */
        VectorSubtract(self->s.origin, e->s.origin, away);
        away[2] = 0;
        VectorNormalize(away);

        self->velocity[0] = away[0] * AI_CHASE_SPEED * 1.5f;
        self->velocity[1] = away[1] * AI_CHASE_SPEED * 1.5f;
        return qtrue;
    }
    return qfalse;
}

/*
 * AI_CoverPeek - When in cover (not visible to enemy), briefly step out
 * to one side to fire, then return to cover. Uses move_angles[2] as timer.
 */
static void AI_CoverPeek(edict_t *self)
{
    vec3_t peek_pos, to_enemy, perp;
    trace_t tr;
    float side;

    if (!self->enemy || AI_Visible(self, self->enemy))
        return;

    /* Pick a side to peek from (alternates) */
    side = (self->ai_flags & 0x0100) ? 1.0f : -1.0f;

    VectorSubtract(self->enemy->s.origin, self->s.origin, to_enemy);
    to_enemy[2] = 0;
    VectorNormalize(to_enemy);

    /* Perpendicular direction */
    perp[0] = -to_enemy[1] * side;
    perp[1] = to_enemy[0] * side;
    perp[2] = 0;

    peek_pos[0] = self->s.origin[0] + perp[0] * 64.0f;
    peek_pos[1] = self->s.origin[1] + perp[1] * 64.0f;
    peek_pos[2] = self->s.origin[2];

    tr = gi.trace(self->s.origin, self->mins, self->maxs,
                   peek_pos, self, MASK_MONSTERSOLID);
    if (tr.fraction > 0.8f) {
        VectorCopy(tr.endpos, self->move_origin);
        self->velocity[0] = perp[0] * AI_CHASE_SPEED;
        self->velocity[1] = perp[1] * AI_CHASE_SPEED;
    }
}

/*
 * AI_TryRetreat - When critically wounded (<20% HP), run directly away
 * from the enemy. Returns qtrue if retreating.
 */
static qboolean AI_TryRetreat(edict_t *self)
{
    float health_pct;
    vec3_t away, move_pos;
    trace_t tr;

    if (!self->enemy)
        return qfalse;

    health_pct = (float)self->health /
                 (float)(self->max_health > 0 ? self->max_health : 100);

    if (health_pct > 0.2f)
        return qfalse;  /* not critically wounded */

    /* Run directly away from enemy */
    VectorSubtract(self->s.origin, self->enemy->s.origin, away);
    away[2] = 0;
    VectorNormalize(away);

    move_pos[0] = self->s.origin[0] + away[0] * 200.0f;
    move_pos[1] = self->s.origin[1] + away[1] * 200.0f;
    move_pos[2] = self->s.origin[2];

    /* Check if we can move there */
    tr = gi.trace(self->s.origin, self->mins, self->maxs,
                   move_pos, self, MASK_MONSTERSOLID);
    if (tr.fraction < 0.3f)
        return qfalse;  /* can't retreat, blocked */

    VectorCopy(tr.endpos, self->move_origin);
    self->velocity[0] = away[0] * AI_CHASE_SPEED * 1.2f;
    self->velocity[1] = away[1] * AI_CHASE_SPEED * 1.2f;

    /* Pain sound — fleeing */
    {
        extern int snd_monster_pain1;
        if (snd_monster_pain1)
            gi.sound(self, CHAN_VOICE, snd_monster_pain1, 1.0f, ATTN_NORM, 0);
    }

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
        /* Alert nearby squad mates */
        AI_AlertNearby(self, self->enemy);
        /* Transition to alert */
        self->count = AI_STATE_ALERT;
        self->nextthink = level.time + 0.5f;   /* brief alert pause */
        return;
    }

    /* Idle animation — cycle stand frames */
    self->s.frame++;
    if (self->s.frame < FRAME_STAND_START || self->s.frame > FRAME_STAND_END)
        self->s.frame = FRAME_STAND_START;

    /* Patrol: if monster has a target, follow path_corner chain */
    if (self->target && !self->enemy) {
        edict_t *goal = self->patrol_target;

        /* Wait at waypoint before continuing */
        if (self->patrol_wait > level.time)
            goto patrol_done;

        if (!goal) {
            /* Find initial path_corner */
            goal = AI_FindByTargetname(self->target);
            self->patrol_target = goal;
        }

        if (goal) {
            vec3_t diff;
            VectorSubtract(goal->s.origin, self->s.origin, diff);
            diff[2] = 0;

            if (VectorLength(diff) < 32.0f) {
                /* Reached waypoint — pause if wait value set */
                if (goal->wait > 0)
                    self->patrol_wait = level.time + goal->wait;

                /* Move to next path_corner */
                if (goal->target) {
                    edict_t *next = AI_FindByTargetname(goal->target);
                    if (next) {
                        self->patrol_target = next;
                        goal = next;
                    } else {
                        self->patrol_target = AI_FindByTargetname(self->target);
                    }
                } else {
                    /* Last corner — loop back to first */
                    self->patrol_target = AI_FindByTargetname(self->target);
                }
            }

            /* Walk toward current waypoint — run animation */
            if (goal) {
                float patrol_speed = self->speed > 0 ? self->speed * 0.4f : AI_CHASE_SPEED * 0.4f;
                AI_MoveToward(self, goal->s.origin, patrol_speed);

                /* Use run frames instead of stand frames while patrolling */
                self->s.frame++;
                if (self->s.frame < FRAME_RUN_START || self->s.frame > FRAME_RUN_END)
                    self->s.frame = FRAME_RUN_START;

                /* Face movement direction */
                {
                    float yaw = (float)(atan2(diff[1], diff[0]) * 180.0 / 3.14159265);
                    self->s.angles[1] = yaw;
                }
            }
        }
    }
patrol_done:

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

    /* Call out enemy position to nearby allies */
    {
        extern game_export_t globals;
        int ci;
        for (ci = 1; ci < globals.num_edicts; ci++) {
            edict_t *ally = &globals.edicts[ci];
            vec3_t adiff;
            float adist;
            if (ally == self || !ally->inuse || ally->health <= 0)
                continue;
            if (!(ally->svflags & SVF_MONSTER))
                continue;
            if (ally->enemy) continue;  /* already engaged */
            VectorSubtract(ally->s.origin, self->s.origin, adiff);
            adist = VectorLength(adiff);
            if (adist < 800.0f) {
                /* Alert nearby idle allies to the enemy position */
                ally->enemy = self->enemy;
                ally->count = AI_STATE_ALERT;
                VectorCopy(self->enemy->s.origin, ally->move_origin);
                ally->nextthink = level.time + 0.2f + ((float)(rand() % 30)) * 0.01f;
            }
        }
        /* Callout sound */
        {
            int snd = gi.soundindex("npc/alert.wav");
            if (snd)
                gi.sound(self, CHAN_VOICE, snd, 1.0f, ATTN_NORM, 0);
        }
    }

    /* After a brief alert, start chasing */
    self->count = AI_STATE_CHASE;
    self->nextthink = level.time + FRAMETIME;
}

static void ai_think_chase(edict_t *self)
{
    float dist;
    float health_pct;

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
    health_pct = (float)self->health / (float)(self->max_health > 0 ? self->max_health : 100);

    /* Guards stand ground — don't chase, just attack from position */
    if (self->ai_flags & AI_STAND_GROUND) {
        if (dist < AI_ATTACK_RANGE * 1.5f && AI_Visible(self, self->enemy)) {
            self->count = AI_STATE_ATTACK;
            self->velocity[0] = self->velocity[1] = 0;
            self->nextthink = level.time + FRAMETIME;
            return;
        }
        /* Guard stays put, just face enemy */
        self->velocity[0] = self->velocity[1] = 0;
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Light soldiers flee when badly hurt (<30% HP) */
    if (self->classname && Q_stricmp(self->classname, "monster_soldier_light") == 0 &&
        health_pct < 0.3f) {
        vec3_t flee_dir;
        VectorSubtract(self->s.origin, self->enemy->s.origin, flee_dir);
        flee_dir[2] = 0;
        VectorNormalize(flee_dir);
        /* Run away faster than normal */
        self->velocity[0] = flee_dir[0] * AI_CHASE_SPEED * 1.5f;
        self->velocity[1] = flee_dir[1] * AI_CHASE_SPEED * 1.5f;
        /* Run animation */
        self->s.frame++;
        if (self->s.frame < FRAME_RUN_START || self->s.frame > FRAME_RUN_END)
            self->s.frame = FRAME_RUN_START;
        /* If far enough away, disengage */
        if (dist > AI_SIGHT_RANGE * 0.8f) {
            self->enemy = NULL;
            self->count = AI_STATE_IDLE;
            self->velocity[0] = self->velocity[1] = 0;
        }
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Generic wounded retreat: any monster below 30% HP tries to fall back */
    if (health_pct < 0.3f &&
        !(self->classname && Q_stricmp(self->classname, "monster_soldier_light") == 0) &&
        !(self->ai_flags & AI_STAND_GROUND)) {
        /* Try to find cover first */
        if (AI_SeekCover(self)) {
            AI_MoveToward(self, self->move_origin, AI_CHASE_SPEED * 1.2f);
        } else {
            /* No cover — just back away from enemy */
            vec3_t retreat_dir;
            VectorSubtract(self->s.origin, self->enemy->s.origin, retreat_dir);
            retreat_dir[2] = 0;
            VectorNormalize(retreat_dir);
            self->velocity[0] = retreat_dir[0] * AI_CHASE_SPEED;
            self->velocity[1] = retreat_dir[1] * AI_CHASE_SPEED;
        }
        /* Still fire back while retreating (suppressive) */
        if (AI_Visible(self, self->enemy) && dist < AI_ATTACK_RANGE * 1.5f) {
            AI_FaceEnemy(self);
            if (level.time > self->dmg_debounce_time) {
                if (snd_monster_fire)
                    gi.sound(self, CHAN_WEAPON, snd_monster_fire, 1.0f, ATTN_NORM, 0);
                self->dmg_debounce_time = level.time + 0.4f;  /* slower fire while retreating */
            }
        }
        /* Signal nearby allies to provide covering fire */
        {
            extern game_export_t globals;
            int ci;
            for (ci = 1; ci < globals.num_edicts; ci++) {
                edict_t *ally = &globals.edicts[ci];
                vec3_t adiff;
                float adist;
                if (ally == self || !ally->inuse || ally->health <= 0)
                    continue;
                if (!(ally->svflags & SVF_MONSTER))
                    continue;
                VectorSubtract(ally->s.origin, self->s.origin, adiff);
                adist = VectorLength(adiff);
                if (adist < 512.0f && ally->enemy && AI_Visible(ally, ally->enemy)) {
                    /* Ally fires a covering burst */
                    if (level.time > ally->move_angles[2] + 0.2f) {
                        vec3_t aim_dir, aim_end;
                        trace_t ctr;
                        VectorSubtract(ally->enemy->s.origin, ally->s.origin, aim_dir);
                        VectorNormalize(aim_dir);
                        VectorMA(ally->s.origin, 1024.0f, aim_dir, aim_end);
                        ctr = gi.trace(ally->s.origin, NULL, NULL, aim_end, ally, MASK_SHOT);
                        R_AddTracer(ally->s.origin, ctr.endpos, 1.0f, 0.7f, 0.3f);
                        if (snd_monster_fire)
                            gi.sound(ally, CHAN_WEAPON, snd_monster_fire, 0.8f, ATTN_NORM, 0);
                        ally->move_angles[2] = level.time;
                    }
                }
            }
        }
        self->s.frame++;
        if (self->s.frame < FRAME_RUN_START || self->s.frame > FRAME_RUN_END)
            self->s.frame = FRAME_RUN_START;
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* SS soldiers seek cover more aggressively when hurt (<60% HP) */
    if (self->classname && Q_stricmp(self->classname, "monster_soldier_ss") == 0 &&
        health_pct < 0.6f && self->dmg_debounce_time <= level.time) {
        if (AI_SeekCover(self)) {
            AI_MoveToward(self, self->move_origin, AI_CHASE_SPEED * 1.3f);
            /* Run animation */
            self->s.frame++;
            if (self->s.frame < FRAME_RUN_START || self->s.frame > FRAME_RUN_END)
                self->s.frame = FRAME_RUN_START;
            self->dmg_debounce_time = level.time + 2.0f;  /* cover check cooldown */
            self->nextthink = level.time + FRAMETIME;
            return;
        }
    }

    /* If in attack range and have line of sight, attack */
    if (dist < AI_ATTACK_RANGE && AI_Visible(self, self->enemy)) {
        self->count = AI_STATE_ATTACK;
        self->velocity[0] = self->velocity[1] = 0;
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Run animation — cycle run frames */
    self->s.frame++;
    if (self->s.frame < FRAME_RUN_START || self->s.frame > FRAME_RUN_END)
        self->s.frame = FRAME_RUN_START;

    /* Flanking: offset approach angle based on entity index to spread out */
    if (self->max_health >= 80 && dist > 128.0f) {
        extern game_export_t globals;
        vec3_t to_enemy, flank_pos;
        float angle_offset;
        float rad;
        int idx = (int)(self - &globals.edicts[0]);

        VectorSubtract(self->enemy->s.origin, self->s.origin, to_enemy);
        to_enemy[2] = 0;
        VectorNormalize(to_enemy);

        /* Offset angle based on entity index: +/- 30-60 degrees */
        angle_offset = (idx % 2 == 0) ? 0.5f : -0.5f;  /* ~30 degrees */
        if (idx % 4 >= 2) angle_offset *= 2.0f;         /* ~60 degrees */

        rad = angle_offset;
        flank_pos[0] = self->enemy->s.origin[0] +
                       (to_enemy[0] * cosf(rad) - to_enemy[1] * sinf(rad)) * -64.0f;
        flank_pos[1] = self->enemy->s.origin[1] +
                       (to_enemy[0] * sinf(rad) + to_enemy[1] * cosf(rad)) * -64.0f;
        flank_pos[2] = self->s.origin[2];

        AI_MoveToward(self, flank_pos, AI_CHASE_SPEED);

        /* Flanking callout — shout periodically when flanking */
        if (level.time > self->move_angles[0] + 3.0f) {
            int snd = gi.soundindex("npc/flank.wav");
            if (snd)
                gi.sound(self, CHAN_VOICE, snd, 0.8f, ATTN_NORM, 0);
            self->move_angles[0] = level.time;
        }
    } else {
        /* Direct chase toward enemy or last known position */
        AI_MoveToward(self, self->move_origin, AI_CHASE_SPEED);
    }

    /* Push apart from nearby allies to avoid clumping */
    AI_FormationSpread(self);

    /* If lost sight, consider throwing grenade at last known position */
    if (self->ai_flags & AI_LOST_SIGHT) {
        vec3_t diff;
        float last_dist;
        VectorSubtract(self->move_origin, self->s.origin, diff);
        last_dist = VectorLength(diff);

        /* Throw grenade at last known position if we're tough enough */
        if (self->max_health >= 120 && last_dist > 128.0f &&
            level.time > self->move_angles[1]) {
            AI_ThrowGrenade(self, self->move_origin);
            self->move_angles[1] = level.time + AI_GRENADE_COOLDOWN;
        }

        if (last_dist < 32.0f) {
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
    float health_pct;

    if (!self->enemy || !self->enemy->inuse || self->enemy->health <= 0) {
        self->enemy = NULL;
        self->count = AI_STATE_IDLE;
        self->nextthink = level.time + 0.5f;
        return;
    }

    AI_FaceEnemy(self);
    dist = AI_Range(self, self->enemy);
    health_pct = (float)self->health / (float)(self->max_health > 0 ? self->max_health : 100);

    /* Attack animation — cycle attack frames */
    self->s.frame++;
    if (self->s.frame < FRAME_ATTACK_START || self->s.frame > FRAME_ATTACK_END)
        self->s.frame = FRAME_ATTACK_START;

    /* If target moved out of range, chase */
    if (dist > AI_ATTACK_RANGE * 1.2f) {
        self->count = AI_STATE_CHASE;
        VectorCopy(self->enemy->s.origin, self->move_origin);
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Lost LOS: suppressive fire then peek/chase */
    if (!AI_Visible(self, self->enemy)) {
        /* Tough soldiers fire a burst toward last known position */
        if (self->max_health >= 80 && self->move_angles[2] < level.time) {
            vec3_t aim_dir, aim_end, spray;
            trace_t tr;
            VectorSubtract(self->move_origin, self->s.origin, aim_dir);
            VectorNormalize(aim_dir);
            /* Add random spread for suppressive fire */
            spray[0] = aim_dir[0] + ((float)(rand() % 100) - 50.0f) * 0.002f;
            spray[1] = aim_dir[1] + ((float)(rand() % 100) - 50.0f) * 0.002f;
            spray[2] = aim_dir[2];
            VectorMA(self->s.origin, 1024.0f, spray, aim_end);
            tr = gi.trace(self->s.origin, NULL, NULL, aim_end, self, MASK_SHOT);
            R_AddTracer(self->s.origin, tr.endpos, 1.0f, 0.7f, 0.3f);
            self->move_angles[2] = level.time + 0.15f; /* suppress cooldown */
        }
        AI_CoverPeek(self);
        self->count = AI_STATE_CHASE;
        VectorCopy(self->enemy->s.origin, self->move_origin);
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Surrender: critically wounded light soldiers give up */
    if (health_pct < 0.1f && self->max_health < 100) {
        /* Stop fighting, stand still */
        self->velocity[0] = self->velocity[1] = 0;
        self->enemy = NULL;
        self->takedamage = DAMAGE_YES;  /* still killable */
        self->count = AI_STATE_IDLE;
        self->ai_flags &= ~(AI_LOST_SIGHT | AI_PURSUE_TEMP);
        /* "Hands up" — use pain frame as surrender pose */
        self->s.frame = FRAME_PAIN1_START;
        {
            int snd = gi.soundindex("npc/surrender.wav");
            if (snd)
                gi.sound(self, CHAN_VOICE, snd, 1.0f, ATTN_NORM, 0);
        }
        self->nextthink = level.time + 999.0f;  /* stay surrendered */
        return;
    }

    /* Blind fire from cover: fire around corners with very low accuracy */
    if (self->max_health >= 60 && health_pct < 0.5f &&
        self->move_angles[2] < level.time) {
        vec3_t aim_dir, aim_end, offset_pos;
        trace_t tr;
        float side = (rand() % 2 == 0) ? 40.0f : -40.0f;
        vec3_t right_dir;

        VectorSubtract(self->enemy->s.origin, self->s.origin, aim_dir);
        VectorNormalize(aim_dir);
        /* Perpendicular offset to simulate leaning out */
        right_dir[0] = -aim_dir[1];
        right_dir[1] = aim_dir[0];
        right_dir[2] = 0;
        VectorMA(self->s.origin, side, right_dir, offset_pos);
        offset_pos[2] += 20;
        /* Very inaccurate blind fire */
        aim_dir[0] += ((float)(rand() % 100) - 50.0f) * 0.004f;
        aim_dir[1] += ((float)(rand() % 100) - 50.0f) * 0.004f;
        VectorMA(offset_pos, 1024.0f, aim_dir, aim_end);
        tr = gi.trace(offset_pos, NULL, NULL, aim_end, self, MASK_SHOT);
        R_AddTracer(offset_pos, tr.endpos, 1.0f, 0.6f, 0.2f);
        if (snd_monster_fire)
            gi.sound(self, CHAN_WEAPON, snd_monster_fire, 0.8f, ATTN_NORM, 0);
        /* Can hit player with lucky shot */
        if (tr.ent && tr.ent->takedamage && tr.ent->health > 0) {
            int blind_dmg = (self->dmg ? self->dmg : 10) / 3;
            if (blind_dmg < 1) blind_dmg = 1;
            tr.ent->health -= blind_dmg;
            if (tr.ent->client) {
                tr.ent->client->pers_health = tr.ent->health;
                tr.ent->client->blend[0] = 1.0f;
                tr.ent->client->blend[3] = 0.2f;
            }
        }
        self->move_angles[2] = level.time + 0.8f; /* blind fire cooldown */
    }

    /* Full retreat when critically wounded */
    if (AI_TryRetreat(self)) {
        self->count = AI_STATE_CHASE;
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Seek cover when badly hurt */
    if (health_pct < AI_COVER_HEALTH_PCT && self->dmg_debounce_time <= level.time) {
        if (AI_SeekCover(self)) {
            self->count = AI_STATE_CHASE;  /* move to cover position */
            self->nextthink = level.time + FRAMETIME;
            return;
        }
    }

    /* Dodge: react when player is aiming directly (skilled monsters) */
    if (self->max_health >= 80 && gi.irand(0, 10) == 0) {
        AI_Dodge(self);
    }

    /* Squad leader: toughest monster coordinates nearby allies */
    if (self->max_health >= 120 && level.time > self->move_angles[0] + 4.0f) {
        extern game_export_t globals;
        int i;
        qboolean should_advance = (dist > AI_ATTACK_RANGE * 0.7f);

        for (i = 1; i < globals.num_edicts; i++) {
            edict_t *ally = &globals.edicts[i];
            vec3_t d;
            float ally_dist;

            if (ally == self || !ally->inuse || ally->health <= 0)
                continue;
            if (!(ally->svflags & SVF_MONSTER) || !ally->enemy)
                continue;

            VectorSubtract(ally->s.origin, self->s.origin, d);
            ally_dist = VectorLength(d);
            if (ally_dist > 600.0f)
                continue;

            if (should_advance) {
                /* Order advance — allies chase more aggressively */
                ally->count = AI_STATE_CHASE;
                VectorCopy(self->enemy->s.origin, ally->move_origin);
            }
            /* else hold position — allies stay in attack state */
        }

        /* Leader callout sound */
        {
            int snd = gi.soundindex(should_advance ?
                "npc/advance.wav" : "npc/hold.wav");
            if (snd)
                gi.sound(self, CHAN_VOICE, snd, 1.0f, ATTN_NORM, 0);
        }
    }

    /* Combat callout: periodically re-alert allies with updated enemy position */
    if (level.time > self->move_angles[0] + 5.0f) {
        self->move_angles[0] = level.time;
        AI_AlertNearby(self, self->enemy);

        /* AI taunt: periodically taunt the player during combat */
        if (gi.irand(0, 3) == 0 && self->enemy->client) {
            int snd = gi.soundindex("npc/taunt.wav");
            if (snd)
                gi.sound(self, CHAN_VOICE, snd, 0.9f, ATTN_NORM, 0);
        }
    }

    /* Strafe while attacking (change direction periodically) */
    if (dist > AI_MELEE_RANGE) {
        /* Use move_angles[2] as strafe timer */
        if (level.time > self->move_angles[2]) {
            self->move_angles[2] = level.time + AI_STRAFE_INTERVAL;
            /* Flip strafe direction, stored in ai_flags bit 0x0100 */
            self->ai_flags ^= 0x0100;
        }
        AI_Strafe(self, (self->ai_flags & 0x0100) ? 1 : -1);
    }

    /* Grenade throw — when at range and cooldown expired */
    if (dist > AI_GRENADE_RANGE && level.time > self->move_angles[1]) {
        /* Only SS soldiers and bosses throw grenades (tougher variants) */
        if (self->max_health >= 120) {
            AI_ThrowGrenade(self, self->enemy->s.origin);
            self->move_angles[1] = level.time + AI_GRENADE_COOLDOWN;
        }
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
        float inaccuracy = 0.5f;  /* default spread */

        /* Per-type accuracy and fire rate adjustments */
        if (self->classname) {
            if (Q_stricmp(self->classname, "monster_soldier_light") == 0)
                inaccuracy = 0.7f;     /* worse aim */
            else if (Q_stricmp(self->classname, "monster_soldier_ss") == 0)
                inaccuracy = 0.3f;     /* precise */
            else if (Q_stricmp(self->classname, "monster_guard") == 0)
                inaccuracy = 0.25f;    /* very precise, slow rate */
            else if (Q_stricmp(self->classname, "monster_sniper") == 0)
                inaccuracy = 0.1f;     /* extremely precise */
        }

        /* Difficulty scales accuracy: easy=worse, hard/nightmare=better */
        {
            extern cvar_t *skill;
            int sk = skill ? (int)skill->value : 1;
            if (sk <= 0) inaccuracy *= 1.5f;
            else if (sk == 2) inaccuracy *= 0.7f;
            else if (sk >= 3) inaccuracy *= 0.4f;
        }

        /* Panic fire: critically wounded monsters shoot faster but wildly */
        if (health_pct < 0.2f) {
            inaccuracy *= 2.5f;  /* terrible aim */
            self->dmg_debounce_time = level.time + 0.2f; /* rapid fire */
        }

        VectorCopy(self->s.origin, start);
        start[2] += 20;    /* eye height */

        VectorSubtract(self->enemy->s.origin, start, dir);
        /* Add spread/inaccuracy scaled by type */
        dir[0] += ((float)(rand() % 100) - 50) * 0.01f * inaccuracy;
        dir[1] += ((float)(rand() % 100) - 50) * 0.01f * inaccuracy;
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

                        /* Gunshot wounds cause bleeding for 5 seconds */
                        if (tr.ent->health > 0 && damage >= 8) {
                            tr.ent->client->bleed_end = level.time + 5.0f;
                            tr.ent->client->bleed_next_tick = level.time + 1.0f;
                        }
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

        /* Per-type fire rate */
        {
            float fire_rate = AI_ATTACK_INTERVAL;
            extern cvar_t *skill;
            int sk = skill ? (int)skill->value : 1;

            if (self->classname) {
                if (Q_stricmp(self->classname, "monster_guard") == 0)
                    fire_rate = 1.5f;   /* slow deliberate shots */
                else if (Q_stricmp(self->classname, "monster_soldier_ss") == 0)
                    fire_rate = 0.6f;   /* aggressive rate */
                else if (Q_stricmp(self->classname, "monster_soldier_light") == 0)
                    fire_rate = 1.2f;   /* hesitant */
                else if (Q_stricmp(self->classname, "monster_sniper") == 0)
                    fire_rate = 2.5f;   /* slow but deadly */
            }

            /* Difficulty scales fire rate: easy=slower, hard/nightmare=faster */
            if (sk <= 0) fire_rate *= 1.4f;
            else if (sk == 2) fire_rate *= 0.8f;
            else if (sk >= 3) fire_rate *= 0.6f;

            self->dmg_debounce_time = level.time + fire_rate;
        }
    }

    self->nextthink = level.time + FRAMETIME;
}

/*
 * Boss-specific attack think — multi-phase combat:
 * Phase 1 (>50% HP): 3-round burst fire + grenades
 * Phase 2 (<=50% HP): ground stomp AoE + charge attacks
 */
static void boss_think_attack(edict_t *self)
{
    float dist, health_pct;

    if (!self->enemy || !self->enemy->inuse || self->enemy->health <= 0) {
        self->enemy = NULL;
        self->count = AI_STATE_IDLE;
        self->nextthink = level.time + 0.5f;
        return;
    }

    AI_FaceEnemy(self);
    dist = AI_Range(self, self->enemy);
    health_pct = (float)self->health / (float)(self->max_health > 0 ? self->max_health : 500);

    /* Attack animation */
    self->s.frame++;
    if (self->s.frame < FRAME_ATTACK_START || self->s.frame > FRAME_ATTACK_END)
        self->s.frame = FRAME_ATTACK_START;

    /* Chase if target out of range */
    if (dist > AI_ATTACK_RANGE * 1.5f || !AI_Visible(self, self->enemy)) {
        self->count = AI_STATE_CHASE;
        VectorCopy(self->enemy->s.origin, self->move_origin);
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Strafe while attacking */
    if (dist > AI_MELEE_RANGE * 2) {
        if (level.time > self->move_angles[2]) {
            self->move_angles[2] = level.time + 1.0f;
            self->ai_flags ^= 0x0100;
        }
        AI_Strafe(self, (self->ai_flags & 0x0100) ? 1 : -1);
    }

    /* Phase 2: Ground stomp AoE when hurt and close */
    if (health_pct <= 0.5f && dist < 192.0f && level.time > self->move_angles[0]) {
        /* Ground stomp — AoE damage around boss */
        vec3_t up = {0, 0, 1};
        R_ParticleEffect(self->s.origin, up, 13, 30);   /* ground dust */
        R_ParticleEffect(self->s.origin, up, 11, 12);   /* debris */
        R_AddDlight(self->s.origin, 1.0f, 0.4f, 0.1f, 300.0f, 0.3f);

        if (self->enemy->client && dist < 192.0f) {
            int stomp_dmg = 25;
            float knockup;
            if (self->enemy->client->invuln_time <= level.time) {
                self->enemy->health -= stomp_dmg;
                self->enemy->client->pers_health = self->enemy->health;
                self->enemy->client->blend[0] = 1.0f;
                self->enemy->client->blend[1] = 0.2f;
                self->enemy->client->blend[2] = 0.0f;
                self->enemy->client->blend[3] = 0.5f;
                /* Knockback upward */
                knockup = 200.0f * (1.0f - dist / 192.0f);
                self->enemy->velocity[2] += knockup;
            }
        }

        {
            extern void SCR_AddScreenShake(float intensity, float duration);
            SCR_AddScreenShake(0.8f, 0.4f);
        }

        self->move_angles[0] = level.time + 3.0f;  /* stomp cooldown */
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Phase 2: Charge attack when hurt and at mid range */
    if (health_pct <= 0.5f && dist > 128.0f && dist < 400.0f &&
        level.time > self->move_angles[1] + 5.0f) {
        vec3_t charge_dir;
        VectorSubtract(self->enemy->s.origin, self->s.origin, charge_dir);
        charge_dir[2] = 0;
        VectorNormalize(charge_dir);
        self->velocity[0] = charge_dir[0] * 500.0f;
        self->velocity[1] = charge_dir[1] * 500.0f;
        self->move_angles[1] = level.time;  /* charge cooldown */
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Burst fire — 3 rapid shots */
    if (self->dmg_debounce_time <= level.time) {
        int burst;
        for (burst = 0; burst < 3; burst++) {
            vec3_t start, end, dir;
            trace_t tr;

            VectorCopy(self->s.origin, start);
            start[2] += 20;

            VectorSubtract(self->enemy->s.origin, start, dir);
            dir[0] += ((float)(rand() % 80) - 40) * 0.01f;
            dir[1] += ((float)(rand() % 80) - 40) * 0.01f;
            VectorNormalize(dir);
            VectorMA(start, 8192, dir, end);

            tr = gi.trace(start, NULL, NULL, end, self, MASK_SHOT);

            if (tr.fraction < 1.0f) {
                vec3_t tracer_start, muzzle;
                VectorMA(start, 16, dir, tracer_start);
                R_AddTracer(tracer_start, tr.endpos, 1.0f, 0.5f, 0.2f);

                VectorMA(start, 16, dir, muzzle);
                R_ParticleEffect(muzzle, dir, 3, 2);
                R_AddDlight(muzzle, 1.0f, 0.7f, 0.3f, 150.0f, 0.05f);

                if (tr.ent && tr.ent->takedamage && tr.ent->health > 0) {
                    if (!tr.ent->client || tr.ent->client->invuln_time <= level.time) {
                        tr.ent->health -= self->dmg;
                        R_ParticleEffect(tr.endpos, tr.plane.normal, 1, 4);
                        if (tr.ent->client) {
                            tr.ent->client->pers_health = tr.ent->health;
                            tr.ent->client->blend[0] = 1.0f;
                            tr.ent->client->blend[1] = 0.0f;
                            tr.ent->client->blend[2] = 0.0f;
                            tr.ent->client->blend[3] = 0.3f;
                            AI_DamageDirectionToPlayer(tr.ent, self->s.origin);
                        }
                        if (tr.ent->health <= 0 && !tr.ent->deadflag) {
                            tr.ent->deadflag = 1;
                            if (tr.ent->client)
                                tr.ent->client->ps.pm_type = PM_DEAD;
                        }
                    }
                } else {
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 0, 4);
                }
            }
        }

        if (snd_monster_fire)
            gi.sound(self, CHAN_WEAPON, snd_monster_fire, 1.0f, ATTN_NORM, 0);

        self->dmg_debounce_time = level.time + 0.5f;  /* faster than normal */
    }

    /* Throw grenades at range */
    if (dist > AI_GRENADE_RANGE && level.time > self->move_angles[1]) {
        AI_ThrowGrenade(self, self->enemy->s.origin);
        self->move_angles[1] = level.time + AI_GRENADE_COOLDOWN * 0.5f;
    }

    self->nextthink = level.time + FRAMETIME;
}

static void ai_think_pain(edict_t *self)
{
    /* Advance pain animation frame */
    self->s.frame++;
    if (self->s.frame > FRAME_PAIN1_END && self->s.frame < FRAME_PAIN2_START)
        self->s.frame = FRAME_PAIN1_END;  /* hold last pain frame */
    if (self->s.frame > FRAME_PAIN2_END)
        self->s.frame = FRAME_PAIN2_END;

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

    /* Blood trail — wounded monsters drip blood periodically */
    if (self->max_health > 0 && self->health < self->max_health / 2) {
        /* Random chance each think frame */
        if (rand() % 8 == 0) {
            vec3_t blood_pos, blood_down;
            VectorCopy(self->s.origin, blood_pos);
            blood_pos[2] -= 10;
            VectorSet(blood_down, 0, 0, -1);
            R_ParticleEffect(blood_pos, blood_down, 1, 1);  /* blood drip */
        }
    }

    /* Grenade avoidance — always check regardless of state */
    if (AI_AvoidGrenade(self)) {
        self->nextthink = level.time + FRAMETIME;
        return;
    }

    /* Medic behavior: prioritize healing allies over combat */
    if (self->ai_flags & 0x0200) {
        AI_MedicThink(self);
    }

    switch (self->count) {  /* count field used as AI state */
    case AI_STATE_IDLE:     ai_think_idle(self); break;
    case AI_STATE_ALERT:    ai_think_alert(self); break;
    case AI_STATE_CHASE:    ai_think_chase(self); break;
    case AI_STATE_ATTACK:
        if (self->classname && Q_stricmp(self->classname, "monster_boss") == 0)
            boss_think_attack(self);
        else
            ai_think_attack(self);
        break;
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

    /* Pain animation — pick a random pain sequence */
    if (rand() & 1)
        self->s.frame = FRAME_PAIN1_START;
    else
        self->s.frame = FRAME_PAIN2_START;

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

    /* Monster infighting: if hit by another monster, fight back */
    if (other && (other->svflags & SVF_MONSTER) && other != self) {
        if (!self->enemy || self->enemy == other) {
            self->enemy = other;
            self->count = AI_STATE_CHASE;
            self->nextthink = level.time + FRAMETIME;
        } else if (damage > 20) {
            /* Heavy damage from another monster overrides current target */
            self->enemy = other;
            self->count = AI_STATE_CHASE;
            self->nextthink = level.time + FRAMETIME;
        }
    }
    /* If no enemy yet, target attacker */
    else if (!self->enemy && other && other->client) {
        self->enemy = other;
    }

    /* Suppression: rapid hits force monster into extended cover */
    if (self->dmg_debounce_time > level.time - 1.0f) {
        /* Hit again within 1s — suppressed, seek cover longer */
        if (AI_SeekCover(self)) {
            AI_MoveToward(self, self->move_origin, AI_CHASE_SPEED);
            self->count = AI_STATE_PAIN;
            self->nextthink = level.time + 1.5f;  /* stay down longer */
        }
    }
    self->dmg_debounce_time = level.time;
}

/* Corpse sink think — gradually lowers corpse into ground before removal */
static void monster_corpse_sink(edict_t *self)
{
    self->s.origin[2] -= 1.0f;  /* sink 10 units/sec at 10Hz */

    /* Remove after sinking ~20 units */
    if (self->s.origin[2] < self->move_origin[2] - 20.0f) {
        self->inuse = qfalse;
        gi.unlinkentity(self);
        return;
    }

    self->nextthink = level.time + FRAMETIME;
    gi.linkentity(self);
}

/* Corpse removal think — start sinking after delay */
static void monster_corpse_remove(edict_t *self)
{
    /* Record starting Z for sink depth tracking */
    VectorCopy(self->s.origin, self->move_origin);
    self->think = monster_corpse_sink;
    self->nextthink = level.time + FRAMETIME;
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
    self->s.frame = FRAME_DEATH1_START;  /* death animation first frame */
    self->svflags |= SVF_DEADMONSTER;

    /* Schedule corpse removal after 10 seconds */
    self->think = monster_corpse_remove;
    self->nextthink = level.time + 10.0f;

    /* Notify nearby allies — may break morale */
    AI_AllyDied(self->s.origin);

    /* Death drops — monsters drop ammo based on their weapon type */
    if (self->weapon_index > 0 && self->weapon_index < WEAP_COUNT) {
        static const char *ammo_drop_names[] = {
            NULL,               /* WEAP_NONE */
            NULL,               /* WEAP_KNIFE (melee) */
            "ammo_pistol",      /* WEAP_PISTOL1 */
            "ammo_pistol",      /* WEAP_PISTOL2 */
            "ammo_shotgun",     /* WEAP_SHOTGUN */
            "ammo_machinegun",  /* WEAP_MACHINEGUN */
            "ammo_assault",     /* WEAP_ASSAULT */
            "ammo_sniper",      /* WEAP_SNIPER */
            "ammo_slugger",     /* WEAP_SLUGGER */
            "ammo_rockets",     /* WEAP_ROCKET */
            "ammo_fuel",        /* WEAP_FLAMEGUN */
            "ammo_cells",       /* WEAP_MPG */
            "ammo_grenades",    /* WEAP_GRENADE */
        };
        const char *drop = ammo_drop_names[self->weapon_index];
        if (drop) {
            /* 60% chance to drop ammo, 20% chance to drop weapon instead */
            int roll = rand() % 100;
            if (roll < 60) {
                G_DropItem(self->s.origin, drop);
            } else if (roll < 80) {
                /* Drop weapon — map weapon_index to weapon classname */
                static const char *weap_drop_names[] = {
                    NULL, NULL,
                    "weapon_pistol1", "weapon_pistol2",
                    "weapon_shotgun", "weapon_machinegun",
                    "weapon_assault", "weapon_sniper",
                    "weapon_slugger", "weapon_rocket",
                    "weapon_flamegun", "weapon_mpg",
                    "weapon_grenade",
                };
                G_DropItem(self->s.origin, weap_drop_names[self->weapon_index]);
            }
            /* 20% chance: drop nothing */
        }
    }

    gi.linkentity(self);

    gi.dprintf("Monster killed: %s\n", self->classname ? self->classname : "unknown");
}

/* ==========================================================================
   Monster Spawn Functions
   ========================================================================== */

/*
 * monster_start - Common initialization for all monster types
 *
 * Difficulty scaling (skill cvar 0-3):
 *   0 (Easy):   HP x0.75, damage x0.5, slower
 *   1 (Normal): no change
 *   2 (Hard):   HP x1.25, damage x1.5, faster, better aim
 *   3 (Nightmare): HP x1.5, damage x2.0, fast, accurate, aggressive
 */
static void monster_start(edict_t *ent, int health, int damage,
                           float speed)
{
    extern cvar_t *skill;
    int sk = skill ? (int)skill->value : 1;

    /* Precache monster sounds once */
    if (!monster_sounds_cached) {
        snd_monster_pain1 = gi.soundindex("npc/pain1.wav");
        snd_monster_pain2 = gi.soundindex("npc/pain2.wav");
        snd_monster_die = gi.soundindex("npc/die1.wav");
        snd_monster_sight = gi.soundindex("npc/sight1.wav");
        snd_monster_fire = gi.soundindex("weapons/mp5/fire.wav");
        monster_sounds_cached = qtrue;
    }

    /* Scale stats by difficulty */
    if (sk <= 0) {
        health = (int)(health * 0.75f);
        damage = (int)(damage * 0.5f);
        speed *= 0.85f;
    } else if (sk == 2) {
        health = (int)(health * 1.25f);
        damage = (int)(damage * 1.5f);
        speed *= 1.1f;
    } else if (sk >= 3) {
        health = (int)(health * 1.5f);
        damage = (int)(damage * 2.0f);
        speed *= 1.25f;
    }
    if (damage < 1) damage = 1;

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
 * SP_monster_sniper - Long-range sniper enemy
 * Very accurate, high damage, slow fire rate. Stays at distance.
 */
void SP_monster_sniper(edict_t *ent, void *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    gi.dprintf("  Spawning sniper at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);

    ent->classname = "monster_sniper";
    ent->ai_flags |= AI_STAND_GROUND;  /* snipers don't chase */
    monster_start(ent, 80, 35, AI_CHASE_SPEED * 0.4f);
    ent->yaw_speed = 12.0f;
    ent->weapon_index = 7;  /* WEAP_SNIPER for drops */
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

/*
 * SP_monster_medic - Medic that heals wounded allies
 * Runs to injured allies and restores health. Low combat ability.
 */
void SP_monster_medic(edict_t *ent, void *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    gi.dprintf("  Spawning medic at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);

    ent->classname = "monster_medic";
    ent->ai_flags |= 0x0200;  /* AI_MEDIC flag */
    monster_start(ent, 80, 5, AI_CHASE_SPEED * 0.9f);
    ent->yaw_speed = 12.0f;
}

/* AI Medic behavior — called from monster_think for medic-flagged monsters */
static void AI_MedicThink(edict_t *self)
{
    extern game_export_t globals;
    int i;
    edict_t *best_patient = NULL;
    float best_dist = 512.0f;  /* max heal range */

    /* Find nearest wounded ally */
    for (i = 1; i < globals.num_edicts; i++) {
        edict_t *e = &globals.edicts[i];
        vec3_t diff;
        float dist;

        if (e == self || !e->inuse || e->health <= 0)
            continue;
        if (!(e->svflags & SVF_MONSTER))
            continue;
        if (e->health >= e->max_health)
            continue;  /* not wounded */

        VectorSubtract(e->s.origin, self->s.origin, diff);
        dist = VectorLength(diff);
        if (dist < best_dist) {
            best_dist = dist;
            best_patient = e;
        }
    }

    if (best_patient) {
        /* Move toward wounded ally */
        AI_MoveToward(self, best_patient->s.origin, AI_CHASE_SPEED);

        /* Heal if close enough */
        if (best_dist < 64.0f) {
            int heal = 5;
            best_patient->health += heal;
            if (best_patient->health > best_patient->max_health)
                best_patient->health = best_patient->max_health;

            /* Healing particles */
            {
                vec3_t up = {0, 0, 1};
                R_ParticleEffect(best_patient->s.origin, up, 2, 4); /* green */
            }
        }
    }
}

/* ==========================================================================
   AI Formation — spread apart when multiple monsters chase same target
   ========================================================================== */

#define AI_FORMATION_DIST   96.0f   /* min distance between allied monsters */
#define AI_FORMATION_PUSH   0.4f    /* push strength when too close */

static void AI_FormationSpread(edict_t *self)
{
    extern game_export_t globals;
    int i;
    vec3_t push;

    push[0] = push[1] = push[2] = 0;

    for (i = 1; i < globals.num_edicts; i++) {
        edict_t *other = &globals.edicts[i];
        vec3_t diff;
        float dist;

        if (other == self || !other->inuse || other->health <= 0)
            continue;
        if (!(other->svflags & SVF_MONSTER))
            continue;
        /* Only spread from allies chasing same target */
        if (other->enemy != self->enemy)
            continue;

        VectorSubtract(self->s.origin, other->s.origin, diff);
        diff[2] = 0;
        dist = VectorLength(diff);

        if (dist < AI_FORMATION_DIST && dist > 1.0f) {
            float scale = (AI_FORMATION_DIST - dist) / AI_FORMATION_DIST;
            VectorNormalize(diff);
            push[0] += diff[0] * scale * AI_CHASE_SPEED * AI_FORMATION_PUSH;
            push[1] += diff[1] * scale * AI_CHASE_SPEED * AI_FORMATION_PUSH;
        }
    }

    /* Apply push to velocity */
    self->velocity[0] += push[0];
    self->velocity[1] += push[1];
}

/* ==========================================================================
   AI Morale — nearby ally deaths cause remaining monsters to flee
   ========================================================================== */

#define AI_MORALE_RANGE     512.0f  /* range to notice ally deaths */
#define AI_MORALE_FLEE_PCT  0.5f    /* flee if >50% of nearby allies dead */

void AI_AllyDied(vec3_t death_origin)
{
    extern game_export_t globals;
    int i;

    for (i = 1; i < globals.num_edicts; i++) {
        edict_t *e = &globals.edicts[i];
        vec3_t diff;
        float dist;
        int alive_nearby, dead_nearby;
        int j;

        if (!e->inuse || e->health <= 0)
            continue;
        if (!(e->svflags & SVF_MONSTER))
            continue;

        VectorSubtract(e->s.origin, death_origin, diff);
        dist = VectorLength(diff);
        if (dist > AI_MORALE_RANGE)
            continue;

        /* Count alive vs dead monsters nearby */
        alive_nearby = 0;
        dead_nearby = 0;
        for (j = 1; j < globals.num_edicts; j++) {
            edict_t *other = &globals.edicts[j];
            vec3_t d2;
            float d2_len;
            if (other == e || !other->inuse)
                continue;
            if (!(other->svflags & SVF_MONSTER))
                continue;
            VectorSubtract(other->s.origin, e->s.origin, d2);
            d2_len = VectorLength(d2);
            if (d2_len > AI_MORALE_RANGE)
                continue;
            if (other->health > 0)
                alive_nearby++;
            else
                dead_nearby++;
        }

        /* If too many allies dead, break and flee */
        if (dead_nearby > 0 && alive_nearby > 0 &&
            (float)dead_nearby / (float)(alive_nearby + dead_nearby) > AI_MORALE_FLEE_PCT) {
            /* Low-health soldiers flee; tougher ones hold ground */
            if (e->max_health < 120) {
                vec3_t flee_dir;
                VectorSubtract(e->s.origin, death_origin, flee_dir);
                flee_dir[2] = 0;
                VectorNormalize(flee_dir);
                VectorCopy(flee_dir, e->move_origin);
                VectorMA(e->s.origin, 300.0f, flee_dir, e->move_origin);
                e->count = AI_STATE_CHASE; /* will chase toward flee point */
                e->enemy = NULL;           /* disengage */
            }
        }
    }
}

/* ==========================================================================
   AI Hearing — gunshots alert idle monsters within hearing range
   ========================================================================== */

#define AI_HEAR_RANGE_LOUD  1200.0f  /* normal gunfire */
#define AI_HEAR_RANGE_QUIET  400.0f  /* silenced weapon */

void AI_HearGunshot(vec3_t origin, edict_t *shooter, qboolean silenced)
{
    extern game_export_t globals;
    float hear_range = silenced ? AI_HEAR_RANGE_QUIET : AI_HEAR_RANGE_LOUD;
    int i;

    for (i = 1; i < globals.num_edicts; i++) {
        edict_t *e = &globals.edicts[i];
        vec3_t diff;
        float dist;

        if (e == shooter || !e->inuse || e->health <= 0)
            continue;
        if (!(e->svflags & SVF_MONSTER))
            continue;
        if (e->enemy)  /* already in combat */
            continue;

        VectorSubtract(e->s.origin, origin, diff);
        dist = VectorLength(diff);
        if (dist > hear_range)
            continue;

        /* Heard the shot — investigate the source */
        e->enemy = shooter;
        e->count = AI_STATE_ALERT;
        VectorCopy(origin, e->move_origin);  /* remember sound location */
        e->nextthink = level.time + 0.3f + ((float)(rand() % 50)) * 0.01f;
    }
}
