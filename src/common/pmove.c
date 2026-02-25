/*
 * pmove.c - Player movement physics
 *
 * Quake II player movement code adapted for Soldier of Fortune.
 * Handles ground movement, jumping, ducking, swimming, flying,
 * stair stepping, and spectator noclip mode.
 *
 * Called by the game module via gi.Pmove() each server frame.
 * Uses trace/pointcontents callbacks to interact with BSP geometry.
 *
 * Original SoF: Pmove at 0x0D1E0 in SoF.exe
 * Based on Q2 pmove.c (id Software GPL)
 */

#include "q_shared.h"
#include "qcommon.h"

#include <math.h>
#include <string.h>

/* Movement constants (Q2 standard) */
#define PM_MAXSPEED         300.0f
#define PM_DUCKSPEED        100.0f
#define PM_ACCELERATE       10.0f
#define PM_AIRACCELERATE    0.0f
#define PM_WATERACCELERATE  10.0f
#define PM_FRICTION         6.0f
#define PM_WATERFRICTION    1.0f
#define PM_WATERSPEED       400.0f
#define PM_STOPSPEED        100.0f
#define PM_JUMPSPEED        270.0f
#define PM_STEPSIZE         18.0f
#define PM_MAXVELOCITY      2000.0f

#define MIN_STEP_NORMAL     0.7f    /* can't step on steep slopes */

/* Local pmove state for the current call */
static pmove_t  *pm;
static float    pm_frametime;

static vec3_t   pml_origin;
static vec3_t   pml_velocity;
static vec3_t   forward, right, up;

/* ==========================================================================
   Utility
   ========================================================================== */

static float PM_ClampVelocity(void)
{
    float speed = VectorLength(pml_velocity);
    if (speed > PM_MAXVELOCITY) {
        float scale = PM_MAXVELOCITY / speed;
        VectorScale(pml_velocity, scale, pml_velocity);
        return PM_MAXVELOCITY;
    }
    return speed;
}

static void PM_ClipVelocity(vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
    float backoff = DotProduct(in, normal) * overbounce;
    int i;

    for (i = 0; i < 3; i++) {
        out[i] = in[i] - normal[i] * backoff;
        if (out[i] > -0.1f && out[i] < 0.1f)
            out[i] = 0;
    }
}

static void AngleVectors(vec3_t angles, vec3_t fwd, vec3_t rt, vec3_t up_out)
{
    float angle;
    float sr, sp, sy, cr, cp, cy;

    angle = angles[1] * (3.14159265f / 180.0f);
    sy = (float)sin(angle);
    cy = (float)cos(angle);
    angle = angles[0] * (3.14159265f / 180.0f);
    sp = (float)sin(angle);
    cp = (float)cos(angle);
    angle = angles[2] * (3.14159265f / 180.0f);
    sr = (float)sin(angle);
    cr = (float)cos(angle);

    if (fwd) {
        fwd[0] = cp * cy;
        fwd[1] = cp * sy;
        fwd[2] = -sp;
    }
    if (rt) {
        rt[0] = (-1 * sr * sp * cy + -1 * cr * -sy);
        rt[1] = (-1 * sr * sp * sy + -1 * cr * cy);
        rt[2] = -1 * sr * cp;
    }
    if (up_out) {
        up_out[0] = (cr * sp * cy + -sr * -sy);
        up_out[1] = (cr * sp * sy + -sr * cy);
        up_out[2] = cr * cp;
    }
}

/* ==========================================================================
   Ground Detection
   ========================================================================== */

