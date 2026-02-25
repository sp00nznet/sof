/*
 * r_surf.c - BSP surface rendering
 *
 * Renders loaded BSP world geometry using OpenGL immediate mode.
 * Converts BSP face→surfedge→edge→vertex chains into GL polygons.
 *
 * BSP Face Rendering Pipeline:
 *   1. For each visible face:
 *      a. Get face's surfedge list (face->firstedge, face->numedges)
 *      b. Each surfedge is an index into the edges array (negative = reversed)
 *      c. Each edge has two vertex indices
 *      d. Emit vertices as a GL triangle fan
 *   2. Texture coordinates computed from texinfo vecs[2][4]
 *
 * Original ref_gl.dll: R_DrawBrushModel at 0x3000BC00
 */

#include "r_local.h"
#include "r_bsp.h"

/* ==========================================================================
   Current Map State
   ========================================================================== */

static bsp_world_t  r_worldmodel;
static qboolean     r_worldloaded = qfalse;

/* Camera state */
static vec3_t   r_camera_origin;
static vec3_t   r_camera_angles;    /* pitch, yaw, roll */
static float    r_camera_speed = 400.0f;

/* Stats */
static int  c_brush_polys;
static int  c_visible_faces;

/* Per-texinfo cached texture lookups (populated on map load) */
#define MAX_TEXINFO_CACHE   4096
static image_t  *r_texinfo_images[MAX_TEXINFO_CACHE];

/* ==========================================================================
   Map Loading
   ========================================================================== */

void R_LoadWorldMap(const char *name)
{
    char fullname[MAX_QPATH];

    /* Build full path */
    Com_sprintf(fullname, sizeof(fullname), "maps/%s.bsp", name);

    /* Free previous map */
    if (r_worldloaded) {
        R_FreeLightmaps();
        BSP_Free(&r_worldmodel);
        r_worldloaded = qfalse;
    }

    Com_Printf("Loading map: %s\n", fullname);

    if (!BSP_Load(fullname, &r_worldmodel)) {
        Com_Printf("R_LoadWorldMap: couldn't load %s\n", fullname);
        return;
    }

    r_worldloaded = qtrue;

    /* Load textures for all texinfo entries */
    {
        int ti;
        int loaded = 0, missed = 0;

        memset(r_texinfo_images, 0, sizeof(r_texinfo_images));
        R_ImageBeginRegistration();

        for (ti = 0; ti < r_worldmodel.num_texinfo && ti < MAX_TEXINFO_CACHE; ti++) {
            const char *texname = r_worldmodel.texinfo[ti].texture;
            if (texname[0]) {
                r_texinfo_images[ti] = R_FindImage(texname);
                if (r_texinfo_images[ti])
                    loaded++;
                else
                    missed++;
            }
        }

        R_ImageEndRegistration();
        Com_Printf("Textures: %d loaded, %d missing (of %d texinfo)\n",
                   loaded, missed, r_worldmodel.num_texinfo);
    }

    /* Build lightmap atlas textures */
    R_BuildLightmaps(&r_worldmodel);

    /* Reset camera to origin */
    VectorClear(r_camera_origin);
    VectorClear(r_camera_angles);

    /* Try to find a player start from entity string */
    if (r_worldmodel.entity_string) {
        const char *p = r_worldmodel.entity_string;
        /* Simple scan for info_player_start origin */
        while (*p) {
            if (strstr(p, "info_player_start")) {
                /* Look for origin in this entity block */
                const char *orig = strstr(p, "\"origin\"");
                if (orig) {
                    orig = strchr(orig + 8, '"');
                    if (orig) {
                        orig++;
                        sscanf(orig, "%f %f %f",
                               &r_camera_origin[0],
                               &r_camera_origin[1],
                               &r_camera_origin[2]);
                        r_camera_origin[2] += 56;  /* eye height */
                        Com_Printf("Camera at player start: %.0f %.0f %.0f\n",
                                   r_camera_origin[0], r_camera_origin[1],
                                   r_camera_origin[2]);
                    }
                }
                break;
            }
            p++;
        }
    }
}

qboolean R_WorldLoaded(void)
{
    return r_worldloaded;
}

