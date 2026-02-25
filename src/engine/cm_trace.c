/*
 * cm_trace.c - Collision Model: BSP trace and point contents
 *
 * Implements the core collision detection used by trace() (95 xrefs in game)
 * and pointcontents() (26 xrefs). Based on Quake II's CM module.
 *
 * The BSP collision model uses brushes (convex volumes defined by planes)
 * for all solid geometry. Traces sweep an AABB through the BSP tree and
 * test against brushes in each leaf.
 *
 * Key functions:
 *   CM_BoxTrace     - Sweep AABB from start to end, return first hit
 *   CM_PointContents - Test what material type a point is inside
 *   CM_TransformedBoxTrace - Trace against an inline brush model
 *
 * Original addresses:
 *   CM_BoxTrace:         0x26A40
 *   CM_PointContents:    0x26310
 *   CM_PointLeafnum:     0x262D0
 */

#include "../common/qcommon.h"
#include "../renderer/r_bsp.h"

/* ==========================================================================
   Trace State (per-trace, not global)
   ========================================================================== */

typedef struct {
    vec3_t      start, end;
    vec3_t      mins, maxs;         /* trace box extents */
    vec3_t      extents;            /* max of abs(mins), abs(maxs) per axis */
    vec3_t      absmins, absmaxs;   /* bounds of entire trace motion */

    trace_t     trace;              /* output */
    int         contents;           /* content mask */

    bsp_world_t *world;

    int         checkcount;         /* avoid double-testing brushes */
    qboolean    ispoint;            /* optimization: point trace (zero-size box) */
} trace_work_t;

static int cm_checkcount;           /* global brush check counter */

/* ==========================================================================
   Brush Testing
   ========================================================================== */

/*
 * Test whether a trace passes through a brush.
 * Uses the Separating Axis Theorem on each brush side plane.
 * For a point trace, we just test the point against each plane.
 * For a box trace, we expand the planes by the box extents.
 */
static void CM_TestBrush(trace_work_t *tw, bsp_brush_t *brush)
{
    int             i;
    bsp_plane_t     *plane;
    bsp_brushside_t *side;
    float           enter_frac, leave_frac;
    float           d1, d2;
    qboolean        getout, startout;
    float           f;
    bsp_plane_t     *clipplane;

    if (!brush->numsides)
        return;

    enter_frac = -1;
    leave_frac = 1;
    clipplane = NULL;

    getout = qfalse;
    startout = qfalse;

    for (i = 0; i < brush->numsides; i++) {
        side = &tw->world->brushsides[brush->firstside + i];

        if (side->planenum >= tw->world->num_planes)
            continue;

        plane = &tw->world->planes[side->planenum];

        /* Calculate distance from start and end to plane */
        if (tw->ispoint) {
            d1 = DotProduct(tw->start, plane->normal) - plane->dist;
            d2 = DotProduct(tw->end, plane->normal) - plane->dist;
        } else {
            /* Expand plane by box extents */
            float ofs_x, ofs_y, ofs_z;

            ofs_x = (plane->normal[0] < 0) ? tw->maxs[0] : tw->mins[0];
            ofs_y = (plane->normal[1] < 0) ? tw->maxs[1] : tw->mins[1];
            ofs_z = (plane->normal[2] < 0) ? tw->maxs[2] : tw->mins[2];

            d1 = DotProduct(tw->start, plane->normal) -
                 (plane->dist - (plane->normal[0] * ofs_x +
                                 plane->normal[1] * ofs_y +
                                 plane->normal[2] * ofs_z));
            d2 = DotProduct(tw->end, plane->normal) -
                 (plane->dist - (plane->normal[0] * ofs_x +
                                 plane->normal[1] * ofs_y +
                                 plane->normal[2] * ofs_z));
        }

        if (d2 > 0)
            getout = qtrue;     /* endpoint is outside this plane */

        if (d1 > 0)
            startout = qtrue;   /* startpoint is outside this plane */

        /* If completely in front of plane, no intersection with this brush */
        if (d1 > 0 && (d2 >= 0.03125f || d2 >= d1))
            return;

        /* If completely behind plane, continue checking other sides */
        if (d1 <= 0 && d2 <= 0)
            continue;

        /* Entering or leaving? */
        if (d1 > d2) {
            /* Entering — we're going from outside to inside */
            f = (d1 - 0.03125f) / (d1 - d2);
            if (f < 0) f = 0;
            if (f > enter_frac) {
                enter_frac = f;
                clipplane = plane;
            }
        } else {
            /* Leaving — we're going from inside to outside */
            f = (d1 + 0.03125f) / (d1 - d2);
            if (f > 1) f = 1;
            if (f < leave_frac)
                leave_frac = f;
        }
    }

    /* All planes checked */
    if (!startout) {
        /* Start point was inside all planes — we started inside the brush */
        tw->trace.startsolid = qtrue;
        if (!getout)
            tw->trace.allsolid = qtrue;
        return;
    }

    if (enter_frac < leave_frac) {
        if (enter_frac > -1 && enter_frac < tw->trace.fraction) {
            if (enter_frac < 0)
                enter_frac = 0;
            tw->trace.fraction = enter_frac;
            if (clipplane) {
                VectorCopy(clipplane->normal, tw->trace.plane.normal);
                tw->trace.plane.dist = clipplane->dist;
            }
            tw->trace.contents = brush->contents;
        }
    }
}

