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
 * The game module's gi.trace calls this. It first traces against BSP
 * geometry, then checks entity bounding boxes along the trace path.
 */
trace_t SV_Trace(vec3_t start, vec3_t mins, vec3_t maxs,
                 vec3_t end, edict_t *passedict, int contentmask)
{
    trace_t trace;

    /* Start with full-distance trace */
    memset(&trace, 0, sizeof(trace));
    trace.fraction = 1.0f;
    VectorCopy(end, trace.endpos);

    /* TODO: trace against BSP world via CM_BoxTrace */
    /* TODO: trace against entities via SV_AreaEdicts */

    (void)mins; (void)maxs; (void)passedict; (void)contentmask;

    return trace;
}