static void PM_CategorizePosition(void)
{
    vec3_t point;
    trace_t trace;

    /* Check water level */
    pm->waterlevel = 0;
    pm->watertype = 0;

    if (pm->pointcontents) {
        int cont;
        point[0] = pml_origin[0];
        point[1] = pml_origin[1];
        point[2] = pml_origin[2] + pm->mins[2] + 1;
        cont = pm->pointcontents(point);
        if (cont & MASK_WATER) {
            pm->watertype = cont;
            pm->waterlevel = 1;
            point[2] = pml_origin[2];
            cont = pm->pointcontents(point);
            if (cont & MASK_WATER) {
                pm->waterlevel = 2;
                point[2] = pml_origin[2] + pm->viewheight;
                cont = pm->pointcontents(point);
                if (cont & MASK_WATER)
                    pm->waterlevel = 3;
            }
        }
    }

    /* Check for ground */
    point[0] = pml_origin[0];
    point[1] = pml_origin[1];
    point[2] = pml_origin[2] - 0.25f;

    if (pm->trace) {
        trace = pm->trace(pml_origin, pm->mins, pm->maxs, point);

        if (trace.fraction == 1.0f || trace.plane.normal[2] < MIN_STEP_NORMAL) {
            pm->groundentity = NULL;
            pm->s.pm_flags &= ~PMF_ON_GROUND;
        } else {
            pm->groundentity = trace.ent;
            pm->s.pm_flags |= PMF_ON_GROUND;

            /* Landing event detection */
            if (!(pm->s.pm_flags & PMF_ON_GROUND)) {
                /* Just landed */
            }
        }
    } else {
        /* No trace function — assume on ground */
        pm->s.pm_flags |= PMF_ON_GROUND;
    }
}

/* ==========================================================================
   Ground Movement
   ========================================================================== */

static void PM_Friction(void)
{
    float speed, newspeed, control, drop;

    speed = VectorLength(pml_velocity);
    if (speed < 1) {
        pml_velocity[0] = 0;
        pml_velocity[1] = 0;
        return;
    }

    drop = 0;

    if (pm->s.pm_flags & PMF_ON_GROUND) {
        control = speed < PM_STOPSPEED ? PM_STOPSPEED : speed;
        drop += control * PM_FRICTION * pm_frametime;
    }

    if (pm->waterlevel)
        drop += speed * PM_WATERFRICTION * pm->waterlevel * pm_frametime;

    newspeed = speed - drop;
    if (newspeed < 0)
        newspeed = 0;
    newspeed /= speed;

    pml_velocity[0] *= newspeed;
    pml_velocity[1] *= newspeed;
    pml_velocity[2] *= newspeed;
}

static void PM_Accelerate(vec3_t wishdir, float wishspeed, float accel)
{
    float addspeed, accelspeed, currentspeed;

    currentspeed = DotProduct(pml_velocity, wishdir);
    addspeed = wishspeed - currentspeed;
    if (addspeed <= 0)
        return;

    accelspeed = accel * pm_frametime * wishspeed;
    if (accelspeed > addspeed)
        accelspeed = addspeed;

    pml_velocity[0] += accelspeed * wishdir[0];
    pml_velocity[1] += accelspeed * wishdir[1];
    pml_velocity[2] += accelspeed * wishdir[2];
}

static void PM_AirAccelerate(vec3_t wishdir, float wishspeed, float accel)
{
    float addspeed, accelspeed, currentspeed, wishspd;

    wishspd = wishspeed;
    if (wishspd > 30)
        wishspd = 30;

    currentspeed = DotProduct(pml_velocity, wishdir);
    addspeed = wishspd - currentspeed;
    if (addspeed <= 0)
        return;

    accelspeed = accel * wishspeed * pm_frametime;
    if (accelspeed > addspeed)
        accelspeed = addspeed;

    pml_velocity[0] += accelspeed * wishdir[0];
    pml_velocity[1] += accelspeed * wishdir[1];
    pml_velocity[2] += accelspeed * wishdir[2];
}

/* ==========================================================================
   Slide Move (the core of player movement)
   ========================================================================== */

#define MAX_CLIP_PLANES 5

