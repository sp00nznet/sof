/*
 * r_local.h - Renderer internal types and declarations
 *
 * Based on Quake II ref_gl (id Software GPL) with SoF extensions.
 * SoF's renderer uses OpenGL 1.1 loaded entirely via GetProcAddress
 * (336 core functions + extensions: ARB_multitexture, S3_s3tc,
 * EXT_compiled_vertex_array).
 *
 * Original: ref_gl.dll, image base 0x30000000, single GetRefAPI export
 * In the recomp, the renderer is statically linked into the main binary.
 */

#ifndef R_LOCAL_H
#define R_LOCAL_H

#include "../common/q_shared.h"
#include "../common/qcommon.h"
#include "../engine/win32_compat.h"
#include "r_bsp.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

/* ==========================================================================
   Renderer Configuration
   ========================================================================== */

#define MAX_DLIGHTS         32
#define MAX_ENTITIES        128
#define MAX_PARTICLES       4096
#define MAX_LIGHTSTYLES_R   256
#define TEXNUM_LIGHTMAPS    1024
#define MAX_GLTEXTURES      2048

/* ==========================================================================
   Image / Texture
   ========================================================================== */

typedef enum {
    it_skin,
    it_sprite,
    it_wall,
    it_pic,
    it_sky
} imagetype_t;

typedef struct image_s {
    char            name[MAX_QPATH];
    imagetype_t     type;
    int             width, height;
    int             upload_width, upload_height;
    GLuint          texnum;
    float           sl, tl, sh, th;     /* tex coords for sub-image (pics) */
    qboolean        scrap;
    qboolean        has_alpha;
    int             registration_sequence;
} image_t;

/* ==========================================================================
   GL State
   ========================================================================== */

typedef struct {
    /* Window */
    int         width, height;
    int         fullscreen;

    /* GL capabilities */
    const char  *vendor_string;
    const char  *renderer_string;
    const char  *version_string;
    const char  *extensions_string;

    /* Extension flags */
    qboolean    have_multitexture;
    qboolean    have_s3tc;
    qboolean    have_compiled_vertex_array;

    /* Active texture unit for multitexture */
    int         currenttextures[2];
    int         currenttmu;

    /* GL state tracking */
    float       inverse_intensity;
    int         texture_count;
} glstate_t;

typedef struct {
    float   inverse_intensity;
    qboolean fullscreen;

    int     prev_mode;

    int     lightmap_textures;

    int     currenttextures[2];
    int     currenttmu;

    unsigned char originalRedGammaTable[256];
    unsigned char originalGreenGammaTable[256];
    unsigned char originalBlueGammaTable[256];
} glconfig_t;

extern glstate_t    gl_state;
extern glconfig_t   gl_config;

/* ==========================================================================
   Renderer Cvars
   ========================================================================== */

extern cvar_t   *r_mode;
extern cvar_t   *r_fullscreen;
extern cvar_t   *r_drawworld;
extern cvar_t   *r_drawentities;
extern cvar_t   *r_speeds;
extern cvar_t   *r_novis;
extern cvar_t   *r_nocull;
extern cvar_t   *gl_texturemode;
extern cvar_t   *gl_modulate;

/* SoF-specific renderer cvars */
extern cvar_t   *ghl_specular;
extern cvar_t   *ghl_mip;

/* ==========================================================================
   Renderer API (matches refexport_t from sof_types.h)
   ========================================================================== */

/* Lifecycle */
int     R_Init(void *hinstance, void *hWnd);
void    R_Shutdown(void);

/* Frame rendering */
void    R_BeginFrame(float camera_separation);
void    R_RenderFrame(refdef_t *fd);
void    R_EndFrame(void);

/* Registration */
void    R_BeginRegistration(const char *map);
struct model_s *R_RegisterModel(const char *name);
struct image_s *R_RegisterSkin(const char *name);
image_t *R_RegisterPic(const char *name);
void    R_SetSky(const char *name, float rotate, vec3_t axis);
void    R_EndRegistration(void);

/* 2D Drawing */
void    R_DrawGetPicSize(int *w, int *h, const char *name);
void    R_DrawPic(int x, int y, const char *name);
void    R_DrawStretchPic(int x, int y, int w, int h, const char *name);
void    R_DrawChar(int x, int y, int ch);
void    R_DrawTileClear(int x, int y, int w, int h, const char *name);
void    R_DrawFill(int x, int y, int w, int h, int c);
void    R_DrawFadeScreen(void);
void    R_DrawStretchRaw(int x, int y, int w, int h, int cols, int rows, byte *data);

/* Video mode */
void    R_SetMode(void);

/* ==========================================================================
   GL Function Loading
   SoF originally loaded 336 GL 1.1 functions + extensions via GetProcAddress.
   We use SDL_GL_GetProcAddress through our platform layer.
   ========================================================================== */

