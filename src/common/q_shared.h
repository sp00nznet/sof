/*
 * q_shared.h - Definitions shared between engine, game, renderer, and sound
 *
 * Based on Quake II GPL source (id Software) with SoF-specific extensions.
 * This header is included by every compilation unit in the project.
 */

#ifndef Q_SHARED_H
#define Q_SHARED_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <assert.h>

/* ==========================================================================
   Platform Detection
   ========================================================================== */

#if defined(_WIN32) || defined(__WIN32__)
  #define SOF_PLATFORM_WINDOWS  1
#elif defined(__linux__)
  #define SOF_PLATFORM_LINUX    1
#elif defined(__APPLE__)
  #define SOF_PLATFORM_MACOS    1
#endif

/* ==========================================================================
   Basic Types
   ========================================================================== */

typedef uint8_t     byte;
typedef int32_t     qboolean;
typedef float       vec_t;
typedef vec_t       vec2_t[2];
typedef vec_t       vec3_t[3];
typedef vec_t       vec4_t[4];
typedef vec_t       vec5_t[5];

#define qtrue   1
#define qfalse  0

/* ==========================================================================
   Limits
   ========================================================================== */

#define MAX_STRING_CHARS    1024    /* max length of a string passed to Cmd_TokenizeString */
#define MAX_STRING_TOKENS   80      /* max tokens resulting from Cmd_TokenizeString */
#define MAX_TOKEN_CHARS     128     /* max length of an individual token */

#define MAX_QPATH           64      /* max length of a quake game pathname */
#define MAX_OSPATH          128     /* max length of a filesystem pathname */

#define MAX_INFO_KEY        64
#define MAX_INFO_VALUE      64
#define MAX_INFO_STRING     512

/* Per-level limits */
#define MAX_CLIENTS         256     /* absolute limit */
#define MAX_EDICTS          1024    /* must change protocol to increase more */
#define MAX_LIGHTSTYLES     256
#define MAX_MODELS          256     /* these are sent over the net as bytes */
#define MAX_SOUNDS          256     /* so they cannot be blindly increased */
#define MAX_IMAGES          256
#define MAX_ITEMS           256
#define MAX_GENERAL         (MAX_CLIENTS * 2)   /* general config strings */

/* SoF-specific limits */
#define SOF_MAX_GORE_ZONES  26
#define SOF_EDICT_SIZE      1104    /* from binary analysis */

/* Configstring indices (Q2 standard layout) */
#define CS_NAME             0
#define CS_CDTRACK          1
#define CS_SKY              2
#define CS_SKYAXIS          3
#define CS_SKYROTATE        4
#define CS_STATUSBAR        5       /* display program string */

#define CS_AIRACCEL         29      /* air acceleration control */
#define CS_MAXCLIENTS       30
#define CS_MAPCHECKSUM      31      /* for catching cheater maps */

#define CS_MODELS           32
#define CS_SOUNDS           (CS_MODELS + MAX_MODELS)
#define CS_IMAGES           (CS_SOUNDS + MAX_SOUNDS)
#define CS_LIGHTS           (CS_IMAGES + MAX_IMAGES)
#define CS_ITEMS            (CS_LIGHTS + MAX_LIGHTSTYLES)
#define CS_PLAYERSKINS      (CS_ITEMS + MAX_ITEMS)
#define CS_GENERAL          (CS_PLAYERSKINS + MAX_CLIENTS)
#define MAX_CONFIGSTRINGS   (CS_GENERAL + MAX_GENERAL)

/* ==========================================================================
   Error Codes
   ========================================================================== */

typedef enum {
    ERR_FATAL,      /* exit the entire game with a popup */
    ERR_DROP,       /* print to console and disconnect from game */
    ERR_DISCONNECT  /* don't kill server */
} err_type_t;

/* ==========================================================================
   Vector Math
   ========================================================================== */

