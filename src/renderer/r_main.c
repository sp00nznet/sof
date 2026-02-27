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

/* Fog */
cvar_t  *gl_fog;
cvar_t  *gl_fogdensity;
cvar_t  *gl_fogcolor_r;
cvar_t  *gl_fogcolor_g;
cvar_t  *gl_fogcolor_b;

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
void (APIENTRY *qglFogi)(GLenum pname, GLint param);
void (APIENTRY *qglFogf)(GLenum pname, GLfloat param);
void (APIENTRY *qglFogfv)(GLenum pname, const GLfloat *params);
void (APIENTRY *qglHint)(GLenum target, GLenum mode);
void (APIENTRY *qglReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
void (APIENTRY *qglPixelStorei)(GLenum pname, GLint param);
void (APIENTRY *qglPolygonOffset)(GLfloat factor, GLfloat units);

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
    QGL_LOAD(Fogi);
    QGL_LOAD(Fogf);
    QGL_LOAD(Fogfv);
    QGL_LOAD(Hint);
    QGL_LOAD(ReadPixels);
    QGL_LOAD(PixelStorei);
    QGL_LOAD(PolygonOffset);

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

    /* Fog cvars */
    gl_fog = Cvar_Get("gl_fog", "0", CVAR_ARCHIVE);
    gl_fogdensity = Cvar_Get("gl_fogdensity", "0.001", CVAR_ARCHIVE);
    gl_fogcolor_r = Cvar_Get("gl_fogcolor_r", "0.5", CVAR_ARCHIVE);
    gl_fogcolor_g = Cvar_Get("gl_fogcolor_g", "0.5", CVAR_ARCHIVE);
    gl_fogcolor_b = Cvar_Get("gl_fogcolor_b", "0.5", CVAR_ARCHIVE);

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
static void R_DrawDecals(void);
static void R_UpdateDlights(void);
static void R_UpdateSprites(float frametime);
static void R_DrawSprites(void);
static model_t *R_FindModel(const char *name);

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

    /* Update particles, sprites, and dynamic lights */
    R_UpdateParticles(frametime);
    R_UpdateSprites(frametime);
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

/*
 * Draw a colored wireframe box at an entity's position
 * Used as placeholder rendering for non-BSP entities (monsters, items, etc.)
 */
static void R_DrawEntityBox(vec3_t origin, vec3_t mins, vec3_t maxs,
                            float r, float g, float b)
{
    vec3_t p1, p2;

    VectorAdd(origin, mins, p1);
    VectorAdd(origin, maxs, p2);

    qglDisable(GL_TEXTURE_2D);
    qglColor4f(r, g, b, 0.8f);
    qglEnable(GL_BLEND);
    qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    qglBegin(GL_LINE_STRIP);
    /* Bottom face */
    qglVertex3f(p1[0], p1[1], p1[2]);
    qglVertex3f(p2[0], p1[1], p1[2]);
    qglVertex3f(p2[0], p2[1], p1[2]);
    qglVertex3f(p1[0], p2[1], p1[2]);
    qglVertex3f(p1[0], p1[1], p1[2]);
    qglEnd();

    qglBegin(GL_LINE_STRIP);
    /* Top face */
    qglVertex3f(p1[0], p1[1], p2[2]);
    qglVertex3f(p2[0], p1[1], p2[2]);
    qglVertex3f(p2[0], p2[1], p2[2]);
    qglVertex3f(p1[0], p2[1], p2[2]);
    qglVertex3f(p1[0], p1[1], p2[2]);
    qglEnd();

    qglBegin(GL_LINES);
    /* Vertical edges */
    qglVertex3f(p1[0], p1[1], p1[2]); qglVertex3f(p1[0], p1[1], p2[2]);
    qglVertex3f(p2[0], p1[1], p1[2]); qglVertex3f(p2[0], p1[1], p2[2]);
    qglVertex3f(p2[0], p2[1], p1[2]); qglVertex3f(p2[0], p2[1], p2[2]);
    qglVertex3f(p1[0], p2[1], p1[2]); qglVertex3f(p1[0], p2[1], p2[2]);
    qglEnd();

    qglDisable(GL_BLEND);
    qglEnable(GL_TEXTURE_2D);
    qglColor4f(1, 1, 1, 1);
}

/*
 * R_DrawSolidBox — Helper to draw a solid colored box
 */
static void R_DrawSolidBox(vec3_t mins, vec3_t maxs, float r, float g, float b, float a)
{
    float x0 = mins[0], y0 = mins[1], z0 = mins[2];
    float x1 = maxs[0], y1 = maxs[1], z1 = maxs[2];

    /* Slightly darker color for shading */
    float rd = r * 0.6f, gd = g * 0.6f, bd = b * 0.6f;

    qglBegin(GL_QUADS);
    /* Front face (+Y) */
    qglColor4f(r, g, b, a);
    qglVertex3f(x0, y1, z0); qglVertex3f(x1, y1, z0);
    qglVertex3f(x1, y1, z1); qglVertex3f(x0, y1, z1);
    /* Back face (-Y) */
    qglVertex3f(x1, y0, z0); qglVertex3f(x0, y0, z0);
    qglVertex3f(x0, y0, z1); qglVertex3f(x1, y0, z1);
    /* Top face (+Z) */
    qglColor4f(r, g, b, a);
    qglVertex3f(x0, y0, z1); qglVertex3f(x0, y1, z1);
    qglVertex3f(x1, y1, z1); qglVertex3f(x1, y0, z1);
    /* Bottom face (-Z) */
    qglColor4f(rd, gd, bd, a);
    qglVertex3f(x0, y1, z0); qglVertex3f(x0, y0, z0);
    qglVertex3f(x1, y0, z0); qglVertex3f(x1, y1, z0);
    /* Right face (+X) */
    qglColor4f(rd, gd, bd, a);
    qglVertex3f(x1, y0, z0); qglVertex3f(x1, y1, z0);
    qglVertex3f(x1, y1, z1); qglVertex3f(x1, y0, z1);
    /* Left face (-X) */
    qglVertex3f(x0, y1, z0); qglVertex3f(x0, y0, z0);
    qglVertex3f(x0, y0, z1); qglVertex3f(x0, y1, z1);
    qglEnd();
}

/*
 * R_DrawHumanoid — Draw a simple humanoid figure at the given position
 * Draws: head, torso, 2 arms, 2 legs as solid colored boxes
 * yaw: rotation angle in degrees
 * deadflag: if nonzero, draw fallen/ragdoll pose
 */
static void R_DrawHumanoid(vec3_t origin, float yaw, float r, float g, float b,
                            int deadflag, int health)
{
    vec3_t bmin, bmax;

    qglDisable(GL_TEXTURE_2D);
    qglEnable(GL_BLEND);
    qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    qglEnable(GL_DEPTH_TEST);

    qglPushMatrix();
    qglTranslatef(origin[0], origin[1], origin[2]);
    qglRotatef(yaw, 0, 0, 1);

    if (deadflag) {
        /* Dead: flat on the ground */
        VectorSet(bmin, -16, -6, 0);
        VectorSet(bmax, 16, 6, 4);
        R_DrawSolidBox(bmin, bmax, r * 0.5f, g * 0.5f, b * 0.5f, 0.7f);
    } else {
        /* Torso */
        VectorSet(bmin, -6, -4, 24);
        VectorSet(bmax, 6, 4, 48);
        R_DrawSolidBox(bmin, bmax, r, g, b, 0.9f);

        /* Head */
        VectorSet(bmin, -4, -4, 48);
        VectorSet(bmax, 4, 4, 58);
        R_DrawSolidBox(bmin, bmax, r * 0.9f, g * 0.9f + 0.1f, b * 0.9f + 0.1f, 0.9f);

        /* Left arm */
        VectorSet(bmin, -9, -2, 28);
        VectorSet(bmax, -6, 2, 48);
        R_DrawSolidBox(bmin, bmax, r * 0.8f, g * 0.8f, b * 0.8f, 0.9f);

        /* Right arm */
        VectorSet(bmin, 6, -2, 28);
        VectorSet(bmax, 9, 2, 48);
        R_DrawSolidBox(bmin, bmax, r * 0.8f, g * 0.8f, b * 0.8f, 0.9f);

        /* Left leg */
        VectorSet(bmin, -5, -3, 0);
        VectorSet(bmax, -1, 3, 24);
        R_DrawSolidBox(bmin, bmax, r * 0.7f, g * 0.7f, b * 0.7f, 0.9f);

        /* Right leg */
        VectorSet(bmin, 1, -3, 0);
        VectorSet(bmax, 5, 3, 24);
        R_DrawSolidBox(bmin, bmax, r * 0.7f, g * 0.7f, b * 0.7f, 0.9f);

        /* Health bar above head (if injured) */
        if (health > 0 && health < 100) {
            float bar_w = 12.0f * ((float)health / 100.0f);
            float hr = (health < 30) ? 1.0f : 0.0f;
            float hg = (health >= 30) ? 1.0f : 0.0f;
            VectorSet(bmin, -6, -0.5f, 62);
            VectorSet(bmax, -6 + bar_w, 0.5f, 63);
            R_DrawSolidBox(bmin, bmax, hr, hg, 0.0f, 0.8f);
        }
    }

    qglPopMatrix();

    qglDisable(GL_BLEND);
    qglEnable(GL_TEXTURE_2D);
    qglColor4f(1, 1, 1, 1);
}

/* ==========================================================================
   Entity Interpolation

   Tracks previous entity positions and lerps between old and current
   for smooth visual movement at frame rates higher than 10Hz tick.
   ========================================================================== */

#define MAX_INTERP_ENTS     256

typedef struct {
    int         index;          /* entity index */
    vec3_t      prev_origin;    /* position at last server tick */
    vec3_t      prev_angles;    /* angles at last server tick */
    qboolean    valid;          /* has valid previous state */
} interp_ent_t;

static interp_ent_t    interp_ents[MAX_INTERP_ENTS];
static int             interp_count;
static float           interp_frac;     /* 0..1 fractional server tick */

void R_SetInterpFraction(float frac)
{
    interp_frac = frac;
}

void R_UpdateEntityInterp(void)
{
    game_export_t *ge = SV_GetGameExport();
    int i;

    interp_count = 0;
    if (!ge || !ge->edicts)
        return;

    for (i = 1; i < ge->num_edicts && interp_count < MAX_INTERP_ENTS; i++) {
        edict_t *ent = (edict_t *)((byte *)ge->edicts + i * ge->edict_size);

        if (!ent->inuse)
            continue;

        /* Only interpolate moving entities */
        if (ent->velocity[0] != 0 || ent->velocity[1] != 0 || ent->velocity[2] != 0 ||
            ent->avelocity[0] != 0 || ent->avelocity[1] != 0 || ent->avelocity[2] != 0) {
            interp_ent_t *ie = &interp_ents[interp_count++];
            ie->index = i;
            VectorCopy(ent->s.origin, ie->prev_origin);
            VectorCopy(ent->s.angles, ie->prev_angles);
            ie->valid = qtrue;
        }
    }
}

static qboolean R_GetInterpOrigin(int index, vec3_t out_origin, vec3_t cur_origin,
                                  vec3_t out_angles, vec3_t cur_angles)
{
    int i;
    for (i = 0; i < interp_count; i++) {
        if (interp_ents[i].index == index && interp_ents[i].valid) {
            out_origin[0] = interp_ents[i].prev_origin[0] +
                            (cur_origin[0] - interp_ents[i].prev_origin[0]) * interp_frac;
            out_origin[1] = interp_ents[i].prev_origin[1] +
                            (cur_origin[1] - interp_ents[i].prev_origin[1]) * interp_frac;
            out_origin[2] = interp_ents[i].prev_origin[2] +
                            (cur_origin[2] - interp_ents[i].prev_origin[2]) * interp_frac;

            out_angles[0] = interp_ents[i].prev_angles[0] +
                            (cur_angles[0] - interp_ents[i].prev_angles[0]) * interp_frac;
            out_angles[1] = interp_ents[i].prev_angles[1] +
                            (cur_angles[1] - interp_ents[i].prev_angles[1]) * interp_frac;
            out_angles[2] = interp_ents[i].prev_angles[2] +
                            (cur_angles[2] - interp_ents[i].prev_angles[2]) * interp_frac;
            return qtrue;
        }
    }
    return qfalse;
}

static void R_DrawBrushEntities(void)
{
    game_export_t *ge = SV_GetGameExport();
    int i;

    if (!ge || !ge->edicts || ge->num_edicts <= 0)
        return;

    for (i = 1; i < ge->num_edicts; i++) {
        edict_t *ent = (edict_t *)((byte *)ge->edicts + i * ge->edict_size);
        const char *model_name;
        vec3_t render_origin, render_angles;

        if (!ent->inuse)
            continue;

        /* Get interpolated position if available, otherwise use current */
        if (!R_GetInterpOrigin(i, render_origin, ent->s.origin,
                               render_angles, ent->s.angles)) {
            VectorCopy(ent->s.origin, render_origin);
            VectorCopy(ent->s.angles, render_angles);
        }

        /* Entities with models */
        if (ent->s.modelindex > 0) {
            model_name = SV_GetConfigstring(CS_MODELS + ent->s.modelindex);
            if (model_name && model_name[0] == '*') {
                /* Inline BSP model (doors, platforms, etc.) */
                R_DrawBrushModel(atoi(model_name + 1), render_origin, render_angles);
                continue;
            }
            /* Check for loaded MD2 model */
            if (model_name && model_name[0]) {
                model_t *mod = R_FindModel(model_name);
                if (mod && mod->md2) {
                    float r_c = 0.8f, g_c = 0.8f, b_c = 0.8f;
                    R_DrawAliasModel(mod, render_origin, render_angles,
                                     ent->s.frame, ent->s.frame, 0.0f,
                                     r_c, g_c, b_c);
                    continue;
                }
            }
        }

        /* Non-BSP entities — draw placeholder geometry */
        if (ent->solid != SOLID_NOT && ent->svflags != SVF_NOCLIENT) {
            if (ent->solid == SOLID_TRIGGER)
                continue;   /* don't draw triggers */

            /* Humanoid rendering for monsters and player */
            if ((ent->svflags & SVF_MONSTER) || ent->client) {
                float r_c, g_c, b_c;
                if (ent->svflags & SVF_MONSTER) {
                    r_c = 0.7f; g_c = 0.15f; b_c = 0.1f;  /* dark red = monster */
                } else {
                    r_c = 0.1f; g_c = 0.6f; b_c = 0.1f;   /* green = player */
                }
                R_DrawHumanoid(render_origin, render_angles[1],
                               r_c, g_c, b_c, ent->deadflag, ent->health);
            } else {
                /* Generic entity — yellow wireframe box */
                if (ent->mins[0] != 0 || ent->maxs[0] != 0 ||
                    ent->mins[2] != 0 || ent->maxs[2] != 0) {
                    R_DrawEntityBox(render_origin, ent->mins, ent->maxs,
                                    1.0f, 1.0f, 0.0f);
                }
            }
        }
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

/* ==========================================================================
   First-Person View Weapon Model

   Draws simple 3D geometry for the currently held weapon in view space.
   Uses a separate projection with narrow depth range to prevent
   clipping into world geometry.
   ========================================================================== */

/* View weapon state — set by engine before rendering */
static int   vw_weapon_id;          /* current weapon enum value */
static float vw_kick;               /* recoil kick (0..1, decays) */
static float vw_bob_phase;          /* head bob cycle phase */
static float vw_bob_amount;         /* head bob amplitude */
static float vw_sway_yaw;           /* weapon sway offset (yaw lag) */
static float vw_sway_pitch;         /* weapon sway offset (pitch lag) */
static qboolean vw_reloading;       /* currently reloading */

/* Weapon switch animation state */
static int   vw_prev_weapon;        /* previous weapon (for switch detect) */
static float vw_switch_frac;        /* 0=normal, 1=fully lowered */
static int   vw_switch_phase;       /* 0=idle, 1=lowering, 2=raising */
static int   vw_switch_target;      /* weapon to switch to */

void R_SetViewWeaponState(int weapon_id, float kick, float bob_phase,
                          float bob_amount, float sway_yaw, float sway_pitch,
                          qboolean reloading)
{
    /* Detect weapon switch */
    if (weapon_id != vw_prev_weapon && vw_prev_weapon > 0 && weapon_id > 0) {
        if (vw_switch_phase == 0) {
            /* Start lowering the current weapon */
            vw_switch_phase = 1;
            vw_switch_frac = 0;
            vw_switch_target = weapon_id;
            /* Don't change weapon_id yet — keep showing old weapon while lowering */
            weapon_id = vw_prev_weapon;
        }
    }

    if (vw_switch_phase == 0)
        vw_prev_weapon = weapon_id;

    vw_weapon_id = weapon_id;
    vw_kick = kick;
    vw_bob_phase = bob_phase;
    vw_bob_amount = bob_amount;
    vw_sway_yaw = sway_yaw;
    vw_sway_pitch = sway_pitch;
    vw_reloading = reloading;
}

/*
 * Draw a simple box shape (used as building block for weapon geometry)
 */
static void VW_DrawBox(float x1, float y1, float z1,
                       float x2, float y2, float z2,
                       float r, float g, float b)
{
    /* Front face */
    qglColor4f(r, g, b, 1.0f);
    qglNormal3f(0, 0, 1);
    qglVertex3f(x1, y1, z2);
    qglVertex3f(x2, y1, z2);
    qglVertex3f(x2, y2, z2);
    qglVertex3f(x1, y2, z2);

    /* Back face */
    qglColor4f(r * 0.6f, g * 0.6f, b * 0.6f, 1.0f);
    qglNormal3f(0, 0, -1);
    qglVertex3f(x2, y1, z1);
    qglVertex3f(x1, y1, z1);
    qglVertex3f(x1, y2, z1);
    qglVertex3f(x2, y2, z1);

    /* Top face */
    qglColor4f(r * 0.9f, g * 0.9f, b * 0.9f, 1.0f);
    qglNormal3f(0, 1, 0);
    qglVertex3f(x1, y2, z1);
    qglVertex3f(x1, y2, z2);
    qglVertex3f(x2, y2, z2);
    qglVertex3f(x2, y2, z1);

    /* Bottom face */
    qglColor4f(r * 0.5f, g * 0.5f, b * 0.5f, 1.0f);
    qglNormal3f(0, -1, 0);
    qglVertex3f(x1, y1, z1);
    qglVertex3f(x2, y1, z1);
    qglVertex3f(x2, y1, z2);
    qglVertex3f(x1, y1, z2);

    /* Right face */
    qglColor4f(r * 0.75f, g * 0.75f, b * 0.75f, 1.0f);
    qglNormal3f(1, 0, 0);
    qglVertex3f(x2, y1, z1);
    qglVertex3f(x2, y1, z2);
    qglVertex3f(x2, y2, z2);
    qglVertex3f(x2, y2, z1);

    /* Left face */
    qglColor4f(r * 0.7f, g * 0.7f, b * 0.7f, 1.0f);
    qglNormal3f(-1, 0, 0);
    qglVertex3f(x1, y1, z2);
    qglVertex3f(x1, y1, z1);
    qglVertex3f(x1, y2, z1);
    qglVertex3f(x1, y2, z2);
}

/*
 * Draw weapon-specific geometry in view space.
 * Coordinate system: X = right, Y = up, Z = forward (into screen).
 */
static void VW_DrawWeaponGeometry(int weapon_id)
{
    qglBegin(GL_QUADS);

    switch (weapon_id) {
    case 1: /* WEAP_KNIFE */
        /* Blade */
        VW_DrawBox(-0.3f, -0.5f, 3.0f,  0.3f, 0.1f, 7.0f,
                   0.75f, 0.75f, 0.8f);
        /* Handle */
        VW_DrawBox(-0.4f, -2.5f, 3.5f,  0.4f, -0.5f, 5.0f,
                   0.35f, 0.2f, 0.1f);
        /* Guard */
        VW_DrawBox(-0.6f, -0.6f, 3.2f,  0.6f, 0.3f, 3.6f,
                   0.5f, 0.5f, 0.55f);
        break;

    case 2: /* WEAP_PISTOL1 (.44 Desert Eagle) */
        /* Slide */
        VW_DrawBox(-0.5f, 0.0f, 2.0f,  0.5f, 1.0f, 7.5f,
                   0.55f, 0.55f, 0.55f);
        /* Barrel */
        VW_DrawBox(-0.25f, 0.3f, 7.5f,  0.25f, 0.7f, 9.0f,
                   0.4f, 0.4f, 0.42f);
        /* Grip */
        VW_DrawBox(-0.5f, -2.5f, 2.5f,  0.5f, 0.0f, 5.0f,
                   0.3f, 0.25f, 0.15f);
        /* Trigger guard */
        VW_DrawBox(-0.3f, -1.0f, 5.0f,  0.3f, -0.5f, 6.0f,
                   0.45f, 0.45f, 0.45f);
        break;

    case 3: /* WEAP_PISTOL2 (Silver Talon) */
        /* Slide */
        VW_DrawBox(-0.4f, 0.0f, 2.0f,  0.4f, 0.8f, 7.0f,
                   0.7f, 0.7f, 0.72f);
        /* Barrel */
        VW_DrawBox(-0.2f, 0.2f, 7.0f,  0.2f, 0.6f, 8.5f,
                   0.6f, 0.6f, 0.62f);
        /* Grip */
        VW_DrawBox(-0.4f, -2.2f, 2.5f,  0.4f, 0.0f, 4.8f,
                   0.25f, 0.2f, 0.15f);
        break;

    case 4: /* WEAP_SHOTGUN */
        /* Barrel */
        VW_DrawBox(-0.35f, -0.1f, 0.0f,  0.35f, 0.5f, 12.0f,
                   0.3f, 0.3f, 0.32f);
        /* Pump */
        VW_DrawBox(-0.4f, -0.5f, 4.0f,  0.4f, -0.1f, 8.0f,
                   0.5f, 0.35f, 0.15f);
        /* Stock */
        VW_DrawBox(-0.5f, -1.5f, -2.0f,  0.5f, 0.3f, 1.0f,
                   0.35f, 0.25f, 0.12f);
        /* Receiver */
        VW_DrawBox(-0.5f, -0.5f, 0.0f,  0.5f, 0.8f, 3.0f,
                   0.35f, 0.35f, 0.35f);
        break;

    case 5: /* WEAP_MACHINEGUN (MP5) */
        /* Body */
        VW_DrawBox(-0.4f, -0.3f, 0.0f,  0.4f, 0.5f, 10.0f,
                   0.25f, 0.25f, 0.27f);
        /* Magazine */
        VW_DrawBox(-0.25f, -2.0f, 3.0f,  0.25f, -0.3f, 5.0f,
                   0.2f, 0.2f, 0.22f);
        /* Stock */
        VW_DrawBox(-0.3f, -0.5f, -3.0f,  0.3f, 0.3f, 0.0f,
                   0.22f, 0.22f, 0.24f);
        /* Barrel */
        VW_DrawBox(-0.2f, 0.1f, 10.0f,  0.2f, 0.4f, 12.0f,
                   0.3f, 0.3f, 0.32f);
        break;

    case 6: /* WEAP_ASSAULT (M4) */
        /* Body/receiver */
        VW_DrawBox(-0.4f, -0.3f, 0.0f,  0.4f, 0.6f, 8.0f,
                   0.35f, 0.35f, 0.3f);
        /* Barrel + handguard */
        VW_DrawBox(-0.3f, 0.0f, 8.0f,  0.3f, 0.4f, 14.0f,
                   0.3f, 0.3f, 0.32f);
        /* Magazine */
        VW_DrawBox(-0.2f, -2.5f, 3.0f,  0.2f, -0.3f, 5.5f,
                   0.2f, 0.2f, 0.2f);
        /* Stock */
        VW_DrawBox(-0.35f, -0.3f, -4.0f,  0.35f, 0.5f, 0.0f,
                   0.3f, 0.25f, 0.15f);
        /* Carry handle / sight */
        VW_DrawBox(-0.15f, 0.6f, 2.0f,  0.15f, 1.0f, 6.0f,
                   0.3f, 0.3f, 0.3f);
        break;

    case 7: /* WEAP_SNIPER (MSG90) */
        /* Long barrel */
        VW_DrawBox(-0.3f, -0.1f, 0.0f,  0.3f, 0.4f, 16.0f,
                   0.3f, 0.3f, 0.28f);
        /* Scope */
        VW_DrawBox(-0.2f, 0.6f, 4.0f,  0.2f, 1.2f, 10.0f,
                   0.15f, 0.15f, 0.17f);
        /* Magazine */
        VW_DrawBox(-0.2f, -1.8f, 5.0f,  0.2f, -0.1f, 7.0f,
                   0.2f, 0.2f, 0.2f);
        /* Stock */
        VW_DrawBox(-0.4f, -0.5f, -5.0f,  0.4f, 0.4f, 0.0f,
                   0.3f, 0.25f, 0.12f);
        break;

    case 8: /* WEAP_SLUGGER */
        /* Drum body */
        VW_DrawBox(-0.6f, -0.8f, 1.0f,  0.6f, 0.8f, 6.0f,
                   0.4f, 0.4f, 0.35f);
        /* Barrel */
        VW_DrawBox(-0.3f, 0.0f, 6.0f,  0.3f, 0.5f, 11.0f,
                   0.35f, 0.35f, 0.37f);
        /* Handle */
        VW_DrawBox(-0.4f, -2.0f, 2.0f,  0.4f, -0.8f, 5.0f,
                   0.3f, 0.2f, 0.12f);
        break;

    case 9: /* WEAP_ROCKET (M202A2) */
        /* Quad tube launcher */
        VW_DrawBox(-0.8f, -0.4f, 0.0f,  0.8f, 0.8f, 12.0f,
                   0.3f, 0.35f, 0.25f);
        /* Handle */
        VW_DrawBox(-0.3f, -2.0f, 3.0f,  0.3f, -0.4f, 6.0f,
                   0.2f, 0.2f, 0.15f);
        /* Sight */
        VW_DrawBox(-0.1f, 0.8f, 3.0f,  0.1f, 1.3f, 5.0f,
                   0.25f, 0.25f, 0.25f);
        break;

    case 10: /* WEAP_FLAMEGUN */
        /* Tank body */
        VW_DrawBox(-0.5f, -0.5f, -1.0f,  0.5f, 0.7f, 6.0f,
                   0.4f, 0.25f, 0.1f);
        /* Nozzle */
        VW_DrawBox(-0.25f, 0.0f, 6.0f,  0.25f, 0.4f, 10.0f,
                   0.3f, 0.3f, 0.32f);
        /* Handle */
        VW_DrawBox(-0.3f, -2.0f, 2.0f,  0.3f, -0.5f, 5.0f,
                   0.2f, 0.15f, 0.1f);
        /* Pilot light tip */
        VW_DrawBox(-0.1f, 0.1f, 10.0f,  0.1f, 0.3f, 10.5f,
                   1.0f, 0.6f, 0.1f);
        break;

    case 11: /* WEAP_MPG (Microwave Pulse Gun) */
        /* Sci-fi body */
        VW_DrawBox(-0.5f, -0.3f, 0.0f,  0.5f, 0.6f, 9.0f,
                   0.3f, 0.35f, 0.5f);
        /* Emitter dish */
        VW_DrawBox(-0.6f, -0.4f, 9.0f,  0.6f, 0.8f, 10.0f,
                   0.4f, 0.45f, 0.6f);
        /* Grip */
        VW_DrawBox(-0.3f, -2.0f, 2.0f,  0.3f, -0.3f, 5.0f,
                   0.25f, 0.25f, 0.3f);
        break;

    case 12: /* WEAP_MPISTOL (Machine Pistol) */
        /* Compact body */
        VW_DrawBox(-0.35f, 0.0f, 2.0f,  0.35f, 0.7f, 7.0f,
                   0.3f, 0.3f, 0.32f);
        /* Extended magazine */
        VW_DrawBox(-0.2f, -2.5f, 3.0f,  0.2f, 0.0f, 4.5f,
                   0.22f, 0.22f, 0.22f);
        /* Grip */
        VW_DrawBox(-0.35f, -2.0f, 4.5f,  0.35f, 0.0f, 6.0f,
                   0.25f, 0.2f, 0.12f);
        break;

    case 13: /* WEAP_GRENADE */
        /* Grenade body (oval-ish) */
        VW_DrawBox(-0.6f, -0.8f, 2.0f,  0.6f, 0.8f, 5.0f,
                   0.3f, 0.35f, 0.2f);
        /* Spoon/lever */
        VW_DrawBox(-0.1f, 0.8f, 2.5f,  0.1f, 1.3f, 4.5f,
                   0.5f, 0.5f, 0.45f);
        /* Pin ring */
        VW_DrawBox(0.3f, 1.0f, 2.0f,  0.5f, 1.4f, 2.5f,
                   0.6f, 0.6f, 0.55f);
        break;

    case 14: /* WEAP_C4 */
        /* C4 brick */
        VW_DrawBox(-0.8f, -0.4f, 2.0f,  0.8f, 0.4f, 5.0f,
                   0.6f, 0.55f, 0.3f);
        /* Detonator */
        VW_DrawBox(-0.15f, 0.4f, 3.0f,  0.15f, 0.8f, 3.5f,
                   0.4f, 0.4f, 0.4f);
        /* Wire */
        VW_DrawBox(-0.05f, 0.7f, 3.2f,  0.05f, 1.0f, 4.5f,
                   0.8f, 0.1f, 0.1f);
        break;

    case 15: /* WEAP_MEDKIT */
        /* Kit body */
        VW_DrawBox(-0.8f, -0.5f, 2.0f,  0.8f, 0.5f, 5.0f,
                   0.8f, 0.8f, 0.8f);
        /* Red cross (horizontal bar) */
        VW_DrawBox(-0.5f, -0.1f, 3.0f,  0.5f, 0.1f, 4.0f,
                   0.9f, 0.1f, 0.1f);
        /* Red cross (vertical bar) */
        VW_DrawBox(-0.1f, -0.4f, 3.0f,  0.1f, 0.4f, 4.0f,
                   0.9f, 0.1f, 0.1f);
        break;

    default: /* WEAP_NONE or unhandled — no weapon drawn */
        break;
    }

    qglEnd();
}

/*
 * R_DrawViewWeapon — Render first-person view weapon model
 *
 * Called after world rendering, before 2D overlay.
 * Uses a separate depth range so the weapon never clips into walls.
 */
static void R_DrawViewWeapon(refdef_t *fd)
{
    float bob_x, bob_y, kick_pitch, kick_back;
    float reload_angle;
    float switch_offset = 0;

    /* Advance weapon switch animation */
    if (vw_switch_phase == 1) {
        /* Lowering old weapon */
        vw_switch_frac += 0.06f;  /* ~17 frames to lower at 60fps */
        if (vw_switch_frac >= 1.0f) {
            vw_switch_frac = 1.0f;
            vw_switch_phase = 2;
            /* Now switch to the new weapon */
            vw_weapon_id = vw_switch_target;
            vw_prev_weapon = vw_switch_target;
        }
    } else if (vw_switch_phase == 2) {
        /* Raising new weapon */
        vw_switch_frac -= 0.05f;  /* slightly slower raise */
        if (vw_switch_frac <= 0) {
            vw_switch_frac = 0;
            vw_switch_phase = 0;
        }
    }
    switch_offset = vw_switch_frac * 12.0f;  /* drop weapon 12 units down */

    if (vw_weapon_id <= 0)
        return;     /* no weapon to draw */

    /* Save current matrices */
    qglMatrixMode(GL_PROJECTION);
    qglPushMatrix();
    qglMatrixMode(GL_MODELVIEW);
    qglPushMatrix();

    /* Set up view weapon projection (narrow FOV, near clip) */
    {
        float znear = 1.0f, zfar = 128.0f;
        float fov_x = 65.0f;   /* narrower than world for better feel */
        float aspect = (float)fd->width / (float)fd->height;
        float fov_y = fov_x / aspect;
        float ymax = znear * (float)tan(fov_y * 3.14159265 / 360.0);
        float xmax = znear * (float)tan(fov_x * 3.14159265 / 360.0);

        qglMatrixMode(GL_PROJECTION);
        qglLoadIdentity();
        qglFrustum(-xmax, xmax, -ymax, ymax, znear, zfar);
    }

    /* Clear depth so weapon always renders on top */
    qglClear(GL_DEPTH_BUFFER_BIT);
    qglEnable(GL_DEPTH_TEST);
    qglDepthFunc(GL_LEQUAL);
    qglDepthMask(GL_TRUE);

    /* Set up modelview in view space */
    qglMatrixMode(GL_MODELVIEW);
    qglLoadIdentity();

    /* Calculate bob offsets */
    bob_x = (float)sin(vw_bob_phase * 2.0f) * vw_bob_amount * 0.4f;
    bob_y = (float)sin(vw_bob_phase) * vw_bob_amount;

    /* Calculate kick offsets */
    kick_pitch = -vw_kick * 8.0f;   /* weapon tips up when firing */
    kick_back = vw_kick * 1.5f;     /* weapon pushes back toward camera */

    /* Reload tilt */
    reload_angle = vw_reloading ? 25.0f : 0.0f;

    /* Position weapon: right side, below center, forward */
    /* switch_offset lowers weapon during weapon switch animation */
    qglTranslatef(3.5f + bob_x + vw_sway_yaw,
                  -3.5f + bob_y + vw_sway_pitch - switch_offset,
                  -10.0f + kick_back);

    /* Apply weapon rotations */
    qglRotatef(kick_pitch, 1, 0, 0);      /* recoil pitch */
    qglRotatef(reload_angle, 0, 0, 1);    /* reload tilt */

    /* Disable textures — weapon is solid colored geometry */
    qglDisable(GL_TEXTURE_2D);
    qglEnable(GL_BLEND);
    qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Draw the weapon geometry */
    VW_DrawWeaponGeometry(vw_weapon_id);

    /* Muzzle flash on kick */
    if (vw_kick > 0.5f) {
        float flash = vw_kick - 0.5f;  /* 0..0.5 range */
        qglColor4f(1.0f, 0.8f, 0.3f, flash * 2.0f);
        qglBegin(GL_TRIANGLES);
        qglVertex3f(-0.3f, 0.3f, 14.0f);
        qglVertex3f(0.3f, 0.3f, 14.0f);
        qglVertex3f(0.0f, 0.5f, 16.0f);
        qglVertex3f(-0.2f, 0.1f, 14.0f);
        qglVertex3f(0.2f, 0.5f, 14.0f);
        qglVertex3f(0.0f, 0.3f, 15.5f);
        qglEnd();
    }

    /* Restore state */
    qglDisable(GL_BLEND);
    qglEnable(GL_TEXTURE_2D);
    qglColor4f(1, 1, 1, 1);

    /* Restore matrices */
    qglMatrixMode(GL_PROJECTION);
    qglPopMatrix();
    qglMatrixMode(GL_MODELVIEW);
    qglPopMatrix();
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

    /* GL fog setup */
    if (gl_fog && gl_fog->value > 0) {
        float fog_color[4];
        float density = gl_fogdensity ? gl_fogdensity->value : 0.001f;

        /* Parse fog color (default: grey) */
        fog_color[0] = gl_fogcolor_r ? gl_fogcolor_r->value : 0.5f;
        fog_color[1] = gl_fogcolor_g ? gl_fogcolor_g->value : 0.5f;
        fog_color[2] = gl_fogcolor_b ? gl_fogcolor_b->value : 0.5f;
        fog_color[3] = 1.0f;

        qglEnable(GL_FOG);
        qglFogi(GL_FOG_MODE, GL_EXP2);
        qglFogfv(GL_FOG_COLOR, fog_color);
        qglFogf(GL_FOG_DENSITY, density);
        qglHint(GL_FOG_HINT, GL_DONT_CARE);
    }

    /* Underwater fog when player is submerged */
    if (fd->rdflags & RDF_UNDERWATER) {
        float water_fog[4] = {0.1f, 0.2f, 0.4f, 1.0f};
        qglEnable(GL_FOG);
        qglFogi(GL_FOG_MODE, GL_EXP2);
        qglFogfv(GL_FOG_COLOR, water_fog);
        qglFogf(GL_FOG_DENSITY, 0.003f);
    }

    /* Draw skybox (before world, depth writes off so world occludes it) */
    R_DrawSkyBox();

    /* Draw BSP world */
    if (R_WorldLoaded() && r_drawworld->value)
        R_DrawWorld();

    /* Draw brush entities (inline BSP models) */
    if (r_drawentities->value)
        R_DrawBrushEntities();

    /* Draw decals on world surfaces */
    R_DrawDecals();

    /* Draw particles and sprites */
    R_DrawParticles();
    R_DrawSprites();

    /* Draw dynamic lights */
    R_DrawDlights();

    /* Disable fog before 2D rendering */
    qglDisable(GL_FOG);

    /* Draw first-person view weapon (after world, before 2D) */
    R_DrawViewWeapon(fd);

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

    case 4: /* flame — orange/red, rises */
        spread = 20.0f; speed = 40.0f;
        for (i = 0; i < count; i++) {
            vel[0] = dir[0] * speed + ((float)(rand()%100) - 50) * spread / 50.0f;
            vel[1] = dir[1] * speed + ((float)(rand()%100) - 50) * spread / 50.0f;
            vel[2] = dir[2] * speed + 30.0f + (float)(rand()%40);
            VectorSet(accel, 0, 0, 40);  /* rises */
            R_AddParticle(org, vel, accel,
                          1.0f, 0.3f + (rand()%40)*0.01f, 0.0f, 1.0f,
                          2.0f, 0.6f);
        }
        break;

    case 5: /* shell casing — brass, ejects right and tumbles */
        for (i = 0; i < count; i++) {
            vel[0] = dir[0] * 80.0f + ((float)(rand()%60) - 30);
            vel[1] = dir[1] * 80.0f + ((float)(rand()%60) - 30);
            vel[2] = 50.0f + (float)(rand()%40);
            VectorSet(accel, 0, 0, -600);  /* heavy gravity */
            R_AddParticle(org, vel, accel,
                          0.85f, 0.7f, 0.3f, 1.0f,
                          0.5f, 1.5f);
        }
        break;

    case 6: /* water drip — blue drop falling down */
        for (i = 0; i < count; i++) {
            vec3_t drip_org;
            VectorCopy(org, drip_org);
            drip_org[0] += ((float)(rand()%20) - 10) * 0.5f;
            drip_org[1] += ((float)(rand()%20) - 10) * 0.5f;
            VectorSet(vel, 0, 0, -120.0f - (float)(rand()%40));
            VectorSet(accel, 0, 0, -300);
            R_AddParticle(drip_org, vel, accel,
                          0.3f, 0.4f, 0.8f, 0.8f,
                          1.2f, 0.8f);
        }
        break;

    case 7: /* steam — white/grey, rises and spreads */
        for (i = 0; i < count; i++) {
            vel[0] = dir[0] * 30.0f + ((float)(rand()%40) - 20);
            vel[1] = dir[1] * 30.0f + ((float)(rand()%40) - 20);
            vel[2] = 40.0f + (float)(rand()%30);
            VectorSet(accel, 0, 0, 20);  /* slight rise */
            R_AddParticle(org, vel, accel,
                          0.7f, 0.7f, 0.7f, 0.4f,
                          0.6f, 2.0f);
        }
        break;

    case 8: /* ambient spark shower — bright sparks falling/arcing */
        for (i = 0; i < count; i++) {
            vel[0] = ((float)(rand()%80) - 40);
            vel[1] = ((float)(rand()%80) - 40);
            vel[2] = -20.0f + (float)(rand()%60);
            VectorSet(accel, 0, 0, -500);
            R_AddParticle(org, vel, accel,
                          1.0f, 0.9f, 0.5f, 1.0f,
                          4.0f, 0.4f);
        }
        break;

    case 9: /* dust motes — slow floating particles */
        for (i = 0; i < count; i++) {
            vec3_t dust_org;
            VectorCopy(org, dust_org);
            dust_org[0] += ((float)(rand()%100) - 50);
            dust_org[1] += ((float)(rand()%100) - 50);
            dust_org[2] += ((float)(rand()%60) - 30);
            vel[0] = ((float)(rand()%20) - 10) * 0.5f;
            vel[1] = ((float)(rand()%20) - 10) * 0.5f;
            vel[2] = ((float)(rand()%10) - 5) * 0.3f;
            VectorSet(accel, 0, 0, 0);
            R_AddParticle(dust_org, vel, accel,
                          0.6f, 0.55f, 0.45f, 0.15f,
                          0.1f, 5.0f);
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

/* ==========================================================================
   Billboard Sprite System

   Camera-facing textured quads for explosions, smoke, energy effects.
   Sprites always face the viewer regardless of world orientation.
   ========================================================================== */

#define MAX_SPRITES     128

typedef struct {
    vec3_t  origin;
    float   size;           /* half-size in world units */
    float   color[4];       /* RGBA */
    float   time;           /* remaining lifetime */
    float   total_time;     /* initial lifetime */
    float   rotation;       /* rotation angle (degrees) */
    float   rot_speed;      /* degrees per second */
    int     anim_frames;    /* total frames (1 = static) */
    int     current_frame;  /* for animated sprites */
    float   frame_time;     /* time per frame */
    float   frame_accum;    /* accumulator */
} r_sprite_t;

static r_sprite_t   r_sprites[MAX_SPRITES];
static int           r_num_sprites;

/*
 * R_AddSprite — Add a billboard sprite effect
 */
void R_AddSprite(vec3_t origin, float size, float r, float g, float b,
                 float alpha, float lifetime, float rotation_speed)
{
    r_sprite_t *s;

    if (r_num_sprites >= MAX_SPRITES)
        return;

    s = &r_sprites[r_num_sprites++];
    VectorCopy(origin, s->origin);
    s->size = size;
    s->color[0] = r;
    s->color[1] = g;
    s->color[2] = b;
    s->color[3] = alpha;
    s->time = lifetime;
    s->total_time = lifetime;
    s->rotation = (float)(rand() % 360);
    s->rot_speed = rotation_speed;
    s->anim_frames = 1;
    s->current_frame = 0;
    s->frame_time = 0;
    s->frame_accum = 0;
}

/*
 * R_UpdateSprites — Advance sprite animations and remove expired
 */
static void R_UpdateSprites(float frametime)
{
    int i;

    for (i = 0; i < r_num_sprites; ) {
        r_sprite_t *s = &r_sprites[i];
        s->time -= frametime;
        s->rotation += s->rot_speed * frametime;

        /* Fade out over last 30% of lifetime */
        {
            float life_frac = s->time / s->total_time;
            if (life_frac < 0.3f)
                s->color[3] *= 0.9f;
        }

        /* Rise upward for smoke-type sprites */
        s->origin[2] += frametime * 10.0f;

        if (s->time <= 0) {
            /* Remove by swapping with last */
            r_sprites[i] = r_sprites[--r_num_sprites];
        } else {
            i++;
        }
    }
}

/*
 * R_DrawSprites — Render all active sprites as camera-facing quads
 */
static void R_DrawSprites(void)
{
    int i;
    vec3_t cam_org, cam_ang;
    vec3_t up, right;
    float yaw_rad, pitch_rad;

    if (r_num_sprites == 0)
        return;

    /* Get camera vectors for billboard orientation */
    R_GetCameraOrigin(cam_org);
    R_GetCameraAngles(cam_ang);

    yaw_rad = cam_ang[1] * (3.14159265f / 180.0f);
    pitch_rad = cam_ang[0] * (3.14159265f / 180.0f);

    /* Camera right vector (perpendicular to view direction, in XY plane) */
    right[0] = -(float)sin(yaw_rad);
    right[1] = (float)cos(yaw_rad);
    right[2] = 0;

    /* Camera up vector (world up for sprites) */
    up[0] = 0;
    up[1] = 0;
    up[2] = 1;

    qglDisable(GL_TEXTURE_2D);
    qglEnable(GL_BLEND);
    qglBlendFunc(GL_SRC_ALPHA, GL_ONE);  /* additive blending */
    qglDepthMask(GL_FALSE);

    qglBegin(GL_QUADS);

    for (i = 0; i < r_num_sprites; i++) {
        r_sprite_t *s = &r_sprites[i];
        vec3_t p1, p2, p3, p4;

        qglColor4f(s->color[0], s->color[1], s->color[2], s->color[3]);

        /* Calculate quad corners */
        p1[0] = s->origin[0] - right[0] * s->size - up[0] * s->size;
        p1[1] = s->origin[1] - right[1] * s->size - up[1] * s->size;
        p1[2] = s->origin[2] - right[2] * s->size - up[2] * s->size;

        p2[0] = s->origin[0] + right[0] * s->size - up[0] * s->size;
        p2[1] = s->origin[1] + right[1] * s->size - up[1] * s->size;
        p2[2] = s->origin[2] + right[2] * s->size - up[2] * s->size;

        p3[0] = s->origin[0] + right[0] * s->size + up[0] * s->size;
        p3[1] = s->origin[1] + right[1] * s->size + up[1] * s->size;
        p3[2] = s->origin[2] + right[2] * s->size + up[2] * s->size;

        p4[0] = s->origin[0] - right[0] * s->size + up[0] * s->size;
        p4[1] = s->origin[1] - right[1] * s->size + up[1] * s->size;
        p4[2] = s->origin[2] - right[2] * s->size + up[2] * s->size;

        qglVertex3f(p1[0], p1[1], p1[2]);
        qglVertex3f(p2[0], p2[1], p2[2]);
        qglVertex3f(p3[0], p3[1], p3[2]);
        qglVertex3f(p4[0], p4[1], p4[2]);
    }

    qglEnd();

    qglDepthMask(GL_TRUE);
    qglDisable(GL_BLEND);
    qglEnable(GL_TEXTURE_2D);
    qglColor4f(1, 1, 1, 1);
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
   Decal System — Projected quads on world surfaces
   ========================================================================== */

#define MAX_R_DECALS    256
#define R_DECAL_LIFETIME 30.0f

typedef struct {
    vec3_t      origin;
    vec3_t      normal;
    float       spawn_time;
    int         type;       /* 0=bullet, 1=blood, 2=scorch */
    qboolean    active;
} r_decal_t;

static r_decal_t   r_decals[MAX_R_DECALS];
static int          r_decal_write;

void R_AddDecal(vec3_t origin, vec3_t normal, int type)
{
    r_decal_t *d = &r_decals[r_decal_write];

    VectorCopy(origin, d->origin);
    VectorCopy(normal, d->normal);
    d->spawn_time = (float)Sys_Milliseconds();
    d->type = type;
    d->active = qtrue;

    r_decal_write = (r_decal_write + 1) % MAX_R_DECALS;
}

static void R_DrawDecals(void)
{
    int i;
    float now = (float)Sys_Milliseconds();
    int active_count = 0;

    /* Count active decals */
    for (i = 0; i < MAX_R_DECALS; i++)
        if (r_decals[i].active) active_count++;
    if (active_count == 0) return;

    qglDisable(GL_TEXTURE_2D);
    qglEnable(GL_BLEND);
    qglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    qglEnable(GL_POLYGON_OFFSET_FILL);
    if (qglPolygonOffset) qglPolygonOffset(-1.0f, -1.0f);
    qglDepthMask(GL_FALSE);

    for (i = 0; i < MAX_R_DECALS; i++) {
        r_decal_t *d = &r_decals[i];
        float age, alpha, size;
        vec3_t right, up, p;

        if (!d->active) continue;

        age = (now - d->spawn_time) * 0.001f;
        if (age > R_DECAL_LIFETIME) {
            d->active = qfalse;
            continue;
        }

        /* Fade out over last 5 seconds */
        alpha = (age > R_DECAL_LIFETIME - 5.0f) ?
                (R_DECAL_LIFETIME - age) / 5.0f : 1.0f;

        /* Decal size and color by type */
        switch (d->type) {
        case 0: /* bullet hole — small dark circle */
            size = 2.0f;
            qglColor4f(0.15f, 0.12f, 0.10f, alpha * 0.8f);
            break;
        case 1: /* blood — reddish splat */
            size = 4.0f;
            qglColor4f(0.5f, 0.0f, 0.0f, alpha * 0.7f);
            break;
        case 2: /* scorch mark — large dark */
            size = 8.0f;
            qglColor4f(0.1f, 0.08f, 0.06f, alpha * 0.6f);
            break;
        default:
            size = 2.0f;
            qglColor4f(0.2f, 0.2f, 0.2f, alpha * 0.5f);
            break;
        }

        /* Build a quad perpendicular to the normal */
        /* Find two tangent vectors */
        if (d->normal[2] > 0.9f || d->normal[2] < -0.9f) {
            /* Normal is mostly Z — use X as reference */
            right[0] = size; right[1] = 0; right[2] = 0;
        } else {
            /* Cross with up to get right */
            right[0] = -d->normal[1] * size;
            right[1] = d->normal[0] * size;
            right[2] = 0;
            /* Normalize and scale */
            {
                float len = (float)sqrt(right[0]*right[0] + right[1]*right[1]);
                if (len > 0.001f) {
                    right[0] = right[0] / len * size;
                    right[1] = right[1] / len * size;
                }
            }
        }
        /* up = normal cross right */
        up[0] = d->normal[1]*right[2] - d->normal[2]*right[1];
        up[1] = d->normal[2]*right[0] - d->normal[0]*right[2];
        up[2] = d->normal[0]*right[1] - d->normal[1]*right[0];

        /* Draw quad slightly offset from surface */
        qglBegin(GL_QUADS);
        p[0] = d->origin[0] + d->normal[0]*0.1f - right[0] - up[0];
        p[1] = d->origin[1] + d->normal[1]*0.1f - right[1] - up[1];
        p[2] = d->origin[2] + d->normal[2]*0.1f - right[2] - up[2];
        qglVertex3f(p[0], p[1], p[2]);

        p[0] = d->origin[0] + d->normal[0]*0.1f + right[0] - up[0];
        p[1] = d->origin[1] + d->normal[1]*0.1f + right[1] - up[1];
        p[2] = d->origin[2] + d->normal[2]*0.1f + right[2] - up[2];
        qglVertex3f(p[0], p[1], p[2]);

        p[0] = d->origin[0] + d->normal[0]*0.1f + right[0] + up[0];
        p[1] = d->origin[1] + d->normal[1]*0.1f + right[1] + up[1];
        p[2] = d->origin[2] + d->normal[2]*0.1f + right[2] + up[2];
        qglVertex3f(p[0], p[1], p[2]);

        p[0] = d->origin[0] + d->normal[0]*0.1f - right[0] + up[0];
        p[1] = d->origin[1] + d->normal[1]*0.1f - right[1] + up[1];
        p[2] = d->origin[2] + d->normal[2]*0.1f - right[2] + up[2];
        qglVertex3f(p[0], p[1], p[2]);
        qglEnd();
    }

    qglDisable(GL_POLYGON_OFFSET_FILL);
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
        /* Attempt to load MD2 mesh data */
        if (!R_LoadMD2(mod, name)) {
            Com_DPrintf("R_RegisterModel: MD2 load failed for %s\n", name);
        }
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

/* ==========================================================================
   Screenshot
   ========================================================================== */

void R_Screenshot_f(void)
{
    byte *buffer;
    int w = g_display.width;
    int h = g_display.height;
    FILE *f;
    char filename[64];
    int i, j;
    static int screenshot_count = 0;

    if (!qglReadPixels) {
        Com_Printf("Screenshot: glReadPixels not available\n");
        return;
    }

    /* Find next available filename */
    snprintf(filename, sizeof(filename), "screenshot%04d.tga", screenshot_count++);

    buffer = (byte *)Z_Malloc(w * h * 3);
    if (!buffer) {
        Com_Printf("Screenshot: out of memory\n");
        return;
    }

    qglPixelStorei(GL_PACK_ALIGNMENT, 1);
    qglReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buffer);

    /* Write TGA file */
    f = fopen(filename, "wb");
    if (!f) {
        Com_Printf("Screenshot: couldn't write %s\n", filename);
        Z_Free(buffer);
        return;
    }

    /* TGA header */
    {
        byte tga_header[18];
        memset(tga_header, 0, 18);
        tga_header[2] = 2;     /* uncompressed true-color */
        tga_header[12] = w & 0xFF;
        tga_header[13] = (w >> 8) & 0xFF;
        tga_header[14] = h & 0xFF;
        tga_header[15] = (h >> 8) & 0xFF;
        tga_header[16] = 24;   /* bits per pixel */
        fwrite(tga_header, 1, 18, f);
    }

    /* Write pixel data (TGA is BGR bottom-to-top, OpenGL reads bottom-to-top) */
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            int idx = (j * w + i) * 3;
            byte bgr[3];
            bgr[0] = buffer[idx + 2]; /* B */
            bgr[1] = buffer[idx + 1]; /* G */
            bgr[2] = buffer[idx + 0]; /* R */
            fwrite(bgr, 1, 3, f);
        }
    }

    fclose(f);
    Z_Free(buffer);

    Com_Printf("Wrote %s (%dx%d)\n", filename, w, h);
}