/* ==========================================================================
   Leaf Testing
   ========================================================================== */

/*
 * Test a trace against all brushes in a leaf
 */
static void CM_TestInLeaf(trace_work_t *tw, bsp_leaf_t *leaf)
{
    int             i;
    bsp_brush_t     *brush;

    if (!(leaf->contents & tw->contents))
        return;

    /* Use leafbrushes array for proper per-leaf brush lookup */
    if (tw->world->leafbrushes && leaf->numleafbrushes > 0) {
        for (i = 0; i < leaf->numleafbrushes; i++) {
            int lb_idx = leaf->firstleafbrush + i;
            int brushnum;

            if (lb_idx >= tw->world->num_leafbrushes)
                break;

            brushnum = tw->world->leafbrushes[lb_idx];
            if (brushnum >= tw->world->num_brushes)
                continue;

            brush = &tw->world->brushes[brushnum];

            if (!(brush->contents & tw->contents))
                continue;

            CM_TestBrush(tw, brush);

            if (tw->trace.allsolid)
                return;
        }
    } else {
        /* Fallback: test against all world brushes */
        for (i = 0; i < tw->world->num_brushes; i++) {
            brush = &tw->world->brushes[i];

            if (!(brush->contents & tw->contents))
                continue;

            CM_TestBrush(tw, brush);

            if (tw->trace.allsolid)
                return;
        }
    }
}

/* ==========================================================================
   BSP Tree Recursion
   ========================================================================== */

/*
 * Recursively trace through the BSP tree
 */
static void CM_RecursiveTrace(trace_work_t *tw, int num,
                              float p1f, float p2f,
                              vec3_t p1, vec3_t p2)
{
    bsp_node_t  *node;
    bsp_plane_t *plane;
    float       t1, t2, offset;
    float       frac, frac2;
    float       idist;
    int         side;
    float       midf;
    vec3_t      mid;

    if (tw->trace.fraction <= p1f)
        return;     /* already hit something nearer */

    /* Leaf node */
    if (num < 0) {
        int leafnum = -(num + 1);
        if (leafnum < tw->world->num_leafs) {
            CM_TestInLeaf(tw, &tw->world->leafs[leafnum]);
        }
        return;
    }

    /* BSP node — split along plane */
    if (num >= tw->world->num_nodes)
        return;

    node = &tw->world->nodes[num];
    if (node->planenum < 0 || node->planenum >= tw->world->num_planes)
        return;

    plane = &tw->world->planes[node->planenum];

    /* Calculate distances to the splitting plane */
    t1 = DotProduct(p1, plane->normal) - plane->dist;
    t2 = DotProduct(p2, plane->normal) - plane->dist;

    /* Expand by box extents for AABB trace */
    if (tw->ispoint) {
        offset = 0;
    } else {
        offset = (float)(fabs(tw->extents[0] * plane->normal[0]) +
                         fabs(tw->extents[1] * plane->normal[1]) +
                         fabs(tw->extents[2] * plane->normal[2]));
    }

    /* Both sides? */
    if (t1 >= offset + 1 && t2 >= offset + 1) {
        CM_RecursiveTrace(tw, node->children[0], p1f, p2f, p1, p2);
        return;
    }
    if (t1 < -offset - 1 && t2 < -offset - 1) {
        CM_RecursiveTrace(tw, node->children[1], p1f, p2f, p1, p2);
        return;
    }

    /* Trace crosses the plane — split into two halves */
    if (t1 < t2) {
        idist = 1.0f / (t1 - t2);
        side = 1;
        frac2 = (t1 + offset + 0.03125f) * idist;
        frac = (t1 - offset + 0.03125f) * idist;
    } else if (t1 > t2) {
        idist = 1.0f / (t1 - t2);
        side = 0;
        frac2 = (t1 - offset - 0.03125f) * idist;
        frac = (t1 + offset + 0.03125f) * idist;
    } else {
        side = 0;
        frac = 1;
        frac2 = 0;
    }

    /* Clamp fractions */
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    if (frac2 < 0) frac2 = 0;
    if (frac2 > 1) frac2 = 1;

    /* First half */
    midf = p1f + (p2f - p1f) * frac;
    mid[0] = p1[0] + frac * (p2[0] - p1[0]);
    mid[1] = p1[1] + frac * (p2[1] - p1[1]);
    mid[2] = p1[2] + frac * (p2[2] - p1[2]);

    CM_RecursiveTrace(tw, node->children[side], p1f, midf, p1, mid);

    /* Second half */
    midf = p1f + (p2f - p1f) * frac2;
    mid[0] = p1[0] + frac2 * (p2[0] - p1[0]);
    mid[1] = p1[1] + frac2 * (p2[1] - p1[1]);
    mid[2] = p1[2] + frac2 * (p2[2] - p1[2]);

    CM_RecursiveTrace(tw, node->children[side ^ 1], midf, p2f, mid, p2);
}

