/*
 * r_main.c - Renderer initialization and main rendering pipeline
 *
 * Based on Quake II ref_gl (id Software GPL) with SoF extensions.
 * Handles GL context creation, extension detection, and frame rendering.
 *
 * Original ref_gl.dll was 225KB with single GetRefAPI export.
 * refexport_t has 54 fields (vs Q2's 21), refimport_t has 27 fields.
 *
 * SoF additions to renderer:
 *   - GHOUL model rendering (DrawModel)
 *   - M32 texture format (32-bit mipmapped, replacing .wal)
 *   - Particle/gore effects
 *   - Decal system
 *   - SetGamma, SetViewport, Scissor, Ortho
 */

#include "r_local.h"
#include "../game/g_local.h"

/* ==========================================================================
   GL State
   ========================================================================== */

glstate_t   gl_state;
glconfig_t  gl_config;

/* ==========================================================================
   Renderer Cvars
   ========================================================================== */

cvar_t  *r_mode;
cvar_t  *r_fullscreen;
cvar_t  *r_drawworld;
cvar_t  *r_drawentities;
cvar_t  *r_speeds;
cvar_t  *r_novis;
cvar_t  *r_nocull;
cvar_t  *gl_texturemode;
cvar_t  *gl_modulate;
cvar_t  *vid_gamma;

/* SoF-specific */
cvar_t  *ghl_specular;
cvar_t  *ghl_mip;

/* ==========================================================================
   Skybox State
   ========================================================================== */

static GLuint   sky_textures[6];    /* rt, lf, up, dn, ft, bk */
static qboolean sky_loaded;
static float    sky_rotate;
static vec3_t   sky_axis;

/* ==========================================================================
   Dynamic Lights
   ========================================================================== */

typedef struct {
    vec3_t  origin;
    vec3_t  color;
    float   intensity;
    float   die;        /* time when light expires (from Sys_Milliseconds) */
} r_dlight_t;

static r_dlight_t   r_dlights[MAX_DLIGHTS];
static int          r_num_dlights;

/* ==========================================================================
   GL Function Pointers
   ========================================================================== */