static void PM_StepSlideMove(void)
{
    vec3_t start_o, start_v;
    vec3_t end;
    trace_t trace;
    int i;
    float time_left;
    int numplanes;
    vec3_t planes[MAX_CLIP_PLANES];

    VectorCopy(pml_origin, start_o);
    VectorCopy(pml_velocity, start_v);

    numplanes = 0;
    time_left = pm_frametime;

    for (i = 0; i < 4; i++) {  /* max 4 bounces */
        end[0] = pml_origin[0] + time_left * pml_velocity[0];
        end[1] = pml_origin[1] + time_left * pml_velocity[1];
        end[2] = pml_origin[2] + time_left * pml_velocity[2];

        if (pm->trace) {
            trace = pm->trace(pml_origin, pm->mins, pm->maxs, end);
        } else {
            /* No trace — just move directly */
            VectorCopy(end, pml_origin);
            break;
        }

        if (trace.allsolid) {
            /* Stuck — don't move */
            pml_velocity[2] = 0;
            return;
        }

        if (trace.fraction > 0) {
            /* Move to trace endpoint */
            VectorCopy(trace.endpos, pml_origin);
        }

        if (trace.fraction == 1)
            break;  /* Moved the full distance */

        time_left -= time_left * trace.fraction;

        /* Clip velocity against the surface */
        if (numplanes < MAX_CLIP_PLANES) {
            VectorCopy(trace.plane.normal, planes[numplanes]);
            numplanes++;
        }

        PM_ClipVelocity(pml_velocity, trace.plane.normal,
                         pml_velocity, 1.01f);
    }

    /* Try stepping up stairs if we're on ground and got blocked */
    if ((pm->s.pm_flags & PMF_ON_GROUND) && pm->trace) {
        vec3_t step_start, step_end;
        trace_t step_trace;

        /* Only step if we hit a vertical wall */
        if (numplanes > 0 && planes[numplanes - 1][2] < MIN_STEP_NORMAL) {
            /* Try stepping up */
            VectorCopy(start_o, step_start);
            step_start[2] += PM_STEPSIZE;

            step_end[0] = step_start[0] + pm_frametime * start_v[0];
            step_end[1] = step_start[1] + pm_frametime * start_v[1];
            step_end[2] = step_start[2];

            /* Check if we can step up */
            step_trace = pm->trace(start_o, pm->mins, pm->maxs, step_start);
            if (!step_trace.allsolid) {
                /* Move forward at elevated height */
                step_trace = pm->trace(step_start, pm->mins, pm->maxs, step_end);
                if (!step_trace.allsolid && step_trace.fraction > 0) {
                    VectorCopy(step_trace.endpos, step_start);

                    /* Step back down */
                    step_end[0] = step_start[0];
                    step_end[1] = step_start[1];
                    step_end[2] = step_start[2] - PM_STEPSIZE;

                    step_trace = pm->trace(step_start, pm->mins, pm->maxs, step_end);
                    if (!step_trace.allsolid) {
                        VectorCopy(step_trace.endpos, pml_origin);
                        VectorCopy(start_v, pml_velocity);
                    }
                }
            }
        }
    }
}

/* ==========================================================================
   Movement Modes
   ========================================================================== */

static void PM_WalkMove(void)
{
    vec3_t wishvel, wishdir;
    float wishspeed, maxspeed;
    float fmove, smove;

    fmove = (float)pm->cmd.forwardmove;
    smove = (float)pm->cmd.sidemove;

    /* Build wish velocity from input */
    wishvel[0] = forward[0] * fmove + right[0] * smove;
    wishvel[1] = forward[1] * fmove + right[1] * smove;
    wishvel[2] = 0;

    VectorCopy(wishvel, wishdir);
    wishspeed = VectorNormalize(wishdir);

    maxspeed = (pm->s.pm_flags & PMF_DUCKED) ? PM_DUCKSPEED : PM_MAXSPEED;
    if (wishspeed > maxspeed) {
        VectorScale(wishvel, maxspeed / wishspeed, wishvel);
        wishspeed = maxspeed;
    }

    if (pm->s.pm_flags & PMF_ON_GROUND) {
        PM_Accelerate(wishdir, wishspeed, PM_ACCELERATE);

        /* Apply gravity if walking down slope */
        if (pml_velocity[2] > 0)
            pml_velocity[2] = 0;

        if (!pml_velocity[0] && !pml_velocity[1])
            return;
    } else {
        /* Air movement */
        PM_AirAccelerate(wishdir, wishspeed, PM_AIRACCELERATE);
    }

    PM_StepSlideMove();
}

static void PM_WaterMove(void)
{
    vec3_t wishvel, wishdir;
    float wishspeed;
    float fmove, smove;

    fmove = (float)pm->cmd.forwardmove;
    smove = (float)pm->cmd.sidemove;

    wishvel[0] = forward[0] * fmove + right[0] * smove;
    wishvel[1] = forward[1] * fmove + right[1] * smove;
    wishvel[2] = (float)pm->cmd.upmove;

    if (!fmove && !smove && !pm->cmd.upmove)
        wishvel[2] -= 60;  /* sink */

    VectorCopy(wishvel, wishdir);
    wishspeed = VectorNormalize(wishdir);

    if (wishspeed > PM_WATERSPEED)
        wishspeed = PM_WATERSPEED;

    PM_Accelerate(wishdir, wishspeed, PM_WATERACCELERATE);
    PM_StepSlideMove();
}