#define DotProduct(x,y)         ((x)[0]*(y)[0]+(x)[1]*(y)[1]+(x)[2]*(y)[2])
#define VectorSubtract(a,b,c)   ((c)[0]=(a)[0]-(b)[0],(c)[1]=(a)[1]-(b)[1],(c)[2]=(a)[2]-(b)[2])
#define VectorAdd(a,b,c)        ((c)[0]=(a)[0]+(b)[0],(c)[1]=(a)[1]+(b)[1],(c)[2]=(a)[2]+(b)[2])
#define VectorCopy(a,b)         ((b)[0]=(a)[0],(b)[1]=(a)[1],(b)[2]=(a)[2])
#define VectorClear(a)          ((a)[0]=(a)[1]=(a)[2]=0)
#define VectorNegate(a,b)       ((b)[0]=-(a)[0],(b)[1]=-(a)[1],(b)[2]=-(a)[2])
#define VectorSet(v,x,y,z)      ((v)[0]=(x),(v)[1]=(y),(v)[2]=(z))
#define VectorScale(v,s,o)      ((o)[0]=(v)[0]*(s),(o)[1]=(v)[1]*(s),(o)[2]=(v)[2]*(s))
#define VectorMA(v,s,b,o)       ((o)[0]=(v)[0]+(b)[0]*(s),(o)[1]=(v)[1]+(b)[1]*(s),(o)[2]=(v)[2]+(b)[2]*(s))

#define VectorCompare(v1,v2)    ((v1)[0]==(v2)[0]&&(v1)[1]==(v2)[1]&&(v1)[2]==(v2)[2])

vec_t   VectorLength(vec3_t v);
vec_t   VectorNormalize(vec3_t v);
void    CrossProduct(vec3_t v1, vec3_t v2, vec3_t cross);

/* ==========================================================================
   Color
   ========================================================================== */

#define COLORPACK(r,g,b,a)  (((a)<<24)|((b)<<16)|((g)<<8)|(r))

/* ==========================================================================
   Key/Value Info Strings (userinfo, serverinfo)
   ========================================================================== */

char    *Info_ValueForKey(char *s, char *key);
void    Info_RemoveKey(char *s, char *key);
void    Info_SetValueForKey(char *s, char *key, char *value);

/* ==========================================================================
   String Utilities
   ========================================================================== */

int     Q_stricmp(const char *s1, const char *s2);
int     Q_strncasecmp(const char *s1, const char *s2, int n);
void    Com_sprintf(char *dest, int size, const char *fmt, ...);
char    *va(const char *format, ...);
void    Q_strncpyz(char *dest, const char *src, int destsize);

/* ==========================================================================
   Random Numbers (SoF exports)
   ========================================================================== */

float   flrand(float min, float max);
int     irand(int min, int max);

/* ==========================================================================
   Byte Order
   ========================================================================== */

short   BigShort(short l);
short   LittleShort(short l);
int     BigLong(int l);
int     LittleLong(int l);
float   BigFloat(float l);
float   LittleFloat(float l);

/* ==========================================================================
   Console Variable
   ========================================================================== */

#define CVAR_ARCHIVE    1       /* set to cause it to be saved to vars.rc */
#define CVAR_USERINFO   2       /* added to userinfo when changed */
#define CVAR_SERVERINFO 4       /* added to serverinfo when changed */
#define CVAR_NOSET      8       /* don't allow change from console at all */
#define CVAR_LATCH      16      /* save changes until server restart */

typedef struct cvar_s {
    char        *name;
    char        *string;
    char        *latched_string;    /* for CVAR_LATCH vars */
    int         flags;
    int         modified;           /* set each time the cvar is changed */
    float       value;
    struct cvar_s *next;
} cvar_t;

/* ==========================================================================
   Console Commands
   ========================================================================== */

typedef void (*xcommand_t)(void);

/* ==========================================================================
   Trace / Collision
   ========================================================================== */

typedef struct csurface_s {
    char        name[16];
    int         flags;
    int         value;
} csurface_t;