bsp_world_t *R_GetWorldModel(void)
{
    return r_worldloaded ? &r_worldmodel : NULL;
}

/* ==========================================================================
   Camera Control (Freecam / Noclip)
   ========================================================================== */

void R_GetCameraOrigin(vec3_t out)
{
    VectorCopy(r_camera_origin, out);
}

void R_SetCameraOrigin(vec3_t origin)
{
    VectorCopy(origin, r_camera_origin);
}

void R_GetCameraAngles(vec3_t out)
{
    VectorCopy(r_camera_angles, out);
}

void R_SetCameraAngles(vec3_t angles)
{
    VectorCopy(angles, r_camera_angles);
}

/*
 * Update camera from input (called each frame)
 * forward/right/up in [-1, 1] range, mouse_dx/dy for look
 */
void R_UpdateCamera(float forward, float right, float up,
                    float mouse_dx, float mouse_dy, float frametime)
{
    float   yaw_rad, pitch_rad;
    float   speed;
    vec3_t  fwd, rt;

    /* Mouse look */
    r_camera_angles[1] -= mouse_dx * 0.15f;    /* yaw (left/right) */
    r_camera_angles[0] += mouse_dy * 0.15f;    /* pitch (up/down) */

    /* Clamp pitch */
    if (r_camera_angles[0] > 89) r_camera_angles[0] = 89;
    if (r_camera_angles[0] < -89) r_camera_angles[0] = -89;

    /* Build direction vectors from angles */
    yaw_rad = r_camera_angles[1] * (float)(3.14159265 / 180.0);
    pitch_rad = r_camera_angles[0] * (float)(3.14159265 / 180.0);

    fwd[0] = (float)(cos(yaw_rad) * cos(pitch_rad));
    fwd[1] = (float)(sin(yaw_rad) * cos(pitch_rad));
    fwd[2] = (float)(-sin(pitch_rad));

    rt[0] = (float)sin(yaw_rad);
    rt[1] = (float)(-cos(yaw_rad));
    rt[2] = 0;

    speed = r_camera_speed * frametime;

    /* Move */
    r_camera_origin[0] += (fwd[0] * forward + rt[0] * right) * speed;
    r_camera_origin[1] += (fwd[1] * forward + rt[1] * right) * speed;
    r_camera_origin[2] += (fwd[2] * forward + up) * speed;
}

/* ==========================================================================
   Surface Rendering
   ========================================================================== */

/*
 * Draw a single BSP face as a triangle fan
 * face_idx is needed for lightmap TC lookup
 */
static void R_DrawFace(bsp_world_t *world, bsp_face_t *face,
                        int face_idx, qboolean use_lightmap)
{
    int         i;
    int         edge_idx;
    bsp_edge_t  *edge;
    float       *v;
    bsp_texinfo_t *ti = NULL;

    if (face->numedges < 3)
        return;

    if (face->texinfo >= 0 && face->texinfo < world->num_texinfo)
        ti = &world->texinfo[face->texinfo];

    qglBegin(GL_TRIANGLE_FAN);

    for (i = 0; i < face->numedges; i++) {
        edge_idx = world->surfedges[face->firstedge + i];

        if (edge_idx >= 0) {
            edge = &world->edges[edge_idx];
            v = world->vertexes[edge->v[0]].point;
        } else {
            edge = &world->edges[-edge_idx];
            v = world->vertexes[edge->v[1]].point;
        }

        /* Compute texture coordinates from texinfo */
        if (ti) {
            float s = DotProduct(v, ti->vecs[0]) + ti->vecs[0][3];
            float t = DotProduct(v, ti->vecs[1]) + ti->vecs[1][3];
            /* Normalize by actual texture dimensions if loaded */
            image_t *img = (face->texinfo < MAX_TEXINFO_CACHE) ?
                            r_texinfo_images[face->texinfo] : NULL;
            if (img && img->width > 0 && img->height > 0) {
                s /= (float)img->width;
                t /= (float)img->height;
            } else {
                s /= 64.0f;
                t /= 64.0f;
            }
            qglTexCoord2f(s, t);

            /* Lightmap TC on TMU1 */
            if (use_lightmap && qglActiveTextureARB) {
                float lm_s, lm_t;
                GLuint lm_tex;
                if (R_GetFaceLightmapTC(face_idx, v, ti,
                                         &lm_s, &lm_t, &lm_tex)) {
                    qglActiveTextureARB(GL_TEXTURE1_ARB);
                    qglTexCoord2f(lm_s, lm_t);
                    qglActiveTextureARB(GL_TEXTURE0_ARB);
                }
            }
        }

        qglVertex3f(v[0], v[1], v[2]);
    }

    qglEnd();
    c_brush_polys++;
}

