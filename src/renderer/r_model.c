/*
 * r_model.c - MD2 (Quake II alias model) loading and rendering
 *
 * MD2 is id Software's triangle mesh format with keyframe vertex animation.
 * Used for weapon pickups, items, projectiles, and misc_model entities.
 *
 * File format (id Tech 2):
 *   - 68-byte header (magic, version, counts, offsets)
 *   - Skin names (64 bytes each)
 *   - Texture coordinates (short s, short t)
 *   - Triangles (3 vertex indices + 3 texcoord indices)
 *   - Frames (scale + translate + name + compressed vertices)
 *   - GL commands (triangle strips/fans for optimized rendering)
 *
 * Original SoF: ref_gl.dll loaded MD2s at 0x30xxxxxx
 */

#include "r_local.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* MD2 file format structures */
#define MD2_IDENT       (('2' << 24) + ('P' << 16) + ('D' << 8) + 'I')
#define MD2_VERSION     8

typedef struct {
    int     ident;
    int     version;
    int     skinwidth;
    int     skinheight;
    int     framesize;
    int     num_skins;
    int     num_xyz;        /* vertices per frame */
    int     num_st;         /* texture coords */
    int     num_tris;
    int     num_glcmds;
    int     num_frames;
    int     ofs_skins;
    int     ofs_st;
    int     ofs_tris;
    int     ofs_frames;
    int     ofs_glcmds;
    int     ofs_end;
} md2_header_t;

typedef struct {
    short   s, t;
} md2_stvert_t;

typedef struct {
    short   index_xyz[3];
    short   index_st[3];
} md2_triangle_t;

typedef struct {
    byte    v[3];           /* scaled vertex position */
    byte    lightnormalindex;
} md2_trivertx_t;

typedef struct {
    float           scale[3];
    float           translate[3];
    char            name[16];
    md2_trivertx_t  verts[1];   /* variable length */
} md2_frame_t;

/* ==========================================================================
   MD2 Pre-calculated normals (Quake II's 162 anorms)
   Only need these for basic lighting — use first 6 axial normals
   ========================================================================== */

