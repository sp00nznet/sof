/*
 * sv_world.c - Entity area linking and spatial queries
 *
 * Manages entity-to-world spatial relationships using an area node tree.
 * Entities are linked into BSP area nodes for efficient spatial queries.
 *
 * Key functions:
 *   SV_LinkEdict   — Insert entity into world, compute absmin/absmax
 *   SV_UnlinkEdict — Remove entity from world
 *   SV_AreaEdicts  — Find all entities within an AABB (for triggers, combat)
 *
 * Original SoF: SV_LinkEdict=0x2A870, SV_AreaEdicts=0x2A680
 * Based on Q2 sv_world.c (id Software GPL)
 */

#include "../common/qcommon.h"
#include "../game/g_local.h"
#include "../renderer/r_bsp.h"

/* ==========================================================================
   Area Node Tree
   Subdivides world space for efficient entity spatial queries.
   ========================================================================== */

#define AREA_DEPTH      4
#define AREA_NODES      32      /* 2^(AREA_DEPTH+1) - 1 */

typedef struct areanode_s {
    int     axis;           /* -1 = leaf node */
    float   dist;           /* split distance */
    int     children[2];    /* child node indices */

    /* Linked list of entities in this node */
    int     num_entities;
    edict_t *entities[256]; /* Max entities per node */
} areanode_t;

static areanode_t   sv_areanodes[AREA_NODES];
static int          sv_numareanodes;

static vec3_t       world_mins, world_maxs;

/* ==========================================================================
   Area Node Construction
   ========================================================================== */

static int SV_CreateAreaNode(int depth, vec3_t mins, vec3_t maxs)
{
    areanode_t *anode;
    vec3_t size, mins1, maxs1, mins2, maxs2;
    int idx;

    idx = sv_numareanodes;
    if (sv_numareanodes >= AREA_NODES)
        return 0;

    anode = &sv_areanodes[sv_numareanodes++];
    memset(anode, 0, sizeof(*anode));

    if (depth == AREA_DEPTH) {
        anode->axis = -1;  /* leaf */
        anode->children[0] = -1;
        anode->children[1] = -1;
        return idx;
    }

    VectorSubtract(maxs, mins, size);
    if (size[0] > size[1])
        anode->axis = 0;
    else
        anode->axis = 1;

    anode->dist = 0.5f * (maxs[anode->axis] + mins[anode->axis]);

    VectorCopy(mins, mins1);
    VectorCopy(maxs, maxs1);
    VectorCopy(mins, mins2);
    VectorCopy(maxs, maxs2);

    maxs1[anode->axis] = anode->dist;
    mins2[anode->axis] = anode->dist;

    anode->children[0] = SV_CreateAreaNode(depth + 1, mins2, maxs2);
    anode->children[1] = SV_CreateAreaNode(depth + 1, mins1, maxs1);

    return idx;
}

void SV_ClearWorld(void)
{
    sv_numareanodes = 0;

    /* Use default world bounds if no BSP loaded */
    VectorSet(world_mins, -4096, -4096, -4096);
    VectorSet(world_maxs, 4096, 4096, 4096);

    SV_CreateAreaNode(0, world_mins, world_maxs);
}

void SV_SetWorldBounds(vec3_t mins, vec3_t maxs)
{
    VectorCopy(mins, world_mins);
    VectorCopy(maxs, world_maxs);
    sv_numareanodes = 0;
    SV_CreateAreaNode(0, world_mins, world_maxs);
}

/* ==========================================================================
   Entity Linking
   ========================================================================== */

static void SV_LinkToAreaNode(edict_t *ent, int node_idx)
{
    areanode_t *anode;

    if (node_idx < 0 || node_idx >= sv_numareanodes)
        return;

    anode = &sv_areanodes[node_idx];

    /* If leaf node, add entity here */
    if (anode->axis == -1) {
        if (anode->num_entities < 256) {
            anode->entities[anode->num_entities++] = ent;
        }
        return;
    }

    /* Check which child(ren) the entity overlaps */
    if (ent->absmax[anode->axis] > anode->dist)
        SV_LinkToAreaNode(ent, anode->children[0]);
    if (ent->absmin[anode->axis] < anode->dist)
        SV_LinkToAreaNode(ent, anode->children[1]);
}

void SV_LinkEdict(edict_t *ent)
{
    if (!ent)
        return;

    /* Calculate absmin/absmax from origin + mins/maxs */
    VectorAdd(ent->s.origin, ent->mins, ent->absmin);
    VectorAdd(ent->s.origin, ent->maxs, ent->absmax);

    /* Expand slightly for floating point imprecision */
    ent->absmin[0] -= 1;
    ent->absmin[1] -= 1;
    ent->absmin[2] -= 1;
    ent->absmax[0] += 1;
    ent->absmax[1] += 1;
    ent->absmax[2] += 1;

    /* Link into area nodes */
    ent->linked = qtrue;
    if (sv_numareanodes > 0)
        SV_LinkToAreaNode(ent, 0);
}

void SV_UnlinkEdict(edict_t *ent)
{
    int i, j;

    if (!ent)
        return;

    ent->linked = qfalse;

    /* Remove from all area nodes */
    for (i = 0; i < sv_numareanodes; i++) {
        areanode_t *anode = &sv_areanodes[i];
        for (j = 0; j < anode->num_entities; j++) {
            if (anode->entities[j] == ent) {
                anode->entities[j] = anode->entities[anode->num_entities - 1];
                anode->num_entities--;
                break;
            }
        }
    }
}