/* Core GL functions we need (subset — add more as needed) */
extern void (APIENTRY *qglBegin)(GLenum mode);
extern void (APIENTRY *qglEnd)(void);
extern void (APIENTRY *qglVertex3f)(GLfloat x, GLfloat y, GLfloat z);
extern void (APIENTRY *qglTexCoord2f)(GLfloat s, GLfloat t);
extern void (APIENTRY *qglColor4f)(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
extern void (APIENTRY *qglNormal3f)(GLfloat nx, GLfloat ny, GLfloat nz);

extern void (APIENTRY *qglClear)(GLbitfield mask);
extern void (APIENTRY *qglClearColor)(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
extern void (APIENTRY *qglEnable)(GLenum cap);
extern void (APIENTRY *qglDisable)(GLenum cap);
extern void (APIENTRY *qglBlendFunc)(GLenum sfactor, GLenum dfactor);
extern void (APIENTRY *qglDepthFunc)(GLenum func);
extern void (APIENTRY *qglDepthMask)(GLboolean flag);
extern void (APIENTRY *qglViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
extern void (APIENTRY *qglScissor)(GLint x, GLint y, GLsizei width, GLsizei height);
extern void (APIENTRY *qglMatrixMode)(GLenum mode);
extern void (APIENTRY *qglLoadIdentity)(void);
extern void (APIENTRY *qglOrtho)(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
extern void (APIENTRY *qglFrustum)(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
extern void (APIENTRY *qglPushMatrix)(void);
extern void (APIENTRY *qglPopMatrix)(void);
extern void (APIENTRY *qglRotatef)(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
extern void (APIENTRY *qglTranslatef)(GLfloat x, GLfloat y, GLfloat z);

extern void (APIENTRY *qglBindTexture)(GLenum target, GLuint texture);
extern void (APIENTRY *qglGenTextures)(GLsizei n, GLuint *textures);
extern void (APIENTRY *qglDeleteTextures)(GLsizei n, const GLuint *textures);
extern void (APIENTRY *qglTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
extern void (APIENTRY *qglTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
extern void (APIENTRY *qglTexParameteri)(GLenum target, GLenum pname, GLint param);
extern void (APIENTRY *qglTexEnvi)(GLenum target, GLenum pname, GLint param);

extern void (APIENTRY *qglDrawArrays)(GLenum mode, GLint first, GLsizei count);
extern void (APIENTRY *qglDrawElements)(GLenum mode, GLsizei count, GLenum type, const void *indices);
extern void (APIENTRY *qglVertexPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
extern void (APIENTRY *qglTexCoordPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
extern void (APIENTRY *qglColorPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
extern void (APIENTRY *qglEnableClientState)(GLenum cap);
extern void (APIENTRY *qglDisableClientState)(GLenum cap);

extern GLenum (APIENTRY *qglGetError)(void);
extern const GLubyte *(APIENTRY *qglGetString)(GLenum name);
extern void (APIENTRY *qglGetIntegerv)(GLenum pname, GLint *params);
extern void (APIENTRY *qglGetFloatv)(GLenum pname, GLfloat *params);
extern void (APIENTRY *qglFinish)(void);
extern void (APIENTRY *qglFlush)(void);

/* ARB_multitexture constants */
#ifndef GL_TEXTURE0_ARB
#define GL_TEXTURE0_ARB     0x84C0
#define GL_TEXTURE1_ARB     0x84C1
#endif

/* Multitexture extension (ARB_multitexture) */
typedef void (APIENTRY *PFNGLACTIVETEXTUREARBPROC)(GLenum texture);
typedef void (APIENTRY *PFNGLCLIENTACTIVETEXTUREARBPROC)(GLenum texture);
extern PFNGLACTIVETEXTUREARBPROC        qglActiveTextureARB;
extern PFNGLCLIENTACTIVETEXTUREARBPROC  qglClientActiveTextureARB;

/* Load all GL function pointers */
qboolean QGL_Init(void);
void QGL_Shutdown(void);

/* Image/Texture system (r_image.c) */
void        R_InitImages(void);
void        R_ShutdownImages(void);
image_t    *R_FindImage(const char *name);
image_t    *R_FindPic(const char *name);
GLuint      R_GetNoTexture(void);
void        R_ImageBeginRegistration(void);
void        R_ImageEndRegistration(void);

/* Lightmap system (r_light.c) */
void        R_BuildLightmaps(bsp_world_t *world);
void        R_FreeLightmaps(void);
qboolean    R_GetFaceLightmapTC(int face_idx, float *vertex,
                                 bsp_texinfo_t *ti,
                                 float *out_s, float *out_t,
                                 GLuint *out_texnum);
GLuint      R_GetFaceLightmapTexture(int face_idx);

/* BSP surface rendering (r_surf.c) */
void        R_LoadWorldMap(const char *name);
qboolean    R_WorldLoaded(void);
void        R_DrawWorld(void);
void        R_RenderWorldView(void);
void        R_InitSurfCommands(void);

/* Camera */
void        R_GetCameraOrigin(vec3_t out);
void        R_SetCameraOrigin(vec3_t origin);
void        R_GetCameraAngles(vec3_t out);
void        R_SetCameraAngles(vec3_t angles);
void        R_UpdateCamera(float forward, float right, float up,
                           float mouse_dx, float mouse_dy, float frametime);

#endif /* R_LOCAL_H */
