/*
 * g_phys.c - Entity physics simulation
 *
 * Handles entity movement based on movetype: gravity, friction,
 * collision response, and push mechanics for doors/platforms.
 *
 * Called from RunFrame for each active entity with a movetype.
 *
 * Original gamex86.dll: SV_Physics_* at 0x5003xxxx
 * Based on Q2 g_phys.c (id Software GPL)
 */

#include "g_local.h"

#include <math.h>

/* Extern game import */
extern game_import_t gi;

/* Forward declarations */
static void SV_CheckVelocity(edict_t *ent);
static void SV_Impact(edict_t *e1, trace_t *trace);

/* ==========================================================================
   Utility
   ========================================================================== */

#define SV_GRAVITY      800.0f
#define FRAMETIME       0.1f        /* 10 Hz */

static void SV_CheckVelocity(edict_t *ent)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (ent->velocity[i] > 2000)
            ent->velocity[i] = 2000;
        if (ent->velocity[i] < -2000)
            ent->velocity[i] = -2000;
    }
}

static void SV_Impact(edict_t *e1, trace_t *trace)
{
    edict_t *e2 = trace->ent;

    if (e1->touch && e2) {
        e1->touch(e1, e2, &trace->plane, trace->surface);
    }

    if (e2 && e2->touch) {
        e2->touch(e2, e1, NULL, NULL);
    }
}

/*
 * SV_FlyMove - Move an entity and handle collisions
 *
 * Attempts to move entity along its velocity vector, clipping
 * against world and other entities. Returns the trace result.
 */
static trace_t SV_PushEntity(edict_t *ent, vec3_t push)
{
    trace_t trace;
    vec3_t start, end;

    VectorCopy(ent->s.origin, start);
    VectorAdd(start, push, end);

    trace = gi.trace(start, ent->mins, ent->maxs, end, ent,
                     ent->clipmask ? ent->clipmask : MASK_SOLID);

    VectorCopy(trace.endpos, ent->s.origin);
    gi.linkentity(ent);

    if (trace.fraction != 1.0f) {
        SV_Impact(ent, &trace);

        /* If entity was removed by touch, don't continue */
        if (!ent->inuse)
            return trace;
    }

    return trace;
}

/* ==========================================================================
   Physics Implementations
   ========================================================================== */

/*
 * SV_Physics_None - No movement at all
 */
static void SV_Physics_None(edict_t *ent)
{
    (void)ent;
}

/*
 * SV_Physics_Noclip - Move without collision (spectators, debug)
 */
static void SV_Physics_Noclip(edict_t *ent)
{
    ent->s.angles[0] += FRAMETIME * ent->avelocity[0];
    ent->s.angles[1] += FRAMETIME * ent->avelocity[1];
    ent->s.angles[2] += FRAMETIME * ent->avelocity[2];

    ent->s.origin[0] += FRAMETIME * ent->velocity[0];
    ent->s.origin[1] += FRAMETIME * ent->velocity[1];
    ent->s.origin[2] += FRAMETIME * ent->velocity[2];

    gi.linkentity(ent);
}

/*
 * SV_Physics_Toss - Gravity + bounce (grenades, gibs, debris)
 */
static void SV_Physics_Toss(edict_t *ent)
{
    trace_t trace;
    vec3_t move;
    float backoff;
    int i;

    SV_CheckVelocity(ent);

    /* Apply gravity */
    if (ent->movetype != MOVETYPE_FLY &&
        ent->movetype != MOVETYPE_FLYMISSILE) {
        float grav = ent->gravity ? ent->gravity : 1.0f;
        ent->velocity[2] -= grav * SV_GRAVITY * FRAMETIME;
    }

    /* Apply angular velocity */
    ent->s.angles[0] += FRAMETIME * ent->avelocity[0];
    ent->s.angles[1] += FRAMETIME * ent->avelocity[1];
    ent->s.angles[2] += FRAMETIME * ent->avelocity[2];

    /* Move */
    move[0] = ent->velocity[0] * FRAMETIME;
    move[1] = ent->velocity[1] * FRAMETIME;
    move[2] = ent->velocity[2] * FRAMETIME;

    trace = SV_PushEntity(ent, move);
    if (!ent->inuse)
        return;

    if (trace.fraction < 1.0f) {
        if (ent->movetype == MOVETYPE_BOUNCE) {
            backoff = 1.5f;
        } else {
            backoff = 1.0f;
        }

        /* Clip velocity against surface */
        for (i = 0; i < 3; i++) {
            ent->velocity[i] = ent->velocity[i] -
                trace.plane.normal[i] * DotProduct(ent->velocity, trace.plane.normal) * backoff;
        }

        /* Stop if on ground and moving slowly */
        if (trace.plane.normal[2] > 0.7f) {
            if (ent->velocity[2] < 60 || ent->movetype != MOVETYPE_BOUNCE) {
                ent->velocity[0] = 0;
                ent->velocity[1] = 0;
                ent->velocity[2] = 0;
                ent->avelocity[0] = 0;
                ent->avelocity[1] = 0;
                ent->avelocity[2] = 0;
            }
        }

        /* Stop if the entity should stop on contact */
        if (ent->movetype == MOVETYPE_STOP) {
            VectorClear(ent->velocity);
            VectorClear(ent->avelocity);
        }
    }
}