static float r_avertexnormals[162][3] = {
    {-0.525731f,  0.000000f,  0.850651f},
    {-0.442863f,  0.238856f,  0.864188f},
    {-0.295242f,  0.000000f,  0.955423f},
    {-0.309017f,  0.500000f,  0.809017f},
    {-0.162460f,  0.262866f,  0.951056f},
    { 0.000000f,  0.000000f,  1.000000f},
    { 0.000000f,  0.850651f,  0.525731f},
    {-0.147621f,  0.716567f,  0.681718f},
    { 0.147621f,  0.716567f,  0.681718f},
    { 0.000000f,  0.525731f,  0.850651f},
    { 0.309017f,  0.500000f,  0.809017f},
    { 0.525731f,  0.000000f,  0.850651f},
    { 0.295242f,  0.000000f,  0.955423f},
    { 0.442863f,  0.238856f,  0.864188f},
    { 0.162460f,  0.262866f,  0.951056f},
    {-0.681718f,  0.147621f,  0.716567f},
    {-0.809017f,  0.309017f,  0.500000f},
    {-0.587785f,  0.425325f,  0.688191f},
    {-0.850651f,  0.525731f,  0.000000f},
    {-0.864188f,  0.442863f,  0.238856f},
    {-0.716567f,  0.681718f,  0.147621f},
    {-0.688191f,  0.587785f,  0.425325f},
    {-0.500000f,  0.809017f,  0.309017f},
    {-0.238856f,  0.864188f,  0.442863f},
    {-0.425325f,  0.688191f,  0.587785f},
    {-0.716567f,  0.681718f, -0.147621f},
    {-0.500000f,  0.809017f, -0.309017f},
    {-0.525731f,  0.850651f,  0.000000f},
    { 0.000000f,  0.850651f, -0.525731f},
    {-0.238856f,  0.864188f, -0.442863f},
    { 0.000000f,  0.955423f, -0.295242f},
    {-0.262866f,  0.951056f, -0.162460f},
    { 0.000000f,  1.000000f,  0.000000f},
    { 0.000000f,  0.955423f,  0.295242f},
    {-0.262866f,  0.951056f,  0.162460f},
    { 0.238856f,  0.864188f,  0.442863f},
    { 0.262866f,  0.951056f,  0.162460f},
    { 0.500000f,  0.809017f,  0.309017f},
    { 0.238856f,  0.864188f, -0.442863f},
    { 0.262866f,  0.951056f, -0.162460f},
    { 0.500000f,  0.809017f, -0.309017f},
    { 0.850651f,  0.525731f,  0.000000f},
    { 0.716567f,  0.681718f,  0.147621f},
    { 0.716567f,  0.681718f, -0.147621f},
    { 0.525731f,  0.850651f,  0.000000f},
    { 0.425325f,  0.688191f,  0.587785f},
    { 0.864188f,  0.442863f,  0.238856f},
    { 0.688191f,  0.587785f,  0.425325f},
    { 0.809017f,  0.309017f,  0.500000f},
    { 0.681718f,  0.147621f,  0.716567f},
    { 0.587785f,  0.425325f,  0.688191f},
    { 0.955423f,  0.295242f,  0.000000f},
    { 1.000000f,  0.000000f,  0.000000f},
    { 0.951056f,  0.162460f,  0.262866f},
    { 0.850651f, -0.525731f,  0.000000f},
    { 0.955423f, -0.295242f,  0.000000f},
    { 0.864188f, -0.442863f,  0.238856f},
    { 0.951056f, -0.162460f,  0.262866f},
    { 0.809017f, -0.309017f,  0.500000f},
    { 0.681718f, -0.147621f,  0.716567f},
    { 0.850651f,  0.000000f,  0.525731f},
    { 0.864188f,  0.442863f, -0.238856f},
    { 0.809017f,  0.309017f, -0.500000f},
    { 0.951056f,  0.162460f, -0.262866f},
    { 0.525731f,  0.000000f, -0.850651f},
    { 0.681718f,  0.147621f, -0.716567f},
    { 0.681718f, -0.147621f, -0.716567f},
    { 0.850651f,  0.000000f, -0.525731f},
    { 0.809017f, -0.309017f, -0.500000f},
    { 0.864188f, -0.442863f, -0.238856f},
    { 0.951056f, -0.162460f, -0.262866f},
    { 0.147621f,  0.716567f, -0.681718f},
    { 0.309017f,  0.500000f, -0.809017f},
    { 0.425325f,  0.688191f, -0.587785f},
    { 0.442863f,  0.238856f, -0.864188f},
    { 0.587785f,  0.425325f, -0.688191f},
    { 0.688191f,  0.587785f, -0.425325f},
    {-0.147621f,  0.716567f, -0.681718f},
    {-0.309017f,  0.500000f, -0.809017f},
    { 0.000000f,  0.525731f, -0.850651f},
    {-0.525731f,  0.000000f, -0.850651f},
    {-0.442863f,  0.238856f, -0.864188f},
    {-0.295242f,  0.000000f, -0.955423f},
    {-0.162460f,  0.262866f, -0.951056f},
    { 0.000000f,  0.000000f, -1.000000f},
    { 0.295242f,  0.000000f, -0.955423f},
    { 0.162460f,  0.262866f, -0.951056f},
    {-0.442863f, -0.238856f, -0.864188f},
    {-0.309017f, -0.500000f, -0.809017f},
    {-0.162460f, -0.262866f, -0.951056f},
    { 0.000000f, -0.850651f, -0.525731f},
    {-0.147621f, -0.716567f, -0.681718f},
    { 0.147621f, -0.716567f, -0.681718f},
    { 0.000000f, -0.525731f, -0.850651f},
    { 0.309017f, -0.500000f, -0.809017f},
    { 0.442863f, -0.238856f, -0.864188f},
    { 0.162460f, -0.262866f, -0.951056f},
    { 0.238856f, -0.864188f, -0.442863f},
    { 0.500000f, -0.809017f, -0.309017f},
    { 0.425325f, -0.688191f, -0.587785f},
    { 0.716567f, -0.681718f, -0.147621f},
    { 0.688191f, -0.587785f, -0.425325f},
    { 0.587785f, -0.425325f, -0.688191f},
    { 0.000000f, -0.955423f, -0.295242f},
    { 0.000000f, -1.000000f,  0.000000f},
    { 0.262866f, -0.951056f, -0.162460f},
    { 0.000000f, -0.850651f,  0.525731f},
    { 0.000000f, -0.955423f,  0.295242f},
    { 0.238856f, -0.864188f,  0.442863f},
    { 0.262866f, -0.951056f,  0.162460f},
    { 0.500000f, -0.809017f,  0.309017f},
    { 0.716567f, -0.681718f,  0.147621f},
    { 0.525731f, -0.850651f,  0.000000f},
    {-0.238856f, -0.864188f, -0.442863f},
    {-0.500000f, -0.809017f, -0.309017f},
    {-0.262866f, -0.951056f, -0.162460f},
    {-0.850651f, -0.525731f,  0.000000f},
    {-0.716567f, -0.681718f, -0.147621f},
    {-0.716567f, -0.681718f,  0.147621f},
    {-0.525731f, -0.850651f,  0.000000f},
    {-0.500000f, -0.809017f,  0.309017f},
    {-0.238856f, -0.864188f,  0.442863f},
    {-0.262866f, -0.951056f,  0.162460f},
    {-0.864188f, -0.442863f,  0.238856f},
    {-0.809017f, -0.309017f,  0.500000f},
    {-0.688191f, -0.587785f,  0.425325f},
    {-0.681718f, -0.147621f,  0.716567f},
    {-0.442863f, -0.238856f,  0.864188f},
    {-0.587785f, -0.425325f,  0.688191f},
    {-0.309017f, -0.500000f,  0.809017f},
    {-0.147621f, -0.716567f,  0.681718f},
    {-0.425325f, -0.688191f,  0.587785f},
    {-0.162460f, -0.262866f,  0.951056f},
    { 0.442863f, -0.238856f,  0.864188f},
    { 0.162460f, -0.262866f,  0.951056f},
    { 0.309017f, -0.500000f,  0.809017f},
    { 0.147621f, -0.716567f,  0.681718f},
    { 0.000000f, -0.525731f,  0.850651f},
    { 0.425325f, -0.688191f,  0.587785f},
    { 0.587785f, -0.425325f,  0.688191f},
    { 0.688191f, -0.587785f,  0.425325f},
    {-0.955423f,  0.295242f,  0.000000f},
    {-0.951056f,  0.162460f,  0.262866f},
    {-1.000000f,  0.000000f,  0.000000f},
    {-0.850651f,  0.000000f,  0.525731f},
    {-0.955423f, -0.295242f,  0.000000f},
    {-0.951056f, -0.162460f,  0.262866f},
    {-0.864188f,  0.442863f, -0.238856f},
    {-0.951056f,  0.162460f, -0.262866f},
    {-0.809017f,  0.309017f, -0.500000f},
    {-0.864188f, -0.442863f, -0.238856f},
    {-0.951056f, -0.162460f, -0.262866f},
    {-0.809017f, -0.309017f, -0.500000f},
    {-0.681718f,  0.147621f, -0.716567f},
    {-0.681718f, -0.147621f, -0.716567f},
    {-0.850651f,  0.000000f, -0.525731f},
    {-0.688191f,  0.587785f, -0.425325f},
    {-0.587785f,  0.425325f, -0.688191f},
    {-0.425325f,  0.688191f, -0.587785f},
    {-0.425325f, -0.688191f, -0.587785f},
    {-0.587785f, -0.425325f, -0.688191f},
    {-0.688191f, -0.587785f, -0.425325f},
};

