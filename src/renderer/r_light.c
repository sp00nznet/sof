/*
 * r_light.c - BSP lightmap loading and rendering
 *
 * Loads lightmap data from BSP LUMP_LIGHTING and builds per-face GL
 * textures for real-time lighting. Q2 BSP lightmaps are computed by
 * the map compiler (qrad/arghrad) and stored as 8-bit greyscale
 * luxels (or RGB for SoF's ArghRad extended lighting).
 *
 * Lightmap dimensions per face are derived from the face's texture-space
 * extents: the face vertices are projected onto the texinfo s/t vectors
 * and the range determines the lightmap size (1 luxel per 16 world units).
 *
 * Original ref_gl.dll: GL_BuildLightmaps at 0x3000E530
 */

#include "r_local.h"
#include "r_bsp.h"

#include <math.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE    0x812F
#endif

/* ==========================================================================
   Lightmap Atlas
   Packs all face lightmaps into a small number of GL textures.
   ========================================================================== */

#define LM_BLOCK_WIDTH      512
#define LM_BLOCK_HEIGHT     512
#define MAX_LIGHTMAPS       64

/* Per-face lightmap info */
typedef struct {
    int     atlas_index;        /* which atlas texture */
    int     atlas_x, atlas_y;   /* position in atlas */
    int     width, height;      /* lightmap dimensions in luxels */
    float   s_offset, t_offset; /* tex coord offset for face */
    float   s_scale, t_scale;   /* tex coord scale */
} face_lightmap_t;

static GLuint           lm_textures[MAX_LIGHTMAPS];
static int              lm_num_textures;
static byte             lm_buffer[LM_BLOCK_WIDTH * LM_BLOCK_HEIGHT * 3]; /* RGB */
static int              lm_allocated[LM_BLOCK_WIDTH]; /* row allocation per column */

static face_lightmap_t  *lm_faces;
static int              lm_num_faces;
static bsp_world_t     *lm_world;  /* pointer to current world for R_LightPoint */

/* ==========================================================================
   Lightmap Atlas Packing
   Simple shelf packing: track allocated height per column.
   ========================================================================== */

static void LM_InitBlock(void)
{
    memset(lm_allocated, 0, sizeof(lm_allocated));
    memset(lm_buffer, 0, sizeof(lm_buffer));
}

/*
 * Find a free region in the current lightmap atlas block.
 * Returns qtrue and sets *x, *y if space found.
 */
static qboolean LM_AllocBlock(int w, int h, int *x, int *y)
{
    int i, j;
    int best = LM_BLOCK_HEIGHT;

    for (i = 0; i <= LM_BLOCK_WIDTH - w; i++) {
        int best2 = 0;
        int fail = 0;

        for (j = 0; j < w; j++) {
            if (lm_allocated[i + j] >= LM_BLOCK_HEIGHT) {
                fail = 1;
                break;
            }
            if (lm_allocated[i + j] > best2)
                best2 = lm_allocated[i + j];
        }

        if (fail)
            continue;

        if (best2 + h > LM_BLOCK_HEIGHT)
            continue;

        if (best2 < best) {
            best = best2;
            *x = i;
            *y = best;
        }
    }

    if (best + h > LM_BLOCK_HEIGHT)
        return qfalse;

    for (i = 0; i < w; i++)
        lm_allocated[*x + i] = *y + h;

    return qtrue;
}

/*
 * Upload the current atlas block as a GL texture and start a new one.
 */
static void LM_UploadBlock(void)
{
    GLuint tex;

    if (lm_num_textures >= MAX_LIGHTMAPS) {
        Com_Printf("WARNING: MAX_LIGHTMAPS exceeded\n");
        return;
    }

    qglGenTextures(1, &tex);
    qglBindTexture(GL_TEXTURE_2D, tex);
    qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, LM_BLOCK_WIDTH, LM_BLOCK_HEIGHT,
                  0, GL_RGB, GL_UNSIGNED_BYTE, lm_buffer);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    lm_textures[lm_num_textures] = tex;
    lm_num_textures++;

    LM_InitBlock();
}

/* ==========================================================================
   Face Lightmap Computation
   ========================================================================== */

/*
 * Compute lightmap dimensions for a face from its texture-space extents.
 * Q2 standard: 1 luxel per 16 world units along texture axes.
 */