/*
 * SV_Physics_Push - Door/platform movement
 * Moves entity and pushes other entities out of the way.
 * Based on Q2 SV_Push.
 */
static void SV_Physics_Push(edict_t *ent)
{
    vec3_t move, amove;
    vec3_t org_saved;
    edict_t *touch[32];
    int num_touch, i;

    /* Calculate movement this frame */
    VectorScale(ent->velocity, FRAMETIME, move);
    VectorScale(ent->avelocity, FRAMETIME, amove);

    /* If not moving, skip */
    if (!move[0] && !move[1] && !move[2] &&
        !amove[0] && !amove[1] && !amove[2])
        return;

    /* Save position in case we need to revert */
    VectorCopy(ent->s.origin, org_saved);

    /* Apply movement */
    VectorAdd(ent->s.origin, move, ent->s.origin);
    VectorAdd(ent->s.angles, amove, ent->s.angles);
    gi.linkentity(ent);

    /* Find entities overlapping the pusher's new position */
    num_touch = gi.BoxEdicts(ent->absmin, ent->absmax, touch, 32, AREA_SOLID);

    for (i = 0; i < num_touch; i++) {
        edict_t *other = touch[i];

        if (!other || other == ent || !other->inuse)
            continue;
        if (other->movetype == MOVETYPE_PUSH || other->movetype == MOVETYPE_STOP ||
            other->movetype == MOVETYPE_NONE || other->movetype == MOVETYPE_NOCLIP)
            continue;

        /* Push the entity out of the way */
        {
            trace_t trace;
            vec3_t push_end;

            VectorAdd(other->s.origin, move, push_end);
            trace = gi.trace(other->s.origin, other->mins, other->maxs,
                             push_end, other, other->clipmask ? other->clipmask : MASK_SOLID);

            VectorCopy(trace.endpos, other->s.origin);
            gi.linkentity(other);

            /* If still overlapping, call blocked */
            if (trace.fraction < 1.0f || trace.startsolid) {
                if (ent->blocked)
                    ent->blocked(ent, other);
            }
        }
    }
}

/*
 * SV_Physics_Step - Monster/NPC movement with gravity and stair stepping
 */
static void SV_Physics_Step(edict_t *ent)
{
    trace_t trace;
    vec3_t move;

    SV_CheckVelocity(ent);

    /* Apply gravity if not on ground */
    {
        vec3_t point;
        VectorCopy(ent->s.origin, point);
        point[2] -= 0.25f;

        trace = gi.trace(ent->s.origin, ent->mins, ent->maxs, point, ent, MASK_SOLID);

        if (trace.fraction == 1.0f) {
            /* In air — apply gravity */
            ent->velocity[2] -= SV_GRAVITY * FRAMETIME;
        } else {
            /* On ground — apply friction */
            if (ent->velocity[2] < 0)
                ent->velocity[2] = 0;

            float speed = (float)sqrt(ent->velocity[0] * ent->velocity[0] +
                                       ent->velocity[1] * ent->velocity[1]);
            if (speed > 0) {
                float friction = 6.0f;
                float control = speed < 100 ? 100 : speed;
                float newspeed = speed - FRAMETIME * control * friction;
                if (newspeed < 0) newspeed = 0;
                newspeed /= speed;
                ent->velocity[0] *= newspeed;
                ent->velocity[1] *= newspeed;
            }
        }
    }

    /* Move */
    if (ent->velocity[0] || ent->velocity[1] || ent->velocity[2]) {
        move[0] = ent->velocity[0] * FRAMETIME;
        move[1] = ent->velocity[1] * FRAMETIME;
        move[2] = ent->velocity[2] * FRAMETIME;

        trace = SV_PushEntity(ent, move);
        if (!ent->inuse)
            return;

        /* Clip velocity on collision */
        if (trace.fraction < 1.0f) {
            int i;
            float dot = DotProduct(ent->velocity, trace.plane.normal);
            for (i = 0; i < 3; i++)
                ent->velocity[i] -= trace.plane.normal[i] * dot;
        }
    }

    gi.linkentity(ent);
}

/* ==========================================================================
   Main Physics Dispatch
   Called from RunFrame for each entity.
   ========================================================================== */

void G_RunEntity(edict_t *ent)
{
    if (!ent || !ent->inuse)
        return;

    switch (ent->movetype) {
    case MOVETYPE_NONE:
        SV_Physics_None(ent);
        break;

    case MOVETYPE_NOCLIP:
        SV_Physics_Noclip(ent);
        break;

    case MOVETYPE_PUSH:
    case MOVETYPE_STOP:
        SV_Physics_Push(ent);
        break;

    case MOVETYPE_WALK:
        /* Player movement handled by Pmove, not here */
        break;

    case MOVETYPE_STEP:
        SV_Physics_Step(ent);
        break;

    case MOVETYPE_FLY:
    case MOVETYPE_FLYMISSILE:
    case MOVETYPE_TOSS:
    case MOVETYPE_BOUNCE:
        SV_Physics_Toss(ent);
        break;

    default:
        break;
    }
}