/* ==========================================================================
   MD2 Loader
   ========================================================================== */

qboolean R_LoadMD2(model_t *mod, const char *name)
{
    byte *buf;
    int len;
    md2_header_t *hdr;
    md2_mesh_t *mesh;
    md2_triangle_t *tris;
    md2_stvert_t *stverts;
    int i, j;

    /* Load raw file */
    len = FS_LoadFile(name, (void **)&buf);
    if (!buf || len < (int)sizeof(md2_header_t)) {
        Com_DPrintf("R_LoadMD2: couldn't load %s\n", name);
        return qfalse;
    }

    hdr = (md2_header_t *)buf;

    /* Validate header */
    if (hdr->ident != MD2_IDENT || hdr->version != MD2_VERSION) {
        Com_Printf("R_LoadMD2: %s has wrong ident/version (%x/%d)\n",
                    name, hdr->ident, hdr->version);
        Z_Free(buf);
        return qfalse;
    }

    /* Sanity checks */
    if (hdr->num_xyz <= 0 || hdr->num_tris <= 0 || hdr->num_frames <= 0 ||
        hdr->num_xyz > 4096 || hdr->num_tris > 8192 || hdr->num_frames > 512) {
        Com_Printf("R_LoadMD2: %s has bad counts (verts=%d tris=%d frames=%d)\n",
                    name, hdr->num_xyz, hdr->num_tris, hdr->num_frames);
        Z_Free(buf);
        return qfalse;
    }

    /* Allocate mesh structure */
    mesh = (md2_mesh_t *)Z_Malloc(sizeof(md2_mesh_t));
    memset(mesh, 0, sizeof(*mesh));

    mesh->num_verts = hdr->num_xyz;
    mesh->num_tris = hdr->num_tris;
    mesh->num_frames = hdr->num_frames;
    mesh->num_skins = hdr->num_skins;
    mesh->skinwidth = hdr->skinwidth;
    mesh->skinheight = hdr->skinheight;

    /* Allocate triangle indices */
    mesh->tris = (int *)Z_Malloc(mesh->num_tris * 3 * sizeof(int));

    /* Allocate texture coordinates (per vertex) */
    mesh->texcoords = (float *)Z_Malloc(mesh->num_verts * 2 * sizeof(float));

    /* Allocate frame vertex data */
    mesh->frames = (float *)Z_Malloc(mesh->num_frames * mesh->num_verts * 3 * sizeof(float));
    mesh->frame_names = (char (*)[16])Z_Malloc(mesh->num_frames * 16);

    /* Read triangles */
    tris = (md2_triangle_t *)(buf + hdr->ofs_tris);
    for (i = 0; i < mesh->num_tris; i++) {
        mesh->tris[i * 3 + 0] = tris[i].index_xyz[0];
        mesh->tris[i * 3 + 1] = tris[i].index_xyz[1];
        mesh->tris[i * 3 + 2] = tris[i].index_xyz[2];
    }

    /* Read texture coordinates and map to first triangle reference */
    stverts = (md2_stvert_t *)(buf + hdr->ofs_st);
    {
        /* Build a texcoord lookup: for each vertex, find which st index
         * it maps to via triangles. Use first occurrence. */
        float *tc_lookup = (float *)Z_Malloc(hdr->num_st * 2 * sizeof(float));
        float inv_w = 1.0f / (float)hdr->skinwidth;
        float inv_h = 1.0f / (float)hdr->skinheight;

        for (i = 0; i < hdr->num_st; i++) {
            tc_lookup[i * 2 + 0] = (float)stverts[i].s * inv_w;
            tc_lookup[i * 2 + 1] = (float)stverts[i].t * inv_h;
        }

        /* Map texcoords to vertex indices via triangles */
        memset(mesh->texcoords, 0, mesh->num_verts * 2 * sizeof(float));
        for (i = 0; i < mesh->num_tris; i++) {
            for (j = 0; j < 3; j++) {
                int vi = tris[i].index_xyz[j];
                int si = tris[i].index_st[j];
                if (si >= 0 && si < hdr->num_st) {
                    mesh->texcoords[vi * 2 + 0] = tc_lookup[si * 2 + 0];
                    mesh->texcoords[vi * 2 + 1] = tc_lookup[si * 2 + 1];
                }
            }
        }

        Z_Free(tc_lookup);
    }

    /* Read frames — decompress vertex data */
    for (i = 0; i < mesh->num_frames; i++) {
        md2_frame_t *frame = (md2_frame_t *)(buf + hdr->ofs_frames + i * hdr->framesize);
        float *out = mesh->frames + i * mesh->num_verts * 3;

        memcpy(mesh->frame_names[i], frame->name, 16);
        mesh->frame_names[i][15] = 0;

        for (j = 0; j < mesh->num_verts; j++) {
            out[j * 3 + 0] = frame->verts[j].v[0] * frame->scale[0] + frame->translate[0];
            out[j * 3 + 1] = frame->verts[j].v[1] * frame->scale[1] + frame->translate[1];
            out[j * 3 + 2] = frame->verts[j].v[2] * frame->scale[2] + frame->translate[2];
        }

        /* Compute bounding box from first frame */
        if (i == 0) {
            VectorCopy(&out[0], mesh->mins);
            VectorCopy(&out[0], mesh->maxs);
            for (j = 1; j < mesh->num_verts; j++) {
                if (out[j*3+0] < mesh->mins[0]) mesh->mins[0] = out[j*3+0];
                if (out[j*3+1] < mesh->mins[1]) mesh->mins[1] = out[j*3+1];
                if (out[j*3+2] < mesh->mins[2]) mesh->mins[2] = out[j*3+2];
                if (out[j*3+0] > mesh->maxs[0]) mesh->maxs[0] = out[j*3+0];
                if (out[j*3+1] > mesh->maxs[1]) mesh->maxs[1] = out[j*3+1];
                if (out[j*3+2] > mesh->maxs[2]) mesh->maxs[2] = out[j*3+2];
            }
        }
    }

    /* Load skin textures if available */
    if (hdr->num_skins > 0) {
        char *skin_names = (char *)(buf + hdr->ofs_skins);
        int max_skins = hdr->num_skins > 4 ? 4 : hdr->num_skins;

        for (i = 0; i < max_skins; i++) {
            char *sname = skin_names + i * 64;
            if (sname[0]) {
                mod->skins[i] = R_FindImage(sname);
                if (mod->skins[i])
                    mod->num_skins = i + 1;
            }
        }
    }

    mod->md2 = mesh;
    Z_Free(buf);

    Com_DPrintf("MD2: %s — %d tris, %d verts, %d frames\n",
                name, mesh->num_tris, mesh->num_verts, mesh->num_frames);

    return qtrue;
}