static void R_CalcFaceLightmapExtents(bsp_world_t *world, int face_idx,
                                       float *s_min, float *t_min,
                                       int *lm_w, int *lm_h)
{
    bsp_face_t *face = &world->faces[face_idx];
    bsp_texinfo_t *ti;
    float mins[2], maxs[2];
    int i, bmins[2], bmaxs[2];

    if (face->texinfo < 0 || face->texinfo >= world->num_texinfo)
        return;

    ti = &world->texinfo[face->texinfo];
    mins[0] = mins[1] = 999999;
    maxs[0] = maxs[1] = -999999;

    /* Project all face vertices onto texture axes */
    for (i = 0; i < face->numedges; i++) {
        int edge_idx = world->surfedges[face->firstedge + i];
        float *v;

        if (edge_idx >= 0)
            v = world->vertexes[world->edges[edge_idx].v[0]].point;
        else
            v = world->vertexes[world->edges[-edge_idx].v[1]].point;

        {
            float s = DotProduct(v, ti->vecs[0]) + ti->vecs[0][3];
            float t = DotProduct(v, ti->vecs[1]) + ti->vecs[1][3];

            if (s < mins[0]) mins[0] = s;
            if (s > maxs[0]) maxs[0] = s;
            if (t < mins[1]) mins[1] = t;
            if (t > maxs[1]) maxs[1] = t;
        }
    }

    /* Convert to lightmap luxel coordinates (1 luxel per 16 units) */
    bmins[0] = (int)floor(mins[0] / 16.0f);
    bmins[1] = (int)floor(mins[1] / 16.0f);
    bmaxs[0] = (int)ceil(maxs[0] / 16.0f);
    bmaxs[1] = (int)ceil(maxs[1] / 16.0f);

    *s_min = (float)(bmins[0] * 16);
    *t_min = (float)(bmins[1] * 16);
    *lm_w = bmaxs[0] - bmins[0] + 1;
    *lm_h = bmaxs[1] - bmins[1] + 1;

    /* Sanity clamp */
    if (*lm_w < 1) *lm_w = 1;
    if (*lm_h < 1) *lm_h = 1;
    if (*lm_w > 256) *lm_w = 256;
    if (*lm_h > 256) *lm_h = 256;
}

/* ==========================================================================
   Build Lightmaps
   Called after map load to create all face lightmap textures.
   ========================================================================== */