/* Core GL 1.1 functions */
void (APIENTRY *qglBegin)(GLenum mode);
void (APIENTRY *qglEnd)(void);
void (APIENTRY *qglVertex3f)(GLfloat x, GLfloat y, GLfloat z);
void (APIENTRY *qglTexCoord2f)(GLfloat s, GLfloat t);
void (APIENTRY *qglColor4f)(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void (APIENTRY *qglNormal3f)(GLfloat nx, GLfloat ny, GLfloat nz);

void (APIENTRY *qglClear)(GLbitfield mask);
void (APIENTRY *qglClearColor)(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void (APIENTRY *qglEnable)(GLenum cap);
void (APIENTRY *qglDisable)(GLenum cap);
void (APIENTRY *qglBlendFunc)(GLenum sfactor, GLenum dfactor);
void (APIENTRY *qglDepthFunc)(GLenum func);
void (APIENTRY *qglDepthMask)(GLboolean flag);
void (APIENTRY *qglViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
void (APIENTRY *qglScissor)(GLint x, GLint y, GLsizei width, GLsizei height);
void (APIENTRY *qglMatrixMode)(GLenum mode);
void (APIENTRY *qglLoadIdentity)(void);
void (APIENTRY *qglOrtho)(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void (APIENTRY *qglFrustum)(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void (APIENTRY *qglPushMatrix)(void);
void (APIENTRY *qglPopMatrix)(void);
void (APIENTRY *qglRotatef)(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void (APIENTRY *qglTranslatef)(GLfloat x, GLfloat y, GLfloat z);

void (APIENTRY *qglBindTexture)(GLenum target, GLuint texture);
void (APIENTRY *qglGenTextures)(GLsizei n, GLuint *textures);
void (APIENTRY *qglDeleteTextures)(GLsizei n, const GLuint *textures);
void (APIENTRY *qglTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
void (APIENTRY *qglTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
void (APIENTRY *qglTexParameteri)(GLenum target, GLenum pname, GLint param);
void (APIENTRY *qglTexEnvi)(GLenum target, GLenum pname, GLint param);

void (APIENTRY *qglDrawArrays)(GLenum mode, GLint first, GLsizei count);
void (APIENTRY *qglDrawElements)(GLenum mode, GLsizei count, GLenum type, const void *indices);
void (APIENTRY *qglVertexPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
void (APIENTRY *qglTexCoordPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
void (APIENTRY *qglColorPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
void (APIENTRY *qglEnableClientState)(GLenum cap);
void (APIENTRY *qglDisableClientState)(GLenum cap);

GLenum (APIENTRY *qglGetError)(void);
const GLubyte *(APIENTRY *qglGetString)(GLenum name);
void (APIENTRY *qglGetIntegerv)(GLenum pname, GLint *params);
void (APIENTRY *qglGetFloatv)(GLenum pname, GLfloat *params);
void (APIENTRY *qglFinish)(void);
void (APIENTRY *qglFlush)(void);
void (APIENTRY *qglPointSize)(GLfloat size);

/* Extensions */
PFNGLACTIVETEXTUREARBPROC       qglActiveTextureARB;
PFNGLCLIENTACTIVETEXTUREARBPROC qglClientActiveTextureARB;

/* ==========================================================================
   QGL_Init — Load all GL function pointers via SDL
   ========================================================================== */

#define QGL_LOAD(name) \
    qgl##name = (void *)Sys_GL_GetProcAddress("gl" #name); \
    if (!qgl##name) { Com_Printf("WARNING: gl" #name " not found\n"); }

qboolean QGL_Init(void)
{
    /* Core GL 1.1 */
    QGL_LOAD(Begin);
    QGL_LOAD(End);
    QGL_LOAD(Vertex3f);
    QGL_LOAD(TexCoord2f);
    QGL_LOAD(Color4f);
    QGL_LOAD(Normal3f);
    QGL_LOAD(Clear);
    QGL_LOAD(ClearColor);
    QGL_LOAD(Enable);
    QGL_LOAD(Disable);
    QGL_LOAD(BlendFunc);
    QGL_LOAD(DepthFunc);
    QGL_LOAD(DepthMask);
    QGL_LOAD(Viewport);
    QGL_LOAD(Scissor);
    QGL_LOAD(MatrixMode);
    QGL_LOAD(LoadIdentity);
    QGL_LOAD(Ortho);
    QGL_LOAD(Frustum);
    QGL_LOAD(PushMatrix);
    QGL_LOAD(PopMatrix);
    QGL_LOAD(Rotatef);
    QGL_LOAD(Translatef);
    QGL_LOAD(BindTexture);
    QGL_LOAD(GenTextures);
    QGL_LOAD(DeleteTextures);
    QGL_LOAD(TexImage2D);
    QGL_LOAD(TexSubImage2D);
    QGL_LOAD(TexParameteri);
    QGL_LOAD(TexEnvi);
    QGL_LOAD(DrawArrays);
    QGL_LOAD(DrawElements);
    QGL_LOAD(VertexPointer);
    QGL_LOAD(TexCoordPointer);
    QGL_LOAD(ColorPointer);
    QGL_LOAD(EnableClientState);
    QGL_LOAD(DisableClientState);
    QGL_LOAD(GetError);
    QGL_LOAD(GetString);
    QGL_LOAD(GetIntegerv);
    QGL_LOAD(GetFloatv);
    QGL_LOAD(Finish);
    QGL_LOAD(Flush);
    QGL_LOAD(PointSize);

    /* Verify critical functions loaded */
    if (!qglClear || !qglViewport || !qglGetString) {
        Com_Printf("ERROR: Failed to load critical GL functions\n");
        return qfalse;
    }

    return qtrue;
}

void QGL_Shutdown(void)
{
    /* Clear all function pointers */
    qglBegin = NULL;
    qglEnd = NULL;
    qglClear = NULL;
    /* ... etc — not strictly necessary since we're shutting down */
}

/* ==========================================================================
   R_Init — Initialize renderer
   ========================================================================== */

int R_Init(void *hinstance, void *hWnd)
{
    (void)hinstance;
    (void)hWnd;

    Com_Printf("------- Renderer Init -------\n");

    /* Register cvars */
    r_mode = Cvar_Get("r_mode", "6", CVAR_ARCHIVE);  /* 6 = 1024x768 in Q2 */
    r_fullscreen = Cvar_Get("vid_fullscreen", "0", CVAR_ARCHIVE);
    r_drawworld = Cvar_Get("r_drawworld", "1", 0);
    r_drawentities = Cvar_Get("r_drawentities", "1", 0);
    r_speeds = Cvar_Get("r_speeds", "0", 0);
    r_novis = Cvar_Get("r_novis", "0", 0);
    r_nocull = Cvar_Get("r_nocull", "0", 0);
    gl_texturemode = Cvar_Get("gl_texturemode", "GL_LINEAR_MIPMAP_LINEAR", CVAR_ARCHIVE);
    gl_modulate = Cvar_Get("gl_modulate", "1", CVAR_ARCHIVE);
    vid_gamma = Cvar_Get("vid_gamma", "1.0", CVAR_ARCHIVE);

    /* SoF-specific renderer cvars */
    ghl_specular = Cvar_Get("ghl_specular", "1", CVAR_ARCHIVE);
    ghl_mip = Cvar_Get("ghl_mip", "1", CVAR_ARCHIVE);

    /* Create window and GL context */
    {
        int w, h;
        int fs = (int)r_fullscreen->value;

        /* Parse r_mode into width/height (Quake II standard modes) */
        R_GetModeSize((int)r_mode->value, &w, &h);
        if (!Sys_CreateWindow(w, h, fs)) {
            Com_Error(ERR_FATAL, "R_Init: couldn't create window");
            return -1;
        }
    }

    /* Load GL function pointers */
    if (!QGL_Init()) {
        Com_Error(ERR_FATAL, "R_Init: QGL_Init failed");
        return -1;
    }

    /* Query GL info */
    gl_state.vendor_string = (const char *)qglGetString(GL_VENDOR);
    gl_state.renderer_string = (const char *)qglGetString(GL_RENDERER);
    gl_state.version_string = (const char *)qglGetString(GL_VERSION);
    gl_state.extensions_string = (const char *)qglGetString(GL_EXTENSIONS);

    Com_Printf("GL_VENDOR: %s\n", gl_state.vendor_string ? gl_state.vendor_string : "unknown");
    Com_Printf("GL_RENDERER: %s\n", gl_state.renderer_string ? gl_state.renderer_string : "unknown");
    Com_Printf("GL_VERSION: %s\n", gl_state.version_string ? gl_state.version_string : "unknown");

    /* Check for extensions */
    if (gl_state.extensions_string) {
        gl_state.have_multitexture = (strstr(gl_state.extensions_string, "GL_ARB_multitexture") != NULL);
        gl_state.have_s3tc = (strstr(gl_state.extensions_string, "GL_EXT_texture_compression_s3tc") != NULL ||
                              strstr(gl_state.extensions_string, "GL_S3_s3tc") != NULL);
        gl_state.have_compiled_vertex_array = (strstr(gl_state.extensions_string, "GL_EXT_compiled_vertex_array") != NULL);
    }

    if (gl_state.have_multitexture) {
        qglActiveTextureARB = (PFNGLACTIVETEXTUREARBPROC)Sys_GL_GetProcAddress("glActiveTextureARB");
        qglClientActiveTextureARB = (PFNGLCLIENTACTIVETEXTUREARBPROC)Sys_GL_GetProcAddress("glClientActiveTextureARB");
        if (qglActiveTextureARB)
            Com_Printf("...using GL_ARB_multitexture\n");
    }

    /* Set initial GL state */
    qglClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    qglEnable(GL_DEPTH_TEST);
    qglEnable(GL_CULL_FACE);
    qglEnable(GL_TEXTURE_2D);

    /* Initialize texture system */
    R_InitImages();

    /* Register map/camera commands */
    R_InitSurfCommands();

    Com_Printf("------- Renderer Initialized -------\n");

    return 1;  /* success */
}

/* ==========================================================================
   R_Shutdown
   ========================================================================== */

void R_Shutdown(void)
{
    R_ShutdownImages();
    QGL_Shutdown();
    Sys_DestroyWindow();
}

/* Forward declarations — defined in later sections */
static void R_DrawParticles(void);
static void R_DrawDlights(void);
static void R_UpdateDlights(void);

/* ==========================================================================
   Frame Rendering
   ========================================================================== */

void R_BeginFrame(float camera_separation)
{
    static int last_frame_time;
    int now;
    float frametime;

    (void)camera_separation;

    if (!qglClear)
        return;

    /* Compute frametime for particle simulation */
    now = Sys_Milliseconds();
    frametime = (now - last_frame_time) * 0.001f;
    if (frametime > 0.1f) frametime = 0.1f;  /* clamp to 100ms */
    if (frametime < 0.001f) frametime = 0.001f;
    last_frame_time = now;

    /* Update particles and dynamic lights */
    R_UpdateParticles(frametime);
    R_UpdateDlights();

    /* Clear screen */
    qglClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    qglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Render 3D world if loaded — build refdef from freecam state */
    if (R_WorldLoaded()) {
        refdef_t rd;
        vec3_t cam_org, cam_ang;

        memset(&rd, 0, sizeof(rd));
        R_GetCameraOrigin(cam_org);
        R_GetCameraAngles(cam_ang);

        rd.x = 0;
        rd.y = 0;
        rd.width = g_display.width;
        rd.height = g_display.height;
        rd.fov_x = 90.0f;
        rd.fov_y = 73.74f;  /* CalcFovY(90, 1024, 768) */
        VectorCopy(cam_org, rd.vieworg);
        VectorCopy(cam_ang, rd.viewangles);

        R_RenderFrame(&rd);
    }

    /* Switch to 2D mode for HUD/console overlay */
    qglViewport(0, 0, g_display.width, g_display.height);
    qglMatrixMode(GL_PROJECTION);
    qglLoadIdentity();
    qglOrtho(0, g_display.width, g_display.height, 0, -99999, 99999);
    qglMatrixMode(GL_MODELVIEW);
    qglLoadIdentity();
    qglDisable(GL_DEPTH_TEST);
    qglDisable(GL_CULL_FACE);
}

/*
 * R_Setup3DProjection - Set perspective projection from refdef
 */
static void R_Setup3DProjection(refdef_t *fd)
{
    float xmin, xmax, ymin, ymax;
    float znear = 4.0f, zfar = 8192.0f;

    ymax = znear * (float)tan(fd->fov_y * 3.14159265 / 360.0);
    ymin = -ymax;
    xmax = znear * (float)tan(fd->fov_x * 3.14159265 / 360.0);
    xmin = -xmax;

    qglViewport(fd->x, g_display.height - (fd->y + fd->height),
                fd->width, fd->height);

    qglMatrixMode(GL_PROJECTION);
    qglLoadIdentity();
    qglFrustum(xmin, xmax, ymin, ymax, znear, zfar);
}

/*
 * R_Setup3DModelview - Set modelview matrix from refdef vieworg/viewangles
 */
static void R_Setup3DModelview(refdef_t *fd)
{
    qglMatrixMode(GL_MODELVIEW);
    qglLoadIdentity();

    /* Q2 coordinate conversion: Z-up → GL Y-up */
    qglRotatef(-90, 1, 0, 0);
    qglRotatef(90, 0, 0, 1);

    /* Apply view rotation */
    qglRotatef(-fd->viewangles[2], 1, 0, 0);  /* roll */
    qglRotatef(-fd->viewangles[0], 0, 1, 0);  /* pitch */
    qglRotatef(-fd->viewangles[1], 0, 0, 1);  /* yaw */

    /* Translate to view origin (inverted) */
    qglTranslatef(-fd->vieworg[0], -fd->vieworg[1], -fd->vieworg[2]);
}

/* ==========================================================================
   Entity Rendering
   Iterate game entity list and draw inline BSP models (func_door, etc.)
   ========================================================================== */

/* Access game's entity list through engine bridge */
extern game_export_t *SV_GetGameExport(void);
extern const char *SV_GetConfigstring(int index);

static void R_DrawBrushEntities(void)
{
    game_export_t *ge = SV_GetGameExport();
    int i;

    if (!ge || !ge->edicts || ge->num_edicts <= 0)
        return;

    for (i = 1; i < ge->num_edicts; i++) {
        edict_t *ent = (edict_t *)((byte *)ge->edicts + i * ge->edict_size);
        const char *model_name;

        if (!ent->inuse)
            continue;
        if (ent->s.modelindex <= 0)
            continue;

        /* Check if this is an inline BSP model (*N) */
        model_name = SV_GetConfigstring(CS_MODELS + ent->s.modelindex);
        if (!model_name || model_name[0] != '*')
            continue;

        /* Extract BSP submodel index from "*N" name */
        R_DrawBrushModel(atoi(model_name + 1), ent->s.origin, ent->s.angles);
    }
}

/*
 * R_DrawSkyBox - Render the skybox cube centered on the camera
 *
 * Draws a unit cube with 6 textured faces at the camera position.
 * Depth writes disabled so sky is always behind world geometry.
 * Uses Q2 coordinate system (Z-up).
 */
static void R_DrawSkyBox(void)
{
    /*
     * Sky face vertex data: 6 faces of a cube, each with 4 vertices.
     * Q2 convention: rt=+x, lf=-x, up=+z, dn=-z, ft=-y, bk=+y
     * (ft looks toward negative Y in Q2 coordinates)
     */
    static const float sky_verts[6][4][3] = {
        /* rt (+X) */ {{ 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1}},
        /* lf (-X) */ {{-1, 1, 1}, {-1, 1,-1}, {-1,-1,-1}, {-1,-1, 1}},
        /* up (+Z) */ {{-1, 1, 1}, { 1, 1, 1}, { 1,-1, 1}, {-1,-1, 1}},
        /* dn (-Z) */ {{-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1}},
        /* ft (-Y) */ {{ 1,-1, 1}, {-1,-1, 1}, {-1,-1,-1}, { 1,-1,-1}},
        /* bk (+Y) */ {{-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1}},
    };
    float s = 2048.0f;
    vec3_t cam;
    int i;

    if (!sky_loaded)
        return;

    qglDepthMask(GL_FALSE);
    qglDisable(GL_DEPTH_TEST);
    qglEnable(GL_TEXTURE_2D);
    qglColor4f(1, 1, 1, 1);

    /*
     * The modelview currently has rotation + translation (-vieworg).
     * Push and undo the translation so the sky cube stays centered on
     * the camera (rotation-only view).
     */
    qglMatrixMode(GL_MODELVIEW);
    qglPushMatrix();

    R_GetCameraOrigin(cam);
    qglTranslatef(cam[0], cam[1], cam[2]);

    if (sky_rotate != 0)
        qglRotatef(sky_rotate, sky_axis[0], sky_axis[1], sky_axis[2]);

    for (i = 0; i < 6; i++) {
        if (!sky_textures[i])
            continue;

        qglBindTexture(GL_TEXTURE_2D, sky_textures[i]);
        qglBegin(GL_QUADS);
        qglTexCoord2f(0, 0); qglVertex3f(sky_verts[i][0][0]*s, sky_verts[i][0][1]*s, sky_verts[i][0][2]*s);
        qglTexCoord2f(1, 0); qglVertex3f(sky_verts[i][1][0]*s, sky_verts[i][1][1]*s, sky_verts[i][1][2]*s);
        qglTexCoord2f(1, 1); qglVertex3f(sky_verts[i][2][0]*s, sky_verts[i][2][1]*s, sky_verts[i][2][2]*s);
        qglTexCoord2f(0, 1); qglVertex3f(sky_verts[i][3][0]*s, sky_verts[i][3][1]*s, sky_verts[i][3][2]*s);
        qglEnd();
    }

    qglPopMatrix();
    qglEnable(GL_DEPTH_TEST);
    qglDepthMask(GL_TRUE);
}

/*
 * R_RenderFrame - Render a 3D scene from a refdef_t
 *
 * This is the Q2-standard rendering entry point. The engine fills a
 * refdef_t with the view parameters, entity list, particle list, etc.
 * and calls this to render the full 3D scene.
 */
void R_RenderFrame(refdef_t *fd)
{
    if (!fd || !qglClear)
        return;

    /* Update camera state to match refdef (so R_DrawWorld uses correct PVS) */
    R_SetCameraOrigin(fd->vieworg);
    R_SetCameraAngles(fd->viewangles);

    /* Set up 3D projection and modelview */
    R_Setup3DProjection(fd);
    R_Setup3DModelview(fd);

    /* Enable 3D rendering state */
    qglEnable(GL_DEPTH_TEST);
    qglDepthFunc(GL_LEQUAL);
    qglDepthMask(GL_TRUE);
    qglEnable(GL_TEXTURE_2D);
    qglClear(GL_DEPTH_BUFFER_BIT);

    /* Draw skybox (before world, depth writes off so world occludes it) */
    R_DrawSkyBox();

    /* Draw BSP world */
    if (R_WorldLoaded() && r_drawworld->value)
        R_DrawWorld();

    /* Draw brush entities (inline BSP models) */
    if (r_drawentities->value)
        R_DrawBrushEntities();

    /* Draw particles */
    R_DrawParticles();

    /* Draw dynamic lights */
    R_DrawDlights();

    /* Full-screen blend (damage flash, underwater tint) */
    if (fd->blend[3] > 0) {
        qglDisable(GL_TEXTURE_2D);
        qglDisable(GL_DEPTH_TEST);
        qglEnable(GL_BLEND);
        qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        qglMatrixMode(GL_PROJECTION);
        qglPushMatrix();
        qglLoadIdentity();
        qglOrtho(0, 1, 1, 0, -99999, 99999);
        qglMatrixMode(GL_MODELVIEW);
        qglPushMatrix();
        qglLoadIdentity();

        qglColor4f(fd->blend[0], fd->blend[1], fd->blend[2], fd->blend[3]);
        qglBegin(GL_QUADS);
        qglVertex3f(0, 0, 0);
        qglVertex3f(1, 0, 0);
        qglVertex3f(1, 1, 0);
        qglVertex3f(0, 1, 0);
        qglEnd();

        qglMatrixMode(GL_PROJECTION);
        qglPopMatrix();
        qglMatrixMode(GL_MODELVIEW);
        qglPopMatrix();

        qglDisable(GL_BLEND);
        qglEnable(GL_TEXTURE_2D);
        qglColor4f(1, 1, 1, 1);
    }
}

void R_EndFrame(void)
{
    /* Swap buffers */
    Sys_SwapBuffers();
}

/* ==========================================================================
   Particle System
   ========================================================================== */

typedef struct {
    vec3_t  org;
    vec3_t  vel;
    vec3_t  accel;
    float   color[4];       /* RGBA */
    float   alpha_decay;    /* alpha lost per second */
    float   time;           /* time remaining */
} r_particle_t;

#define MAX_R_PARTICLES     2048

static r_particle_t r_particles[MAX_R_PARTICLES];
static int          r_num_particles;

/*
 * R_ClearParticles - Remove all active particles
 */
void R_ClearParticles(void)
{
    r_num_particles = 0;
}

/*
 * R_AddParticle - Spawn a single particle
 */
static void R_AddParticle(vec3_t org, vec3_t vel, vec3_t accel,
                           float r, float g, float b, float a,
                           float alpha_decay, float lifetime)
{
    r_particle_t *p;

    if (r_num_particles >= MAX_R_PARTICLES)
        return;

    p = &r_particles[r_num_particles++];
    VectorCopy(org, p->org);
    VectorCopy(vel, p->vel);
    VectorCopy(accel, p->accel);
    p->color[0] = r; p->color[1] = g;
    p->color[2] = b; p->color[3] = a;
    p->alpha_decay = alpha_decay;
    p->time = lifetime;
}

/*
 * R_ParticleEffect - Spawn a burst of particles at a point
 * type: 0=bullet impact, 1=blood, 2=explosion, 3=spark
 */
void R_ParticleEffect(vec3_t org, vec3_t dir, int type, int count)
{
    int i;
    float spread, speed;
    vec3_t vel, accel;

    switch (type) {
    case 0: /* bullet impact — grey/brown dust */
        spread = 30.0f; speed = 60.0f;
        for (i = 0; i < count; i++) {
            vel[0] = dir[0] * speed + ((float)(rand()%100) - 50) * spread / 50.0f;
            vel[1] = dir[1] * speed + ((float)(rand()%100) - 50) * spread / 50.0f;
            vel[2] = dir[2] * speed + ((float)(rand()%100) - 50) * spread / 50.0f + 20.0f;
            VectorSet(accel, 0, 0, -200);
            R_AddParticle(org, vel, accel, 0.6f, 0.5f, 0.4f, 1.0f, 2.0f, 0.8f);
        }
        break;

    case 1: /* blood — red particles */
        spread = 40.0f; speed = 80.0f;
        for (i = 0; i < count; i++) {
            vel[0] = dir[0] * speed + ((float)(rand()%100) - 50) * spread / 50.0f;
            vel[1] = dir[1] * speed + ((float)(rand()%100) - 50) * spread / 50.0f;
            vel[2] = dir[2] * speed + ((float)(rand()%100) - 50) * spread / 50.0f + 30.0f;
            VectorSet(accel, 0, 0, -300);
            R_AddParticle(org, vel, accel,
                          0.6f + (rand()%40)*0.01f, 0.0f, 0.0f, 1.0f,
                          1.5f, 1.0f);
        }
        break;

    case 2: /* explosion — orange/yellow burst */
        spread = 100.0f; speed = 150.0f;
        for (i = 0; i < count; i++) {
            vel[0] = ((float)(rand()%100) - 50) * spread / 50.0f;
            vel[1] = ((float)(rand()%100) - 50) * spread / 50.0f;
            vel[2] = ((float)(rand()%100) - 50) * spread / 50.0f + 50.0f;
            VectorSet(accel, 0, 0, -100);
            R_AddParticle(org, vel, accel,
                          1.0f, 0.6f + (rand()%40)*0.01f, 0.1f, 1.0f,
                          1.0f, 1.5f);
        }
        break;

    case 3: /* spark — bright yellow/white */
        spread = 50.0f; speed = 120.0f;
        for (i = 0; i < count; i++) {
            vel[0] = dir[0] * speed + ((float)(rand()%100) - 50) * spread / 50.0f;
            vel[1] = dir[1] * speed + ((float)(rand()%100) - 50) * spread / 50.0f;
            vel[2] = dir[2] * speed + ((float)(rand()%100) - 50) * spread / 50.0f + 40.0f;
            VectorSet(accel, 0, 0, -400);
            R_AddParticle(org, vel, accel,
                          1.0f, 1.0f, 0.7f, 1.0f,
                          3.0f, 0.5f);
        }
        break;
    }
}

/*
 * R_UpdateParticles - Simulate particle physics
 */
void R_UpdateParticles(float frametime)
{
    int i;
    r_particle_t *p;

    for (i = 0; i < r_num_particles; ) {
        p = &r_particles[i];
        p->time -= frametime;
        p->color[3] -= p->alpha_decay * frametime;

        if (p->time <= 0 || p->color[3] <= 0) {
            /* Remove by swapping with last */
            r_particles[i] = r_particles[--r_num_particles];
            continue;
        }

        /* Euler integration */
        p->vel[0] += p->accel[0] * frametime;
        p->vel[1] += p->accel[1] * frametime;
        p->vel[2] += p->accel[2] * frametime;
        p->org[0] += p->vel[0] * frametime;
        p->org[1] += p->vel[1] * frametime;
        p->org[2] += p->vel[2] * frametime;

        i++;
    }
}

/*
 * R_DrawParticles - Render all active particles as GL_POINTS
 */
static void R_DrawParticles(void)
{
    int i;

    if (r_num_particles == 0)
        return;

    qglDisable(GL_TEXTURE_2D);
    qglEnable(GL_BLEND);
    qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    qglDepthMask(GL_FALSE);
    if (qglPointSize) qglPointSize(3.0f);

    qglBegin(GL_POINTS);
    for (i = 0; i < r_num_particles; i++) {
        r_particle_t *p = &r_particles[i];
        qglColor4f(p->color[0], p->color[1], p->color[2], p->color[3]);
        qglVertex3f(p->org[0], p->org[1], p->org[2]);
    }
    qglEnd();

    if (qglPointSize) qglPointSize(1.0f);
    qglDepthMask(GL_TRUE);
    qglDisable(GL_BLEND);
    qglEnable(GL_TEXTURE_2D);
    qglColor4f(1, 1, 1, 1);
}

/* ==========================================================================
   Dynamic Lights
   ========================================================================== */

/*
 * R_AddDlight - Add a temporary dynamic light
 */
void R_AddDlight(vec3_t origin, float r, float g, float b, float intensity,
                  float duration)
{
    r_dlight_t *dl;

    if (r_num_dlights >= MAX_DLIGHTS)
        return;

    dl = &r_dlights[r_num_dlights++];
    VectorCopy(origin, dl->origin);
    dl->color[0] = r; dl->color[1] = g; dl->color[2] = b;
    dl->intensity = intensity;
    dl->die = (float)Sys_Milliseconds() + duration * 1000.0f;
}

/*
 * R_UpdateDlights - Remove expired lights
 */
static void R_UpdateDlights(void)
{
    int i;
    float now = (float)Sys_Milliseconds();

    for (i = 0; i < r_num_dlights; ) {
        if (r_dlights[i].die < now) {
            r_dlights[i] = r_dlights[--r_num_dlights];
            continue;
        }
        /* Fade intensity as it expires */
        {
            float remaining = (r_dlights[i].die - now) * 0.001f;
            if (remaining < 0.2f) {
                float scale = remaining / 0.2f;
                r_dlights[i].intensity *= scale;
            }
        }
        i++;
    }
}

/*
 * R_DrawDlights - Render dynamic lights as additive billboards
 * Simple GL 1.1 approach: draw a bright point-sprite at each light pos.
 */
static void R_DrawDlights(void)
{
    int i;

    if (r_num_dlights == 0)
        return;

    qglDisable(GL_TEXTURE_2D);
    qglEnable(GL_BLEND);
    qglBlendFunc(GL_ONE, GL_ONE);   /* additive blending */
    qglDepthMask(GL_FALSE);
    if (qglPointSize) qglPointSize(8.0f);

    qglBegin(GL_POINTS);
    for (i = 0; i < r_num_dlights; i++) {
        r_dlight_t *dl = &r_dlights[i];
        float scale = dl->intensity / 200.0f;
        qglColor4f(dl->color[0] * scale, dl->color[1] * scale,
                    dl->color[2] * scale, 1.0f);
        qglVertex3f(dl->origin[0], dl->origin[1], dl->origin[2]);
    }
    qglEnd();

    if (qglPointSize) qglPointSize(1.0f);
    qglDepthMask(GL_TRUE);
    qglDisable(GL_BLEND);
    qglEnable(GL_TEXTURE_2D);
    qglColor4f(1, 1, 1, 1);
}

/* ==========================================================================
   Model Registration
   ========================================================================== */

static model_t  mod_known[MAX_MOD_KNOWN];
static int      mod_numknown;
static int      mod_registration_sequence;

static model_t *R_FindModel(const char *name)
{
    int i;

    if (!name || !name[0])
        return NULL;

    /* Check if already loaded */
    for (i = 0; i < mod_numknown; i++) {
        if (!strcmp(mod_known[i].name, name)) {
            mod_known[i].registration_sequence = mod_registration_sequence;
            return &mod_known[i];
        }
    }

    return NULL;
}

void R_BeginRegistration(const char *map)
{
    Com_Printf("R_BeginRegistration: %s\n", map);
    mod_registration_sequence++;

    /* Load the BSP world map */
    R_LoadWorldMap(map);
}

struct model_s *R_RegisterModel(const char *name)
{
    model_t *mod;

    if (!name || !name[0])
        return NULL;

    Com_DPrintf("R_RegisterModel: %s\n", name);

    /* Check cache first */
    mod = R_FindModel(name);
    if (mod)
        return (struct model_s *)mod;

    /* Allocate new slot */
    if (mod_numknown >= MAX_MOD_KNOWN) {
        Com_Printf("R_RegisterModel: MAX_MOD_KNOWN exceeded\n");
        return NULL;
    }

    mod = &mod_known[mod_numknown++];
    memset(mod, 0, sizeof(*mod));
    Q_strncpyz(mod->name, name, MAX_QPATH);
    mod->registration_sequence = mod_registration_sequence;

    /* Determine model type from name */
    if (name[0] == '*') {
        /* Inline BSP model (*1, *2, etc.) */
        mod->type = mod_brush;
        mod->bsp_submodel = atoi(name + 1);
    } else if (strstr(name, ".sp2") || strstr(name, ".SP2")) {
        mod->type = mod_sprite;
    } else if (strstr(name, ".ghoul") || strstr(name, ".glm")) {
        mod->type = mod_ghoul;
    } else if (strstr(name, ".md2") || strstr(name, ".mdx")) {
        mod->type = mod_alias;
    } else {
        /* Default to alias for unknown extensions */
        mod->type = mod_alias;
    }

    return (struct model_s *)mod;
}

struct image_s *R_RegisterSkin(const char *name)
{
    image_t *img;

    if (!name || !name[0])
        return NULL;

    Com_DPrintf("R_RegisterSkin: %s\n", name);

    img = R_FindImage(name);
    return (struct image_s *)img;
}

image_t *R_RegisterPic(const char *name)
{
    return R_FindPic(name);
}

void R_SetSky(const char *name, float rotate, vec3_t axis)
{
    static const char *sky_suffixes[6] = { "rt", "lf", "up", "dn", "ft", "bk" };
    char path[MAX_QPATH];
    int i;

    /* Free old sky textures */
    for (i = 0; i < 6; i++) {
        if (sky_textures[i]) {
            qglDeleteTextures(1, &sky_textures[i]);
            sky_textures[i] = 0;
        }
    }
    sky_loaded = qfalse;

    if (!name || !name[0])
        return;

    sky_rotate = rotate;
    if (axis) {
        VectorCopy(axis, sky_axis);
    } else {
        VectorSet(sky_axis, 0, 0, 1);
    }

    /* Load 6 skybox faces: env/[name][suffix].tga */
    for (i = 0; i < 6; i++) {
        Com_sprintf(path, sizeof(path), "env/%s%s.tga", name, sky_suffixes[i]);
        sky_textures[i] = R_LoadSkyTexture(path);
        if (!sky_textures[i]) {
            /* Try pcx fallback */
            Com_sprintf(path, sizeof(path), "env/%s%s.pcx", name, sky_suffixes[i]);
            sky_textures[i] = R_LoadSkyTexture(path);
        }
    }

    /* Check if at least the front face loaded */
    if (sky_textures[4]) {
        sky_loaded = qtrue;
        Com_Printf("Sky: %s (%d faces loaded)\n", name,
                   (sky_textures[0]?1:0) + (sky_textures[1]?1:0) +
                   (sky_textures[2]?1:0) + (sky_textures[3]?1:0) +
                   (sky_textures[4]?1:0) + (sky_textures[5]?1:0));
    } else {
        Com_DPrintf("R_SetSky: couldn't load sky '%s'\n", name);
    }
}

void R_EndRegistration(void)
{
    int i;

    /* Free models that weren't referenced this registration sequence */
    for (i = 0; i < mod_numknown; i++) {
        if (mod_known[i].name[0] &&
            mod_known[i].registration_sequence != mod_registration_sequence) {
            memset(&mod_known[i], 0, sizeof(mod_known[i]));
        }
    }

    /* Free unused images */
    R_ImageEndRegistration();
}

/* ==========================================================================
   2D Drawing Stubs
   ========================================================================== */

void R_DrawGetPicSize(int *w, int *h, const char *name)
{
    image_t *pic = R_FindPic(name);
    if (pic) {
        *w = pic->width;
        *h = pic->height;
    } else {
        *w = 0;
        *h = 0;
    }
}

void R_DrawPic(int x, int y, const char *name)
{
    image_t *pic = R_FindPic(name);
    if (!pic || !pic->texnum || !qglBegin)
        return;

    qglEnable(GL_TEXTURE_2D);
    qglEnable(GL_BLEND);
    qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    qglBindTexture(GL_TEXTURE_2D, pic->texnum);
    qglColor4f(1, 1, 1, 1);

    qglBegin(GL_QUADS);
    qglTexCoord2f(0, 0); qglVertex3f((float)x, (float)y, 0);
    qglTexCoord2f(1, 0); qglVertex3f((float)(x + pic->width), (float)y, 0);
    qglTexCoord2f(1, 1); qglVertex3f((float)(x + pic->width), (float)(y + pic->height), 0);
    qglTexCoord2f(0, 1); qglVertex3f((float)x, (float)(y + pic->height), 0);
    qglEnd();

    qglDisable(GL_BLEND);
}

void R_DrawStretchPic(int x, int y, int w, int h, const char *name)
{
    image_t *pic = R_FindPic(name);
    if (!pic || !pic->texnum || !qglBegin)
        return;

    qglEnable(GL_TEXTURE_2D);
    qglEnable(GL_BLEND);
    qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    qglBindTexture(GL_TEXTURE_2D, pic->texnum);
    qglColor4f(1, 1, 1, 1);

    qglBegin(GL_QUADS);
    qglTexCoord2f(0, 0); qglVertex3f((float)x, (float)y, 0);
    qglTexCoord2f(1, 0); qglVertex3f((float)(x + w), (float)y, 0);
    qglTexCoord2f(1, 1); qglVertex3f((float)(x + w), (float)(y + h), 0);
    qglTexCoord2f(0, 1); qglVertex3f((float)x, (float)(y + h), 0);
    qglEnd();

    qglDisable(GL_BLEND);
}

/*
 * Built-in 8x8 bitmap font texture (generated once on first use).
 * Maps ASCII 32-127 into a 128x64 texture (16 chars x 6 rows).
 * Uses a simple 5x7 pixel font baked into GL texture.
 */
static GLuint   r_charTexture = 0;

/* Minimal 5x7 font bitmaps for ASCII 33-126 (printable chars).
 * Each char is 7 bytes, each byte is a row, bits 4-0 = pixels.
 * Char 0 = '!' (ASCII 33), etc.
 */
static const byte font5x7[] = {
    /* ! */  0x04,0x04,0x04,0x04,0x00,0x04,0x00,
    /* " */  0x0A,0x0A,0x00,0x00,0x00,0x00,0x00,
    /* # */  0x0A,0x1F,0x0A,0x1F,0x0A,0x00,0x00,
    /* $ */  0x0E,0x15,0x06,0x14,0x0F,0x04,0x00,
    /* % */  0x13,0x0B,0x04,0x1A,0x19,0x00,0x00,
    /* & */  0x06,0x09,0x06,0x15,0x12,0x0D,0x00,
    /* ' */  0x04,0x04,0x00,0x00,0x00,0x00,0x00,
    /* ( */  0x08,0x04,0x04,0x04,0x04,0x08,0x00,
    /* ) */  0x02,0x04,0x04,0x04,0x04,0x02,0x00,
    /* * */  0x00,0x0A,0x04,0x0A,0x00,0x00,0x00,
    /* + */  0x00,0x04,0x0E,0x04,0x00,0x00,0x00,
    /* , */  0x00,0x00,0x00,0x00,0x04,0x02,0x00,
    /* - */  0x00,0x00,0x0E,0x00,0x00,0x00,0x00,
    /* . */  0x00,0x00,0x00,0x00,0x04,0x00,0x00,
    /* / */  0x10,0x08,0x04,0x02,0x01,0x00,0x00,
    /* 0 */  0x0E,0x11,0x19,0x15,0x13,0x0E,0x00,
    /* 1 */  0x04,0x06,0x04,0x04,0x04,0x0E,0x00,
    /* 2 */  0x0E,0x11,0x08,0x04,0x02,0x1F,0x00,
    /* 3 */  0x0E,0x11,0x0C,0x10,0x11,0x0E,0x00,
    /* 4 */  0x08,0x0C,0x0A,0x1F,0x08,0x08,0x00,
    /* 5 */  0x1F,0x01,0x0F,0x10,0x11,0x0E,0x00,
    /* 6 */  0x0C,0x02,0x0F,0x11,0x11,0x0E,0x00,
    /* 7 */  0x1F,0x10,0x08,0x04,0x02,0x02,0x00,
    /* 8 */  0x0E,0x11,0x0E,0x11,0x11,0x0E,0x00,
    /* 9 */  0x0E,0x11,0x11,0x1E,0x08,0x06,0x00,
    /* : */  0x00,0x04,0x00,0x04,0x00,0x00,0x00,
    /* ; */  0x00,0x04,0x00,0x04,0x02,0x00,0x00,
    /* < */  0x08,0x04,0x02,0x04,0x08,0x00,0x00,
    /* = */  0x00,0x0E,0x00,0x0E,0x00,0x00,0x00,
    /* > */  0x02,0x04,0x08,0x04,0x02,0x00,0x00,
    /* ? */  0x0E,0x11,0x08,0x04,0x00,0x04,0x00,
    /* @ */  0x0E,0x11,0x1D,0x1D,0x01,0x0E,0x00,
    /* A */  0x0E,0x11,0x11,0x1F,0x11,0x11,0x00,
    /* B */  0x0F,0x11,0x0F,0x11,0x11,0x0F,0x00,
    /* C */  0x0E,0x11,0x01,0x01,0x11,0x0E,0x00,
    /* D */  0x07,0x09,0x11,0x11,0x09,0x07,0x00,
    /* E */  0x1F,0x01,0x0F,0x01,0x01,0x1F,0x00,
    /* F */  0x1F,0x01,0x0F,0x01,0x01,0x01,0x00,
    /* G */  0x0E,0x11,0x01,0x19,0x11,0x0E,0x00,
    /* H */  0x11,0x11,0x1F,0x11,0x11,0x11,0x00,
    /* I */  0x0E,0x04,0x04,0x04,0x04,0x0E,0x00,
    /* J */  0x1C,0x08,0x08,0x08,0x09,0x06,0x00,
    /* K */  0x11,0x09,0x07,0x09,0x11,0x11,0x00,
    /* L */  0x01,0x01,0x01,0x01,0x01,0x1F,0x00,
    /* M */  0x11,0x1B,0x15,0x11,0x11,0x11,0x00,
    /* N */  0x11,0x13,0x15,0x19,0x11,0x11,0x00,
    /* O */  0x0E,0x11,0x11,0x11,0x11,0x0E,0x00,
    /* P */  0x0F,0x11,0x0F,0x01,0x01,0x01,0x00,
    /* Q */  0x0E,0x11,0x11,0x15,0x09,0x16,0x00,
    /* R */  0x0F,0x11,0x0F,0x09,0x11,0x11,0x00,
    /* S */  0x0E,0x11,0x06,0x08,0x11,0x0E,0x00,
    /* T */  0x1F,0x04,0x04,0x04,0x04,0x04,0x00,
    /* U */  0x11,0x11,0x11,0x11,0x11,0x0E,0x00,
    /* V */  0x11,0x11,0x11,0x0A,0x0A,0x04,0x00,
    /* W */  0x11,0x11,0x15,0x15,0x1B,0x11,0x00,
    /* X */  0x11,0x0A,0x04,0x0A,0x11,0x00,0x00,
    /* Y */  0x11,0x0A,0x04,0x04,0x04,0x04,0x00,
    /* Z */  0x1F,0x08,0x04,0x02,0x01,0x1F,0x00,
    /* [ */  0x0E,0x02,0x02,0x02,0x02,0x0E,0x00,
    /* \ */  0x01,0x02,0x04,0x08,0x10,0x00,0x00,
    /* ] */  0x0E,0x08,0x08,0x08,0x08,0x0E,0x00,
    /* ^ */  0x04,0x0A,0x00,0x00,0x00,0x00,0x00,
    /* _ */  0x00,0x00,0x00,0x00,0x00,0x1F,0x00,
    /* ` */  0x02,0x04,0x00,0x00,0x00,0x00,0x00,
    /* a */  0x00,0x0E,0x10,0x1E,0x11,0x1E,0x00,
    /* b */  0x01,0x0F,0x11,0x11,0x11,0x0F,0x00,
    /* c */  0x00,0x0E,0x01,0x01,0x01,0x0E,0x00,
    /* d */  0x10,0x1E,0x11,0x11,0x11,0x1E,0x00,
    /* e */  0x00,0x0E,0x11,0x1F,0x01,0x0E,0x00,
    /* f */  0x0C,0x02,0x0F,0x02,0x02,0x02,0x00,
    /* g */  0x00,0x1E,0x11,0x1E,0x10,0x0E,0x00,
    /* h */  0x01,0x0F,0x11,0x11,0x11,0x11,0x00,
    /* i */  0x04,0x00,0x06,0x04,0x04,0x0E,0x00,
    /* j */  0x08,0x00,0x0C,0x08,0x08,0x06,0x00,
    /* k */  0x01,0x09,0x05,0x03,0x05,0x09,0x00,
    /* l */  0x06,0x04,0x04,0x04,0x04,0x0E,0x00,
    /* m */  0x00,0x0B,0x15,0x15,0x11,0x11,0x00,
    /* n */  0x00,0x0F,0x11,0x11,0x11,0x11,0x00,
    /* o */  0x00,0x0E,0x11,0x11,0x11,0x0E,0x00,
    /* p */  0x00,0x0F,0x11,0x0F,0x01,0x01,0x00,
    /* q */  0x00,0x1E,0x11,0x1E,0x10,0x10,0x00,
    /* r */  0x00,0x0D,0x13,0x01,0x01,0x01,0x00,
    /* s */  0x00,0x0E,0x02,0x04,0x08,0x0E,0x00,
    /* t */  0x02,0x0F,0x02,0x02,0x02,0x0C,0x00,
    /* u */  0x00,0x11,0x11,0x11,0x11,0x1E,0x00,
    /* v */  0x00,0x11,0x11,0x0A,0x0A,0x04,0x00,
    /* w */  0x00,0x11,0x11,0x15,0x15,0x0A,0x00,
    /* x */  0x00,0x11,0x0A,0x04,0x0A,0x11,0x00,
    /* y */  0x00,0x11,0x11,0x1E,0x10,0x0E,0x00,
    /* z */  0x00,0x1F,0x08,0x04,0x02,0x1F,0x00,
    /* { */  0x08,0x04,0x02,0x04,0x08,0x00,0x00,
    /* | */  0x04,0x04,0x04,0x04,0x04,0x04,0x00,
    /* } */  0x02,0x04,0x08,0x04,0x02,0x00,0x00,
    /* ~ */  0x00,0x05,0x0A,0x00,0x00,0x00,0x00,
};

static void R_InitCharFont(void)
{
    byte pixels[128 * 64 * 4];  /* RGBA 128x64 */
    int ch, row, col, px, py;

    memset(pixels, 0, sizeof(pixels));

    for (ch = 33; ch <= 126; ch++) {
        int idx = ch - 33;
        int cx = (ch % 16) * 8;
        int cy = ((ch - 32) / 16) * 8;
        const byte *glyph = &font5x7[idx * 7];

        for (py = 0; py < 7; py++) {
            byte bits = glyph[py];
            for (px = 0; px < 5; px++) {
                if (bits & (1 << px)) {
                    int tx = cx + px + 1;
                    int ty = cy + py;
                    int ofs = (ty * 128 + tx) * 4;
                    pixels[ofs + 0] = 255;
                    pixels[ofs + 1] = 255;
                    pixels[ofs + 2] = 255;
                    pixels[ofs + 3] = 255;
                }
            }
        }
    }

    qglGenTextures(1, &r_charTexture);
    qglBindTexture(GL_TEXTURE_2D, r_charTexture);
    qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 64, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

/* Current draw color for text rendering (default green for console) */
static float r_drawcolor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };

void R_SetDrawColor(float r, float g, float b, float a)
{
    r_drawcolor[0] = r;
    r_drawcolor[1] = g;
    r_drawcolor[2] = b;
    r_drawcolor[3] = a;
}

void R_DrawChar(int x, int y, int ch)
{
    float s1, t1, s2, t2;
    int cx, cy;

    if (!qglBegin)
        return;

    ch &= 255;
    if (ch == ' ' || ch < 32 || ch > 126)
        return;

    /* Generate font texture on first use */
    if (!r_charTexture)
        R_InitCharFont();

    /* Calculate UV coordinates in 128x64 texture */
    cx = (ch % 16) * 8;
    cy = ((ch - 32) / 16) * 8;
    s1 = cx / 128.0f;
    t1 = cy / 64.0f;
    s2 = (cx + 8) / 128.0f;
    t2 = (cy + 8) / 64.0f;

    qglEnable(GL_TEXTURE_2D);
    qglEnable(GL_BLEND);
    qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    qglBindTexture(GL_TEXTURE_2D, r_charTexture);
    qglColor4f(r_drawcolor[0], r_drawcolor[1], r_drawcolor[2], r_drawcolor[3]);

    qglBegin(GL_QUADS);
    qglTexCoord2f(s1, t1); qglVertex3f((float)x,     (float)y,     0);
    qglTexCoord2f(s2, t1); qglVertex3f((float)(x+8),  (float)y,     0);
    qglTexCoord2f(s2, t2); qglVertex3f((float)(x+8),  (float)(y+8), 0);
    qglTexCoord2f(s1, t2); qglVertex3f((float)x,     (float)(y+8), 0);
    qglEnd();

    qglDisable(GL_BLEND);
    qglColor4f(1, 1, 1, 1);
}

void R_DrawString(int x, int y, const char *str)
{
    while (*str) {
        if (*str != ' ')
            R_DrawChar(x, y, (unsigned char)*str);
        x += 8;
        str++;
    }
}

void R_DrawTileClear(int x, int y, int w, int h, const char *name)
{
    (void)x; (void)y; (void)w; (void)h; (void)name;
}

void R_DrawFill(int x, int y, int w, int h, int c)
{
    float r, g, b, a;

    if (!qglDisable || !qglBegin)
        return;

    /* Unpack RGBA if high bits set, otherwise use Q2 palette index */
    if (c & 0xFF000000) {
        /* ARGB packed color from console */
        a = ((c >> 24) & 0xFF) / 255.0f;
        r = (c & 0xFF) / 255.0f;
        g = ((c >> 8) & 0xFF) / 255.0f;
        b = ((c >> 16) & 0xFF) / 255.0f;
    } else {
        /* Q2 palette index — approximate */
        r = ((c >> 5) & 7) / 7.0f;
        g = ((c >> 2) & 7) / 7.0f;
        b = (c & 3) / 3.0f;
        a = 1.0f;
    }

    qglDisable(GL_TEXTURE_2D);
    qglEnable(GL_BLEND);
    qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    qglColor4f(r, g, b, a);
    qglBegin(GL_QUADS);
    qglVertex3f((float)x, (float)y, 0);
    qglVertex3f((float)(x + w), (float)y, 0);
    qglVertex3f((float)(x + w), (float)(y + h), 0);
    qglVertex3f((float)x, (float)(y + h), 0);
    qglEnd();
    qglDisable(GL_BLEND);
    qglEnable(GL_TEXTURE_2D);
    qglColor4f(1, 1, 1, 1);
}

void R_DrawFadeScreen(void)
{
    R_DrawFadeScreenColor(0, 0, 0, 0.8f);
}

void R_DrawFadeScreenColor(float r, float g, float b, float a)
{
    if (!qglEnable || !qglBegin)
        return;

    qglEnable(GL_BLEND);
    qglDisable(GL_TEXTURE_2D);
    qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    qglColor4f(r, g, b, a);
    qglBegin(GL_QUADS);
    qglVertex3f(0, 0, 0);
    qglVertex3f((float)g_display.width, 0, 0);
    qglVertex3f((float)g_display.width, (float)g_display.height, 0);
    qglVertex3f(0, (float)g_display.height, 0);
    qglEnd();
    qglColor4f(1, 1, 1, 1);
    qglEnable(GL_TEXTURE_2D);
    qglDisable(GL_BLEND);
}

void R_DrawStretchRaw(int x, int y, int w, int h, int cols, int rows, byte *data)
{
    (void)x; (void)y; (void)w; (void)h;
    (void)cols; (void)rows; (void)data;
    /* TODO: Used for cinematic playback */
}

/* Quake II standard video modes + modern additions */
typedef struct {
    int     width;
    int     height;
} vidmode_t;

static vidmode_t vid_modes[] = {
    { 320,  240  },     /* 0 */
    { 400,  300  },     /* 1 */
    { 512,  384  },     /* 2 */
    { 640,  480  },     /* 3 */
    { 800,  600  },     /* 4 */
    { 960,  720  },     /* 5 */
    { 1024, 768  },     /* 6 (default) */
    { 1152, 864  },     /* 7 */
    { 1280, 960  },     /* 8 */
    { 1280, 720  },     /* 9 - 720p */
    { 1280, 1024 },     /* 10 */
    { 1600, 1200 },     /* 11 */
    { 1920, 1080 },     /* 12 - 1080p */
    { 2560, 1440 },     /* 13 - 1440p */
    { 3840, 2160 },     /* 14 - 4K */
};

#define NUM_VID_MODES (sizeof(vid_modes) / sizeof(vid_modes[0]))

void R_GetModeSize(int mode, int *w, int *h)
{
    if (mode < 0 || mode >= (int)NUM_VID_MODES) {
        /* Default to 1024x768 */
        *w = 1024;
        *h = 768;
        return;
    }
    *w = vid_modes[mode].width;
    *h = vid_modes[mode].height;
}

void R_SetMode(void)
{
    int w, h;
    int fs = r_fullscreen ? (int)r_fullscreen->value : 0;

    R_GetModeSize((int)r_mode->value, &w, &h);

    /* Resize window */
    if (g_display.window) {
        if (fs) {
            SDL_SetWindowFullscreen(g_display.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        } else {
            SDL_SetWindowFullscreen(g_display.window, 0);
            SDL_SetWindowSize(g_display.window, w, h);
            SDL_SetWindowPosition(g_display.window,
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    }

    g_display.width = w;
    g_display.height = h;
    g_display.fullscreen = fs;

    /* Update GL viewport */
    if (qglViewport)
        qglViewport(0, 0, w, h);

    Com_Printf("Video mode: %dx%d %s\n", w, h, fs ? "fullscreen" : "windowed");
}