void R_FreeMD2(model_t *mod)
{
    if (mod->md2) {
        if (mod->md2->tris) Z_Free(mod->md2->tris);
        if (mod->md2->texcoords) Z_Free(mod->md2->texcoords);
        if (mod->md2->frames) Z_Free(mod->md2->frames);
        if (mod->md2->frame_names) Z_Free(mod->md2->frame_names);
        Z_Free(mod->md2);
        mod->md2 = NULL;
    }
}

/* ==========================================================================
   MD2 Renderer
   ========================================================================== */

/*
 * R_DrawAliasModel - Render an MD2 model at a world position
 *
 * Supports frame interpolation (backlerp between oldframe and frame).
 * Uses basic directional lighting with ambient.
 */
void R_DrawAliasModel(model_t *mod, vec3_t origin, vec3_t angles,
                      int frame, int oldframe, float backlerp,
                      float r, float g, float b)
{
    md2_mesh_t *mesh;
    float *cur_verts, *old_verts;
    float frontlerp;
    int i;

    if (!mod || !mod->md2)
        return;

    mesh = mod->md2;

    /* Clamp frame indices */
    if (frame < 0 || frame >= mesh->num_frames) frame = 0;
    if (oldframe < 0 || oldframe >= mesh->num_frames) oldframe = 0;

    cur_verts = mesh->frames + frame * mesh->num_verts * 3;
    old_verts = mesh->frames + oldframe * mesh->num_verts * 3;
    frontlerp = 1.0f - backlerp;

    /* Set up model transform */
    qglPushMatrix();

    /* Translate to world position */
    qglTranslatef(origin[0], origin[1], origin[2]);

    /* Apply rotation (Q2 convention: YAW=angles[1], PITCH=angles[0]) */
    if (angles[1]) qglRotatef(angles[1], 0, 0, 1);
    if (angles[0]) qglRotatef(-angles[0], 0, 1, 0);
    if (angles[2]) qglRotatef(angles[2], 1, 0, 0);

    /* Bind skin texture if available */
    if (mod->num_skins > 0 && mod->skins[0]) {
        qglEnable(GL_TEXTURE_2D);
        qglBindTexture(GL_TEXTURE_2D, mod->skins[0]->texnum);
    } else {
        qglDisable(GL_TEXTURE_2D);
    }

    /* Draw triangles with vertex interpolation */
    qglBegin(GL_TRIANGLES);
    qglColor4f(r, g, b, 1.0f);

    for (i = 0; i < mesh->num_tris; i++) {
        int j;
        for (j = 0; j < 3; j++) {
            int vi = mesh->tris[i * 3 + j];
            float vx, vy, vz;

            if (vi < 0 || vi >= mesh->num_verts)
                continue;

            /* Interpolate vertex position */
            vx = old_verts[vi*3+0] * backlerp + cur_verts[vi*3+0] * frontlerp;
            vy = old_verts[vi*3+1] * backlerp + cur_verts[vi*3+1] * frontlerp;
            vz = old_verts[vi*3+2] * backlerp + cur_verts[vi*3+2] * frontlerp;

            /* Texture coordinates */
            if (mod->num_skins > 0 && mod->skins[0]) {
                qglTexCoord2f(mesh->texcoords[vi*2+0],
                              mesh->texcoords[vi*2+1]);
            }

            /* Simple height-based shading */
            {
                float shade = 0.6f + 0.4f * (vz - mesh->mins[2]) /
                              (mesh->maxs[2] - mesh->mins[2] + 0.01f);
                qglColor4f(r * shade, g * shade, b * shade, 1.0f);
            }

            qglVertex3f(vx, vy, vz);
        }
    }

    qglEnd();

    /* Restore state */
    if (mod->num_skins == 0 || !mod->skins[0])
        qglEnable(GL_TEXTURE_2D);
    qglColor4f(1, 1, 1, 1);

    qglPopMatrix();
}