void R_BuildLightmaps(bsp_world_t *world)
{
    int     i;
    int     built = 0;

    lm_world = world;

    /* Free previous */
    R_FreeLightmaps();

    if (!world->lightdata || world->lightdata_size == 0) {
        Com_Printf("No lightmap data in BSP\n");
        return;
    }

    lm_num_faces = world->num_faces;
    lm_faces = (face_lightmap_t *)Z_TagMalloc(
        sizeof(face_lightmap_t) * lm_num_faces, Z_TAG_LEVEL);
    memset(lm_faces, 0, sizeof(face_lightmap_t) * lm_num_faces);

    LM_InitBlock();
    lm_num_textures = 0;

    for (i = 0; i < world->num_faces; i++) {
        bsp_face_t *face = &world->faces[i];
        face_lightmap_t *flm = &lm_faces[i];
        float s_min, t_min;
        int lm_w, lm_h;
        int ax, ay;

        flm->atlas_index = -1;

        /* Skip faces with no lightmap */
        if (face->lightofs < 0)
            continue;

        /* Skip special surfaces */
        if (face->texinfo >= 0 && face->texinfo < world->num_texinfo) {
            int flags = world->texinfo[face->texinfo].flags;
            if (flags & (SURF_SKY | SURF_WARP | SURF_NODRAW))
                continue;
        }

        /* Calculate lightmap dimensions */
        R_CalcFaceLightmapExtents(world, i, &s_min, &t_min, &lm_w, &lm_h);

        /* Try to allocate in current block */
        if (!LM_AllocBlock(lm_w, lm_h, &ax, &ay)) {
            /* Upload current block and start new one */
            LM_UploadBlock();
            if (!LM_AllocBlock(lm_w, lm_h, &ax, &ay)) {
                Com_DPrintf("WARNING: lightmap too large (%dx%d) for face %d\n",
                           lm_w, lm_h, i);
                continue;
            }
        }

        /* Copy lightmap data into atlas buffer */
        {
            int x, y;
            byte *src = world->lightdata + face->lightofs;
            int src_size = lm_w * lm_h;

            /* Verify source data fits */
            if (face->lightofs + src_size > world->lightdata_size) {
                Com_DPrintf("WARNING: lightmap overflow for face %d\n", i);
                continue;
            }

            /* Q2 lightmaps: check if RGB (SoF ArghRad) or greyscale */
            /* SoF typically has 3 bytes per luxel (RGB lighting) */
            if (face->lightofs + src_size * 3 <= world->lightdata_size) {
                /* Assume RGB lightmap data (3 bytes per luxel) */
                for (y = 0; y < lm_h; y++) {
                    for (x = 0; x < lm_w; x++) {
                        int dst_ofs = ((ay + y) * LM_BLOCK_WIDTH + (ax + x)) * 3;
                        int src_ofs = (y * lm_w + x) * 3;
                        /* Apply overbright (Q2 style: lightmap * 2) */
                        int r = src[src_ofs + 0] * 2;
                        int g = src[src_ofs + 1] * 2;
                        int b = src[src_ofs + 2] * 2;
                        lm_buffer[dst_ofs + 0] = (byte)(r > 255 ? 255 : r);
                        lm_buffer[dst_ofs + 1] = (byte)(g > 255 ? 255 : g);
                        lm_buffer[dst_ofs + 2] = (byte)(b > 255 ? 255 : b);
                    }
                }
            } else {
                /* Greyscale fallback (1 byte per luxel) */
                for (y = 0; y < lm_h; y++) {
                    for (x = 0; x < lm_w; x++) {
                        int dst_ofs = ((ay + y) * LM_BLOCK_WIDTH + (ax + x)) * 3;
                        int src_ofs = y * lm_w + x;
                        byte val = src[src_ofs];
                        int lit = val * 2;
                        if (lit > 255) lit = 255;
                        lm_buffer[dst_ofs + 0] = (byte)lit;
                        lm_buffer[dst_ofs + 1] = (byte)lit;
                        lm_buffer[dst_ofs + 2] = (byte)lit;
                    }
                }
            }
        }

        /* Store face lightmap info */
        flm->atlas_index = lm_num_textures;
        flm->atlas_x = ax;
        flm->atlas_y = ay;
        flm->width = lm_w;
        flm->height = lm_h;

        /* Texture coordinate transform for this face's lightmap:
         * lm_s = (s - s_min) / 16 + 0.5  → then normalize to atlas
         * lm_t = (t - t_min) / 16 + 0.5  → then normalize to atlas */
        flm->s_offset = s_min;
        flm->t_offset = t_min;
        flm->s_scale = 1.0f / 16.0f;  /* luxels per 16 world units */

        built++;
    }

    /* Upload final block */
    if (built > 0 || lm_num_textures == 0)
        LM_UploadBlock();

    Com_Printf("Lightmaps: %d faces, %d atlas textures (%dx%d)\n",
               built, lm_num_textures, LM_BLOCK_WIDTH, LM_BLOCK_HEIGHT);
}

/* ==========================================================================
   Lightmap Texture Coordinate Computation
   ========================================================================== */

/*
 * R_GetFaceLightmapTC - Compute lightmap texture coordinates for a vertex
 *
 * Given a face index and vertex position, computes the atlas UV for the
 * lightmap. Returns qfalse if this face has no lightmap.
 */
qboolean R_GetFaceLightmapTC(int face_idx, float *vertex,
                              bsp_texinfo_t *ti,
                              float *out_s, float *out_t,
                              GLuint *out_texnum)
{
    face_lightmap_t *flm;

    if (!lm_faces || face_idx < 0 || face_idx >= lm_num_faces)
        return qfalse;

    flm = &lm_faces[face_idx];
    if (flm->atlas_index < 0 || flm->atlas_index >= lm_num_textures)
        return qfalse;

    {
        float s = DotProduct(vertex, ti->vecs[0]) + ti->vecs[0][3];
        float t = DotProduct(vertex, ti->vecs[1]) + ti->vecs[1][3];

        /* Convert to lightmap space: luxel coords within this face's lightmap */
        float ls = (s - flm->s_offset) / 16.0f + 0.5f;
        float lt = (t - flm->t_offset) / 16.0f + 0.5f;

        /* Convert to atlas UV */
        *out_s = (flm->atlas_x + ls) / (float)LM_BLOCK_WIDTH;
        *out_t = (flm->atlas_y + lt) / (float)LM_BLOCK_HEIGHT;
        *out_texnum = lm_textures[flm->atlas_index];
    }

    return qtrue;
}

/*
 * R_GetFaceLightmapTexture - Get the GL texture for a face's lightmap atlas
 */
GLuint R_GetFaceLightmapTexture(int face_idx)
{
    if (!lm_faces || face_idx < 0 || face_idx >= lm_num_faces)
        return 0;
    if (lm_faces[face_idx].atlas_index < 0)
        return 0;
    return lm_textures[lm_faces[face_idx].atlas_index];
}