static void PM_SpectatorMove(void)
{
    vec3_t wishvel;
    float fmove, smove;
    float speed;

    fmove = (float)pm->cmd.forwardmove;
    smove = (float)pm->cmd.sidemove;

    /* Fly movement — no collision */
    pml_velocity[0] = forward[0] * fmove + right[0] * smove;
    pml_velocity[1] = forward[1] * fmove + right[1] * smove;
    pml_velocity[2] = (float)pm->cmd.upmove;

    speed = VectorLength(pml_velocity);
    if (speed > PM_MAXSPEED) {
        VectorScale(pml_velocity, PM_MAXSPEED / speed, pml_velocity);
    }

    pml_origin[0] += pml_velocity[0] * pm_frametime;
    pml_origin[1] += pml_velocity[1] * pm_frametime;
    pml_origin[2] += pml_velocity[2] * pm_frametime;
}

/* ==========================================================================
   Main Pmove Entry Point
   ========================================================================== */

void Pmove(pmove_t *pmove)
{
    pm = pmove;

    /* Compute frametime from command msec */
    pm_frametime = pm->cmd.msec * 0.001f;
    if (pm_frametime > 0.2f)
        pm_frametime = 0.2f;
    if (pm_frametime <= 0)
        return;

    /* Copy state to local */
    VectorCopy(pm->s.origin, pml_origin);
    VectorCopy(pm->s.velocity, pml_velocity);

    /* Set up bounding box */
    pm->mins[0] = -16;
    pm->mins[1] = -16;
    pm->maxs[0] = 16;
    pm->maxs[1] = 16;

    if (pm->s.pm_flags & PMF_DUCKED) {
        pm->mins[2] = -24;
        pm->maxs[2] = 4;
        pm->viewheight = -2;
    } else {
        pm->mins[2] = -24;
        pm->maxs[2] = 32;
        pm->viewheight = 22;
    }

    /* Compute view angles and direction vectors */
    {
        short temp[3];
        int i;
        for (i = 0; i < 3; i++)
            temp[i] = pm->cmd.angles[i] + pm->s.delta_angles[i];
        pm->viewangles[0] = (float)temp[0] * (360.0f / 65536.0f);
        pm->viewangles[1] = (float)temp[1] * (360.0f / 65536.0f);
        pm->viewangles[2] = (float)temp[2] * (360.0f / 65536.0f);
    }

    AngleVectors(pm->viewangles, forward, right, up);

    /* Reset touch list */
    pm->numtouch = 0;

    /* Handle movement based on type */
    switch (pm->s.pm_type) {
    case PM_SPECTATOR:
        PM_SpectatorMove();
        break;

    case PM_DEAD:
    case PM_GIB:
        pm->viewangles[2] = 40;  /* tilt view */
        /* Fall through for gravity */
        /* FALLTHROUGH */

    case PM_NORMAL:
        PM_CategorizePosition();
        PM_Friction();

        /* Gravity */
        if (pm->s.pm_flags & PMF_ON_GROUND) {
            pml_velocity[2] = 0;
        } else {
            pml_velocity[2] -= pm->s.gravity * pm_frametime;
        }

        /* Jump */
        if (pm->s.pm_flags & PMF_ON_GROUND) {
            if (pm->cmd.upmove > 0 && !(pm->s.pm_flags & PMF_JUMP_HELD)) {
                pml_velocity[2] = PM_JUMPSPEED;
                pm->s.pm_flags &= ~PMF_ON_GROUND;
                pm->s.pm_flags |= PMF_JUMP_HELD;
            }
        }

        if (pm->cmd.upmove <= 0)
            pm->s.pm_flags &= ~PMF_JUMP_HELD;

        /* Duck */
        if (pm->cmd.upmove < 0)
            pm->s.pm_flags |= PMF_DUCKED;
        else
            pm->s.pm_flags &= ~PMF_DUCKED;

        /* Move */
        if (pm->waterlevel >= 2) {
            PM_WaterMove();
        } else {
            PM_WalkMove();
        }
        break;

    case PM_FREEZE:
        break;
    }

    /* Clamp velocity */
    PM_ClampVelocity();

    /* Copy local back to state */
    VectorCopy(pml_origin, pm->s.origin);
    VectorCopy(pml_velocity, pm->s.velocity);
}
