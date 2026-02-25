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
        int w = 1024, h = 768;
        int fs = (int)r_fullscreen->value;

        /* TODO: Parse r_mode into width/height properly */
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

    Com_Printf("------- Renderer Initialized -------\n");

    return 1;  /* success */
}

/* ==========================================================================
   R_Shutdown
   ========================================================================== */

void R_Shutdown(void)
{
    QGL_Shutdown();
    Sys_DestroyWindow();
}

/* ==========================================================================
   Frame Rendering
   ========================================================================== */

void R_BeginFrame(float camera_separation)
{
    (void)camera_separation;

    if (!qglClear)
        return;

    /* Clear screen */
    qglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Set up 2D mode for HUD/menus */
    qglViewport(0, 0, g_display.width, g_display.height);
    qglMatrixMode(GL_PROJECTION);
    qglLoadIdentity();
    qglOrtho(0, g_display.width, g_display.height, 0, -99999, 99999);
    qglMatrixMode(GL_MODELVIEW);
    qglLoadIdentity();
}

void R_RenderFrame(refdef_t *fd)
{
    (void)fd;

    /* TODO: Full 3D rendering pipeline:
     * 1. Set up 3D projection from refdef
     * 2. R_DrawWorld (BSP rendering)
     * 3. R_DrawEntities (GHOUL models, brush models)
     * 4. R_DrawParticles
     * 5. R_DrawAlphaSurfaces
     * 6. R_Flash (damage flash, underwater tint)
     */
}

void R_EndFrame(void)
{
    /* Swap buffers */
    Sys_SwapBuffers();
}

/* ==========================================================================
   Registration Stubs
   ========================================================================== */

void R_BeginRegistration(const char *map)
{
    Com_Printf("R_BeginRegistration: %s\n", map);
    /* TODO: Load BSP, lightmaps, textures */
}

struct model_s *R_RegisterModel(const char *name)
{
    Com_DPrintf("R_RegisterModel: %s\n", name);
    /* TODO: Load model (BSP inline model, GHOUL model, or sprite) */
    return NULL;
}

struct image_s *R_RegisterSkin(const char *name)
{
    Com_DPrintf("R_RegisterSkin: %s\n", name);
    /* TODO: Load skin texture */
    return NULL;
}

image_t *R_RegisterPic(const char *name)
{
    Com_DPrintf("R_RegisterPic: %s\n", name);
    /* TODO: Load 2D pic for HUD/menus */
    return NULL;
}

void R_SetSky(const char *name, float rotate, vec3_t axis)
{
    (void)name; (void)rotate; (void)axis;
    /* TODO: Load skybox textures */
}

void R_EndRegistration(void)
{
    /* TODO: Free any images that weren't touched during registration */
}

/* ==========================================================================
   2D Drawing Stubs
   ========================================================================== */

void R_DrawGetPicSize(int *w, int *h, const char *name)
{
    (void)name;
    *w = 0;
    *h = 0;
}

void R_DrawPic(int x, int y, const char *name)
{
    (void)x; (void)y; (void)name;
}

void R_DrawStretchPic(int x, int y, int w, int h, const char *name)
{
    (void)x; (void)y; (void)w; (void)h; (void)name;
}

void R_DrawChar(int x, int y, int ch)
{
    (void)x; (void)y; (void)ch;
}

void R_DrawTileClear(int x, int y, int w, int h, const char *name)
{
    (void)x; (void)y; (void)w; (void)h; (void)name;
}

void R_DrawFill(int x, int y, int w, int h, int c)
{
    if (!qglDisable || !qglBegin)
        return;

    /* Convert Q2 palette index to RGB */
    /* For now, just use a debug color */
    byte r = (c & 0xE0);
    byte g = (c & 0x1C) << 3;
    byte b = (c & 0x03) << 6;

    qglDisable(GL_TEXTURE_2D);
    qglColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    qglBegin(GL_QUADS);
    qglVertex3f((float)x, (float)y, 0);
    qglVertex3f((float)(x + w), (float)y, 0);
    qglVertex3f((float)(x + w), (float)(y + h), 0);
    qglVertex3f((float)x, (float)(y + h), 0);
    qglEnd();
    qglEnable(GL_TEXTURE_2D);
    qglColor4f(1, 1, 1, 1);
}

void R_DrawFadeScreen(void)
{
    if (!qglEnable || !qglBegin)
        return;

    qglEnable(GL_BLEND);
    qglDisable(GL_TEXTURE_2D);
    qglColor4f(0, 0, 0, 0.8f);
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

void R_SetMode(void)
{
    /* TODO: Mode switching */
}