/*
 * Determine face color from texinfo (for untextured wireframe/flat rendering)
 */
static void R_SetFaceColor(bsp_world_t *world, bsp_face_t *face)
{
    if (face->texinfo >= 0 && face->texinfo < world->num_texinfo) {
        bsp_texinfo_t *ti = &world->texinfo[face->texinfo];

        /* Color by surface type */
        if (ti->flags & SURF_SKY) {
            qglColor4f(0.3f, 0.3f, 0.8f, 1.0f);   /* sky = blue */
        } else if (ti->flags & SURF_WARP) {
            qglColor4f(0.2f, 0.5f, 0.8f, 1.0f);   /* water = cyan */
        } else if (ti->flags & SURF_TRANS33) {
            qglColor4f(0.6f, 0.6f, 0.6f, 0.33f);  /* transparent */
        } else if (ti->flags & SURF_TRANS66) {
            qglColor4f(0.6f, 0.6f, 0.6f, 0.66f);  /* semi-transparent */
        } else if (ti->flags & SURF_NODRAW) {
            return;  /* skip invisible surfaces */
        } else {
            /* Hash texture name for a consistent pseudorandom color */
            unsigned hash = 0;
            const char *n = ti->texture;
            while (*n) { hash = hash * 31 + (unsigned char)*n++; }
            float r = 0.3f + (float)((hash >> 0) & 0xFF) / 512.0f;
            float g = 0.3f + (float)((hash >> 8) & 0xFF) / 512.0f;
            float b = 0.3f + (float)((hash >> 16) & 0xFF) / 512.0f;
            qglColor4f(r, g, b, 1.0f);
        }
    } else {
        qglColor4f(0.5f, 0.5f, 0.5f, 1.0f);
    }
}

/* ==========================================================================
   World Rendering
   ========================================================================== */

/*
 * R_DrawSingleFace - Draw one face with texture and lightmap binding
 */