/* ==========================================================================
   R_LightPoint - Sample world light at a position

   Searches nearby floor faces to find the lightmap color at the given
   world position. Returns an RGB light value (0-1 range).
   ========================================================================== */

void R_LightPoint(vec3_t p, vec3_t color)
{
    int i;
    float best_dist = 999999.0f;
    int best_face = -1;

    color[0] = color[1] = color[2] = 1.0f;  /* default: full bright */

    if (!lm_world || !lm_world->lightdata || !lm_faces)
        return;

    /* Find the closest upward-facing face below this point */
    for (i = 0; i < lm_world->num_faces; i++) {
        bsp_face_t *face = &lm_world->faces[i];
        bsp_plane_t *plane = &lm_world->planes[face->planenum];
        float dot, dist;

        if (face->lightofs < 0)
            continue;

        /* Only consider mostly-horizontal surfaces (floors/ceilings) */
        dot = plane->normal[2];
        if (face->side) dot = -dot;
        if (dot < 0.7f)
            continue;  /* not floor-like */

        /* Distance from point to plane */
        dist = p[0] * plane->normal[0] + p[1] * plane->normal[1] +
               p[2] * plane->normal[2] - plane->dist;
        if (face->side)
            dist = -dist;

        /* Only faces below us (positive dist means point is above plane) */
        if (dist < -16.0f || dist > 256.0f)
            continue;

        if (dist < best_dist) {
            /* Check if point is within face's XY bounding box (rough) */
            bsp_texinfo_t *ti = &lm_world->texinfo[face->texinfo];
            float s = p[0] * ti->vecs[0][0] + p[1] * ti->vecs[0][1] +
                      p[2] * ti->vecs[0][2] + ti->vecs[0][3];
            float t = p[0] * ti->vecs[1][0] + p[1] * ti->vecs[1][1] +
                      p[2] * ti->vecs[1][2] + ti->vecs[1][3];
            face_lightmap_t *flm = &lm_faces[i];

            /* Check if s/t coordinates are within this face's lightmap range */
            if (flm->width > 0 && flm->height > 0) {
                /* Use this face's texinfo to compute lightmap position */
                best_dist = dist;
                best_face = i;
            }
        }
    }

    /* Sample the lightmap from the best face */
    if (best_face >= 0) {
        bsp_face_t *face = &lm_world->faces[best_face];
        bsp_texinfo_t *ti = &lm_world->texinfo[face->texinfo];
        face_lightmap_t *flm = &lm_faces[best_face];
        float s, t;
        int ls, lt;

        s = p[0] * ti->vecs[0][0] + p[1] * ti->vecs[0][1] +
            p[2] * ti->vecs[0][2] + ti->vecs[0][3];
        t = p[0] * ti->vecs[1][0] + p[1] * ti->vecs[1][1] +
            p[2] * ti->vecs[1][2] + ti->vecs[1][3];

        /* Convert to luxel coordinates */
        ls = (int)((s - flm->s_offset) / 16.0f);
        lt = (int)((t - flm->t_offset) / 16.0f);

        if (ls < 0) ls = 0;
        if (lt < 0) lt = 0;
        if (ls >= flm->width) ls = flm->width - 1;
        if (lt >= flm->height) lt = flm->height - 1;

        /* Read lightmap data — assume RGB (3 bytes per luxel) */
        if (face->lightofs >= 0 &&
            face->lightofs + (lt * flm->width + ls) * 3 + 2 < lm_world->lightdata_size) {
            byte *sample = lm_world->lightdata + face->lightofs +
                           (lt * flm->width + ls) * 3;
            color[0] = sample[0] / 255.0f;
            color[1] = sample[1] / 255.0f;
            color[2] = sample[2] / 255.0f;

            /* Boost (Q2-style overbright x2, clamped) */
            color[0] = color[0] * 2.0f > 1.0f ? 1.0f : color[0] * 2.0f;
            color[1] = color[1] * 2.0f > 1.0f ? 1.0f : color[1] * 2.0f;
            color[2] = color[2] * 2.0f > 1.0f ? 1.0f : color[2] * 2.0f;
        }
    }
}

/* ==========================================================================
   Cleanup
   ========================================================================== */

void R_FreeLightmaps(void)
{
    int i;

    for (i = 0; i < lm_num_textures; i++) {
        if (lm_textures[i]) {
            qglDeleteTextures(1, &lm_textures[i]);
            lm_textures[i] = 0;
        }
    }
    lm_num_textures = 0;

    if (lm_faces) {
        Z_Free(lm_faces);
        lm_faces = NULL;
    }
    lm_num_faces = 0;
}