typedef struct {
    qboolean    allsolid;       /* if true, plane is not valid */
    qboolean    startsolid;     /* if true, the initial point was in a solid area */
    float       fraction;       /* time completed, 1.0 = didn't hit anything */
    vec3_t      endpos;         /* final position */
    struct {
        vec3_t  normal;
        float   dist;
        byte    type;
        byte    signbits;
        byte    pad[2];
    } plane;
    csurface_t  *surface;       /* surface hit */
    int         contents;       /* contents on other side of surface hit */
    struct edict_s *ent;        /* not set by CM_*() functions */
} trace_t;

/* Content flags (BSP) */
#define CONTENTS_SOLID          1
#define CONTENTS_WINDOW         2
#define CONTENTS_AUX            4
#define CONTENTS_LAVA           8
#define CONTENTS_SLIME          16
#define CONTENTS_WATER          32
#define CONTENTS_MIST           64

#define CONTENTS_PLAYERCLIP     0x10000
#define CONTENTS_MONSTERCLIP    0x20000
#define CONTENTS_CURRENT_0      0x40000
#define CONTENTS_CURRENT_90     0x80000
#define CONTENTS_CURRENT_180    0x100000
#define CONTENTS_CURRENT_270    0x200000
#define CONTENTS_CURRENT_UP     0x400000
#define CONTENTS_CURRENT_DOWN   0x800000
#define CONTENTS_ORIGIN         0x1000000
#define CONTENTS_MONSTER        0x2000000
#define CONTENTS_DEADMONSTER    0x4000000
#define CONTENTS_DETAIL         0x8000000
#define CONTENTS_TRANSLUCENT    0x10000000
#define CONTENTS_LADDER         0x20000000

/* Content masks */
#define MASK_ALL            (-1)
#define MASK_SOLID          (CONTENTS_SOLID | CONTENTS_WINDOW)
#define MASK_PLAYERSOLID    (CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_WINDOW | CONTENTS_MONSTER)
#define MASK_DEADSOLID      (CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_WINDOW)
#define MASK_MONSTERSOLID   (CONTENTS_SOLID | CONTENTS_MONSTERCLIP | CONTENTS_WINDOW | CONTENTS_MONSTER)
#define MASK_WATER          (CONTENTS_WATER | CONTENTS_LAVA | CONTENTS_SLIME)
#define MASK_OPAQUE         (CONTENTS_SOLID | CONTENTS_SLIME | CONTENTS_LAVA)
#define MASK_SHOT           (CONTENTS_SOLID | CONTENTS_MONSTER | CONTENTS_WINDOW | CONTENTS_DEADMONSTER)
#define MASK_CURRENT        (CONTENTS_CURRENT_0 | CONTENTS_CURRENT_90 | CONTENTS_CURRENT_180 | CONTENTS_CURRENT_270 | CONTENTS_CURRENT_UP | CONTENTS_CURRENT_DOWN)

/* BoxEdicts area types */
#define AREA_SOLID      1
#define AREA_TRIGGERS   2

/* Surface flags (BSP) */
#define SURF_LIGHT      0x1
#define SURF_SLICK      0x2
#define SURF_SKY        0x4
#define SURF_WARP       0x8
#define SURF_TRANS33    0x10
#define SURF_TRANS66    0x20
#define SURF_FLOWING    0x40
#define SURF_NODRAW     0x80

/* ==========================================================================
   Player Movement
   ========================================================================== */

typedef struct usercmd_s {
    byte    msec;
    byte    buttons;
    short   angles[3];
    short   forwardmove, sidemove, upmove;
    byte    impulse;
    byte    lightlevel;     /* light level the player is standing on */
} usercmd_t;

/* ==========================================================================
   Player Movement
   ========================================================================== */