/* ==========================================================================
   Area Queries
   ========================================================================== */

typedef struct {
    edict_t **list;
    int     count;
    int     maxcount;
    vec3_t  mins;
    vec3_t  maxs;
    int     areatype;
} areaparms_t;

static void SV_AreaEdicts_r(int node_idx, areaparms_t *ap)
{
    areanode_t *anode;
    int i;

    if (node_idx < 0 || node_idx >= sv_numareanodes)
        return;

    anode = &sv_areanodes[node_idx];

    /* Check entities in this node */
    for (i = 0; i < anode->num_entities; i++) {
        edict_t *ent = anode->entities[i];

        if (!ent || !ent->inuse || !ent->linked)
            continue;

        /* AABB overlap test */
        if (ent->absmin[0] > ap->maxs[0] ||
            ent->absmin[1] > ap->maxs[1] ||
            ent->absmin[2] > ap->maxs[2] ||
            ent->absmax[0] < ap->mins[0] ||
            ent->absmax[1] < ap->mins[1] ||
            ent->absmax[2] < ap->mins[2])
            continue;

        if (ap->count >= ap->maxcount) {
            Com_Printf("SV_AreaEdicts: maxcount exceeded\n");
            return;
        }

        ap->list[ap->count++] = ent;
    }

    /* Recurse into children if not a leaf */
    if (anode->axis != -1) {
        if (ap->maxs[anode->axis] > anode->dist)
            SV_AreaEdicts_r(anode->children[0], ap);
        if (ap->mins[anode->axis] < anode->dist)
            SV_AreaEdicts_r(anode->children[1], ap);
    }
}

int SV_AreaEdicts(vec3_t mins, vec3_t maxs, edict_t **list,
                  int maxcount, int areatype)
{
    areaparms_t ap;

    ap.list = list;
    ap.count = 0;
    ap.maxcount = maxcount;
    VectorCopy(mins, ap.mins);
    VectorCopy(maxs, ap.maxs);
    ap.areatype = areatype;

    if (sv_numareanodes > 0)
        SV_AreaEdicts_r(0, &ap);

    return ap.count;
}

/* ==========================================================================
   Server-side Trace (through entities)
   ========================================================================== */

/*
 * SV_Trace - Trace through world and entities
 *
 * Legacy entry point. The game module's gi.trace is wired to GI_trace
 * in sv_game.c which already calls CM_BoxTrace + SV_AreaEdicts.
 * This function delegates there for any direct callers.
 */

extern bsp_world_t *R_GetWorldModel(void);

trace_t SV_Trace(vec3_t start, vec3_t mins, vec3_t maxs,
                 vec3_t end, edict_t *passedict, int contentmask)
{
    trace_t trace;
    bsp_world_t *world = R_GetWorldModel();

    /* Trace against BSP world */
    if (world && world->loaded) {
        trace = CM_BoxTrace(world, start, mins, maxs, end, contentmask);
    } else {
        memset(&trace, 0, sizeof(trace));
        trace.fraction = 1.0f;
        VectorCopy(end, trace.endpos);
    }

    /* Trace against solid entities */
    {
        edict_t *touch[64];
        int num_touch, i;
        vec3_t trace_mins, trace_maxs;

        for (i = 0; i < 3; i++) {
            if (start[i] < end[i]) {
                trace_mins[i] = start[i] + (mins ? mins[i] : 0);
                trace_maxs[i] = end[i] + (maxs ? maxs[i] : 0);
            } else {
                trace_mins[i] = end[i] + (mins ? mins[i] : 0);
                trace_maxs[i] = start[i] + (maxs ? maxs[i] : 0);
            }
        }

        num_touch = SV_AreaEdicts(trace_mins, trace_maxs, touch, 64, AREA_SOLID);

        for (i = 0; i < num_touch; i++) {
            edict_t *ent = touch[i];

            if (!ent || ent == passedict || !ent->inuse)
                continue;
            if (ent->solid == SOLID_NOT || ent->solid == SOLID_TRIGGER)
                continue;
            if (passedict && ent->owner == passedict)
                continue;

            /* Simple AABB clip test */
            {
                vec3_t ent_expanded_mins, ent_expanded_maxs;
                float t_entry = 0.0f, t_exit = 1.0f;
                vec3_t dir;
                int j;
                qboolean hit = qtrue;

                VectorSubtract(end, start, dir);

                for (j = 0; j < 3; j++) {
                    ent_expanded_mins[j] = ent->absmin[j] - (maxs ? maxs[j] : 0);
                    ent_expanded_maxs[j] = ent->absmax[j] - (mins ? mins[j] : 0);
                }

                for (j = 0; j < 3; j++) {
                    if (dir[j] == 0.0f) {
                        if (start[j] < ent_expanded_mins[j] || start[j] > ent_expanded_maxs[j]) {
                            hit = qfalse;
                            break;
                        }
                    } else {
                        float t1 = (ent_expanded_mins[j] - start[j]) / dir[j];
                        float t2 = (ent_expanded_maxs[j] - start[j]) / dir[j];
                        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
                        if (t1 > t_entry) t_entry = t1;
                        if (t2 < t_exit) t_exit = t2;
                        if (t_entry > t_exit) { hit = qfalse; break; }
                    }
                }

                if (hit && t_entry >= 0.0f && t_entry < trace.fraction) {
                    trace.fraction = t_entry;
                    for (j = 0; j < 3; j++)
                        trace.endpos[j] = start[j] + dir[j] * t_entry;
                    trace.ent = ent;
                }
            }
        }
    }

    return trace;
}
