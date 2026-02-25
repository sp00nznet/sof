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

#endif /* Q_SHARED_H */