/* ==========================================================================
   Public API
   ========================================================================== */

/*
 * CM_BoxTrace - Sweep an AABB from start to end through the BSP world
 *
 * This is the function behind game_import_t.trace (95 xrefs).
 * Returns the first collision along the ray.
 */
trace_t CM_BoxTrace(bsp_world_t *world,
                    vec3_t start, vec3_t mins, vec3_t maxs,
                    vec3_t end, int brushmask)
{
    trace_work_t tw;
    int i;

    memset(&tw, 0, sizeof(tw));
    tw.world = world;
    tw.contents = brushmask;

    /* Initialize trace result */
    tw.trace.fraction = 1.0f;
    tw.trace.surface = NULL;
    tw.trace.ent = NULL;
    VectorCopy(end, tw.trace.endpos);

    if (!world || !world->loaded || !world->nodes) {
        return tw.trace;
    }

    /* Copy trace parameters */
    VectorCopy(start, tw.start);
    VectorCopy(end, tw.end);

    if (mins) {
        VectorCopy(mins, tw.mins);
    } else {
        VectorClear(tw.mins);
    }

    if (maxs) {
        VectorCopy(maxs, tw.maxs);
    } else {
        VectorClear(tw.maxs);
    }

    /* Check if this is a point trace (zero-size box) */
    tw.ispoint = (tw.mins[0] == 0 && tw.mins[1] == 0 && tw.mins[2] == 0 &&
                  tw.maxs[0] == 0 && tw.maxs[1] == 0 && tw.maxs[2] == 0);

    /* Calculate extents for box expansion */
    for (i = 0; i < 3; i++) {
        float a = (float)fabs(tw.mins[i]);
        float b = (float)fabs(tw.maxs[i]);
        tw.extents[i] = (a > b) ? a : b;
    }

    /* Calculate bounds of entire trace motion */
    for (i = 0; i < 3; i++) {
        if (end[i] > start[i]) {
            tw.absmins[i] = start[i] + tw.mins[i] - 1;
            tw.absmaxs[i] = end[i] + tw.maxs[i] + 1;
        } else {
            tw.absmins[i] = end[i] + tw.mins[i] - 1;
            tw.absmaxs[i] = start[i] + tw.maxs[i] + 1;
        }
    }

    /* Trace through BSP tree */
    cm_checkcount++;
    CM_RecursiveTrace(&tw, 0, 0, 1, start, end);

    /* Calculate final endpoint */
    if (tw.trace.fraction == 1.0f) {
        VectorCopy(end, tw.trace.endpos);
    } else {
        for (i = 0; i < 3; i++) {
            tw.trace.endpos[i] = start[i] +
                tw.trace.fraction * (end[i] - start[i]);
        }
    }

    return tw.trace;
}

/*
 * CM_PointContents - What content type is a point inside?
 *
 * This is the function behind game_import_t.pointcontents (26 xrefs).
 * Returns CONTENTS_* flags (SOLID, WATER, LAVA, etc.)
 */
int CM_PointContents(bsp_world_t *world, vec3_t p)
{
    int         leafnum;
    bsp_leaf_t  *leaf;

    if (!world || !world->loaded)
        return 0;

    leafnum = BSP_PointLeaf(world, p);
    if (leafnum < 0 || leafnum >= world->num_leafs)
        return 0;

    leaf = &world->leafs[leafnum];
    return leaf->contents;
}

/*
 * CM_TransformedBoxTrace - Trace against an inline brush model (doors, etc.)
 *
 * Translates the trace into model space, traces, then translates back.
 * Used for moveable BSP objects like doors, platforms, trains.
 */
trace_t CM_TransformedBoxTrace(bsp_world_t *world,
                               vec3_t start, vec3_t mins, vec3_t maxs,
                               vec3_t end, int headnode,
                               int brushmask, vec3_t origin, vec3_t angles)
{
    trace_t tr;
    vec3_t  start_l, end_l;

    (void)headnode;
    (void)angles;   /* TODO: rotated brush models */

    /* Translate into model space */
    VectorSubtract(start, origin, start_l);
    VectorSubtract(end, origin, end_l);

    /* Trace in local space */
    tr = CM_BoxTrace(world, start_l, mins, maxs, end_l, brushmask);

    /* Translate result back */
    if (tr.fraction < 1.0f) {
        int i;
        for (i = 0; i < 3; i++) {
            tr.endpos[i] = start[i] + tr.fraction * (end[i] - start[i]);
        }
    } else {
        VectorCopy(end, tr.endpos);
    }

    return tr;
}