static void R_DrawSingleFace(bsp_world_t *world, int face_idx)
{
    bsp_face_t *face = &world->faces[face_idx];
    image_t *img = NULL;
    qboolean use_lm = qfalse;
    GLuint lm_tex;

    /* Skip faces with NODRAW/SKY flags */
    if (face->texinfo >= 0 && face->texinfo < world->num_texinfo) {
        if (world->texinfo[face->texinfo].flags & SURF_NODRAW)
            return;
        if (world->texinfo[face->texinfo].flags & SURF_SKY)
            return;

        /* Look up cached texture */
        if (face->texinfo < MAX_TEXINFO_CACHE)
            img = r_texinfo_images[face->texinfo];
    }

    if (img && img->texnum) {
        qglEnable(GL_TEXTURE_2D);
        qglBindTexture(GL_TEXTURE_2D, img->texnum);
        if (img->has_alpha) {
            qglEnable(GL_BLEND);
            qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        qglColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    } else {
        qglDisable(GL_TEXTURE_2D);
        qglBindTexture(GL_TEXTURE_2D, R_GetNoTexture());
        R_SetFaceColor(world, face);
    }

    /* Set up lightmap on TMU1 if available */
    lm_tex = R_GetFaceLightmapTexture(face_idx);
    if (lm_tex && gl_state.have_multitexture && qglActiveTextureARB) {
        qglActiveTextureARB(GL_TEXTURE1_ARB);
        qglEnable(GL_TEXTURE_2D);
        qglBindTexture(GL_TEXTURE_2D, lm_tex);
        qglTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        qglActiveTextureARB(GL_TEXTURE0_ARB);
        use_lm = qtrue;
    }

    R_DrawFace(world, face, face_idx, use_lm);

    if (use_lm) {
        qglActiveTextureARB(GL_TEXTURE1_ARB);
        qglDisable(GL_TEXTURE_2D);
        qglActiveTextureARB(GL_TEXTURE0_ARB);
    }

    if (img && img->has_alpha)
        qglDisable(GL_BLEND);

    c_visible_faces++;
}

/*
 * R_DrawWorld - Render BSP world with PVS culling
 *
 * Uses the Potentially Visible Set to only render faces in leafs
 * visible from the camera's current leaf/cluster. Falls back to
 * rendering all faces if PVS data is unavailable.
 */
void R_DrawWorld(void)
{
    int i;
    bsp_world_t *world = &r_worldmodel;

    if (!r_worldloaded)
        return;

    c_brush_polys = 0;
    c_visible_faces = 0;

    /* Set up for world rendering */
    qglEnable(GL_DEPTH_TEST);
    qglDepthFunc(GL_LEQUAL);
    qglDepthMask(GL_TRUE);
    qglEnable(GL_CULL_FACE);
    qglEnable(GL_TEXTURE_2D);

    /* PVS culling: find camera leaf/cluster */
    if (world->vis && world->leafs && world->leaffaces &&
        world->num_leafs > 0 && !(r_novis && r_novis->value)) {
        int cam_leaf = BSP_PointLeaf(world, r_camera_origin);
        int cam_cluster = -1;
        byte *face_drawn;  /* prevent drawing same face twice */

        if (cam_leaf >= 0 && cam_leaf < world->num_leafs)
            cam_cluster = world->leafs[cam_leaf].cluster;

        /* Bitfield to track already-drawn faces */
        face_drawn = (byte *)Z_Malloc((world->num_faces + 7) / 8);
        memset(face_drawn, 0, (world->num_faces + 7) / 8);

        /* Walk all leafs, check PVS visibility */
        for (i = 0; i < world->num_leafs; i++) {
            bsp_leaf_t *leaf = &world->leafs[i];
            int j;

            /* Skip if not visible from camera cluster */
            if (cam_cluster >= 0 && leaf->cluster >= 0) {
                if (!BSP_ClusterVisible(world, cam_cluster, leaf->cluster))
                    continue;
            }

            /* Render all faces in this leaf */
            for (j = 0; j < leaf->numleaffaces; j++) {
                int lf_idx = leaf->firstleafface + j;
                int face_idx;

                if (lf_idx >= world->num_leaffaces)
                    break;

                face_idx = world->leaffaces[lf_idx];
                if (face_idx < 0 || face_idx >= world->num_faces)
                    continue;

                /* Skip already-drawn faces (shared between leafs) */
                if (face_drawn[face_idx >> 3] & (1 << (face_idx & 7)))
                    continue;
                face_drawn[face_idx >> 3] |= (1 << (face_idx & 7));

                R_DrawSingleFace(world, face_idx);
            }
        }

        Z_Free(face_drawn);
    } else {
        /* No PVS — draw all faces (fallback) */
        for (i = 0; i < world->num_faces; i++) {
            R_DrawSingleFace(world, i);
        }
    }

    qglDisable(GL_CULL_FACE);
}

/*
 * R_SetupProjection - Set up perspective projection matrix
 */
static void R_SetupProjection(float fov_x, float fov_y, float znear, float zfar)
{
    float xmin, xmax, ymin, ymax;

    ymax = znear * (float)tan(fov_y * 3.14159265 / 360.0);
    ymin = -ymax;

    xmax = znear * (float)tan(fov_x * 3.14159265 / 360.0);
    xmin = -xmax;

    qglMatrixMode(GL_PROJECTION);
    qglLoadIdentity();
    qglFrustum(xmin, xmax, ymin, ymax, znear, zfar);
}

/*
 * R_SetupCamera - Set up modelview matrix from camera state
 *
 * Q2/SoF coordinate system:
 *   +X = forward, +Y = left, +Z = up
 * OpenGL coordinate system:
 *   -Z = forward, +X = right, +Y = up
 *
 * We apply a basis change rotation then the camera transform.
 */
static void R_SetupCamera(void)
{
    qglMatrixMode(GL_MODELVIEW);
    qglLoadIdentity();

    /* Pitch (around right axis / Y in GL) */
    qglRotatef(-90, 1, 0, 0);          /* Q2: Z-up → GL: Y-up */
    qglRotatef(90, 0, 0, 1);           /* Q2: X-forward → GL: -Z forward */

    /* Apply camera rotation */
    qglRotatef(-r_camera_angles[2], 1, 0, 0);   /* roll */
    qglRotatef(-r_camera_angles[0], 0, 1, 0);   /* pitch */
    qglRotatef(-r_camera_angles[1], 0, 0, 1);   /* yaw */

    /* Translate to camera position (inverted) */
    qglTranslatef(-r_camera_origin[0], -r_camera_origin[1], -r_camera_origin[2]);
}

/*
 * R_RenderWorldView - Full 3D world render pass
 *
 * Called from the main render frame when a map is loaded.
 */
void R_RenderWorldView(void)
{
    if (!r_worldloaded)
        return;

    /* Set up 3D projection */
    R_SetupProjection(90.0f, 73.74f, 4.0f, 8192.0f);
    R_SetupCamera();

    /* Clear depth buffer */
    qglClear(GL_DEPTH_BUFFER_BIT);

    /* Render world geometry */
    R_DrawWorld();
}

/* ==========================================================================
   Map Console Command
   ========================================================================== */

/* Forward declaration — engine calls game spawn after map load */
extern void SV_SpawnMapEntities(const char *mapname, const char *entstring);

static void Cmd_Map_f(void)
{
    const char *mapname;

    if (Cmd_Argc() < 2) {
        Com_Printf("Usage: map <mapname>\n");
        return;
    }

    mapname = Cmd_Argv(1);
    R_LoadWorldMap(mapname);

    /* Spawn entities from BSP */
    if (r_worldloaded && r_worldmodel.entity_string) {
        SV_SpawnMapEntities(mapname, r_worldmodel.entity_string);
    }
}

static void Cmd_Maplist_f(void)
{
    Com_Printf("Known SoF SP maps:\n");
    Com_Printf("  sof1sp1  - Gold: New York Streets\n");
    Com_Printf("  sof1sp2  - Gold: Subway Station\n");
    Com_Printf("  sof1sp3  - Gold: Meat Packing Plant\n");
    Com_Printf("  sof2sp1  - Silver: Iraqi Facility\n");
    Com_Printf("  sof3sp1  - Bronze: Siberian Outpost\n");
    Com_Printf("  sof4sp1  - Iron: Sudan Village\n");
    Com_Printf("  sof5sp1  - Lead: Japan Compound\n");
    Com_Printf("  sofend   - Final: Kosovo Nuke Base\n");
    Com_Printf("\nKnown MP maps:\n");
    Com_Printf("  dm_packing  dm_depot  dm_baths\n");
    Com_Printf("  dm_train    dm_yard   dm_subway\n");
    Com_Printf("\nUsage: map <name>\n");
}

static void Cmd_Campos_f(void)
{
    if (Cmd_Argc() >= 4) {
        r_camera_origin[0] = (float)atof(Cmd_Argv(1));
        r_camera_origin[1] = (float)atof(Cmd_Argv(2));
        r_camera_origin[2] = (float)atof(Cmd_Argv(3));
        Com_Printf("Camera: %.0f %.0f %.0f\n",
                   r_camera_origin[0], r_camera_origin[1], r_camera_origin[2]);
    } else {
        Com_Printf("Camera: %.0f %.0f %.0f  Angles: %.0f %.0f %.0f\n",
                   r_camera_origin[0], r_camera_origin[1], r_camera_origin[2],
                   r_camera_angles[0], r_camera_angles[1], r_camera_angles[2]);
    }
}

void R_InitSurfCommands(void)
{
    Cmd_AddCommand("map", Cmd_Map_f);
    Cmd_AddCommand("maplist", Cmd_Maplist_f);
    Cmd_AddCommand("campos", Cmd_Campos_f);
}