/* pmove types */
#define PM_NORMAL       0       /* walking, running */
#define PM_SPECTATOR    1       /* no clipping */
#define PM_DEAD         2       /* no movement, view only */
#define PM_GIB          3       /* gibs */
#define PM_FREEZE       4       /* frozen in place */

/* pmove flags */
#define PMF_DUCKED          1
#define PMF_JUMP_HELD       2
#define PMF_ON_GROUND       4
#define PMF_TIME_WATERJUMP  8
#define PMF_TIME_LAND       16
#define PMF_TIME_TELEPORT   32
#define PMF_NO_PREDICTION   64

/* buttons */
#define BUTTON_ATTACK       1
#define BUTTON_USE          2
#define BUTTON_ANY          128

typedef struct {
    vec3_t      origin;
    vec3_t      velocity;
    int         pm_type;
    int         pm_flags;
    int         pm_time;
    short       gravity;
    short       delta_angles[3];
} pmove_state_t;

/* Complete pmove structure passed to Pmove() */
typedef struct {
    /* State (in/out) */
    pmove_state_t   s;

    /* Command (input) */
    usercmd_t       cmd;
    qboolean        snapinitial;    /* if true, first snap */

    /* Results (output) */
    int             numtouch;
    struct edict_s  *touchents[32]; /* MAX_TOUCH */
    vec3_t          viewangles;
    float           viewheight;
    vec3_t          mins, maxs;     /* bounding box */

    struct edict_s  *groundentity;
    int             watertype;
    int             waterlevel;

    /* Callbacks */
    trace_t (*trace)(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end);
    int     (*pointcontents)(vec3_t point);
} pmove_t;

/* Player movement function */
void    Pmove(pmove_t *pmove);

/* ==========================================================================
   Entity State — communicated by server to clients
   ========================================================================== */

typedef struct entity_state_s {
    int     number;         /* edict index */
    vec3_t  origin;
    vec3_t  angles;
    vec3_t  old_origin;     /* for lerping */
    int     modelindex;
    int     modelindex2, modelindex3, modelindex4;  /* weapons, CTF flags, etc */
    int     frame;
    int     skinnum;
    unsigned int effects;
    int     renderfx;
    int     solid;
    int     sound;
    int     event;
} entity_state_t;

/* ==========================================================================
   Renderer View Definition
   Passed to R_RenderFrame to describe the current view
   ========================================================================== */

#define MAX_DLIGHTS_DEF     32
#define MAX_ENTITIES_DEF    128
#define MAX_PARTICLES_DEF   4096

typedef struct entity_s {
    struct model_s  *model;
    float           angles[3];
    float           origin[3];
    int             frame;
    float           oldorigin[3];
    int             oldframe;
    float           backlerp;
    int             skinnum;
    int             lightstyle;
    float           alpha;
    struct image_s  *skin;
    int             flags;
} entity_t;

typedef struct dlight_s {
    vec3_t  origin;
    vec3_t  color;
    float   intensity;
} dlight_t;

typedef struct particle_s {
    vec3_t  origin;
    int     color;
    float   alpha;
} particle_t;

typedef struct refdef_s {
    int         x, y, width, height;    /* viewport on screen */
    float       fov_x, fov_y;
    vec3_t      vieworg;
    vec3_t      viewangles;
    float       blend[4];               /* rgba full-screen blend */
    float       time;                   /* time in seconds for shader effects */

    int         rdflags;                /* RDF_UNDERWATER, etc */

    byte        *areabits;              /* if not NULL, only areas with set bits will be drawn */

    int         num_entities;
    entity_t    *entities;

    int         num_dlights;
    dlight_t    *dlights;

    int         num_particles;
    particle_t  *particles;

    byte        *lightstyles;           /* [MAX_LIGHTSTYLES] */
} refdef_t;

/* Refdef flags */
#define RDF_UNDERWATER      1
#define RDF_NOWORLDMODEL    2
#define RDF_IRGOGGLES       4   /* SoF: infrared goggles */

#endif /* Q_SHARED_H */
