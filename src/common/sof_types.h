/*
 * sof_types.h - Common type definitions for Soldier of Fortune recompilation
 *
 * These types mirror the original game's data structures, reconstructed
 * through binary analysis of the original executables.
 *
 * Binary: SoF v1.00 retail, compiled 2000-03-10, MSVC 6.0
 */

#ifndef SOF_TYPES_H
#define SOF_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* ==========================================================================
   Basic Types
   Match the original 32-bit MSVC 6.0 type sizes
   ========================================================================== */

typedef uint8_t     byte;
typedef int32_t     qboolean;

/* ==========================================================================
   Math Types (id Tech 2 heritage)
   ========================================================================== */

typedef float vec_t;
typedef vec_t vec2_t[2];
typedef vec_t vec3_t[3];
typedef vec_t vec4_t[4];

/* ==========================================================================
   Forward Declarations
   ========================================================================== */

typedef struct edict_s      edict_t;
typedef struct gclient_s    gclient_t;
typedef struct cvar_s       cvar_t;
typedef struct pmove_s      pmove_t;
typedef struct trace_s      trace_t;
typedef struct usercmd_s    usercmd_t;
typedef struct model_s      model_s;
typedef struct image_s      image_s;
typedef struct refdef_s     refdef_t;

/* Console variable (Q2 heritage) */
struct cvar_s {
    char        *name;
    char        *string;
    char        *latched_string;
    int         flags;
    qboolean    modified;
    float       value;
    struct cvar_s *next;
};

/* ==========================================================================
   GHOUL Damage System
   The 26-zone gore model that made history
   ========================================================================== */

#define GHOUL_MAX_GORE_ZONES 26

typedef enum {
    GORE_ZONE_HEAD = 0,
    GORE_ZONE_NECK,
    GORE_ZONE_CHEST_UPPER,
    GORE_ZONE_CHEST_LOWER,
    GORE_ZONE_STOMACH,
    GORE_ZONE_GROIN,
    GORE_ZONE_ARM_UPPER_L,
    GORE_ZONE_ARM_LOWER_L,
    GORE_ZONE_HAND_L,
    GORE_ZONE_ARM_UPPER_R,
    GORE_ZONE_ARM_LOWER_R,
    GORE_ZONE_HAND_R,
    GORE_ZONE_LEG_UPPER_L,
    GORE_ZONE_LEG_LOWER_L,
    GORE_ZONE_FOOT_L,
    GORE_ZONE_LEG_UPPER_R,
    GORE_ZONE_LEG_LOWER_R,
    GORE_ZONE_FOOT_R,
    /* Additional zones TBD via further analysis */
    GORE_ZONE_COUNT
} ghoul_gore_zone_t;

typedef struct {
    int             zone_id;
    float           health;
    qboolean        severed;
    qboolean        damaged;
    int             damage_level;
} ghoul_zone_state_t;

/* ==========================================================================
   game_import_t — Engine functions provided to game DLL
   99 function pointers (396 bytes), stored in gamex86.dll at 0x50140820
   Q2 base had 44 fields; SoF adds 55 for GHOUL, extended sound, etc.
   ========================================================================== */

typedef enum {
    MULTICAST_ALL,
    MULTICAST_PHS,
    MULTICAST_PVS,
    MULTICAST_ALL_R,
    MULTICAST_PHS_R,
    MULTICAST_PVS_R
} multicast_t;

typedef struct {
    /* --- Q2-compatible fields (0x000–0x0AC) --- */
    void    (*bprintf)(int printlevel, char *fmt, ...);         /* 0x000 */
    void    (*dprintf)(char *fmt, ...);                         /* 0x004 — 259 xrefs */
    void    (*cprintf)(edict_t *ent, int printlevel, char *fmt, ...); /* 0x008 */
    void    (*centerprintf)(edict_t *ent, char *fmt, ...);      /* 0x00C */
    void    (*sound)(edict_t *ent, int channel, int soundindex,
                     float volume, float attenuation, float timeofs); /* 0x010 */
    void    (*positioned_sound)(vec3_t origin, edict_t *ent, int channel,
                     int soundindex, float volume, float attenuation,
                     float timeofs);                            /* 0x014 */

    void    (*configstring)(int num, char *string);             /* 0x018 */
    void    (*error)(char *fmt, ...);                           /* 0x01C */
    void    *_unknown_020;                                      /* 0x020 */

    int     (*soundindex)(char *name);                          /* 0x024 */
    int     (*imageindex)(char *name);                          /* 0x028 — 129 xrefs */
    void    (*setmodel)(edict_t *ent, char *name);              /* 0x02C */

    trace_t (*trace)(vec3_t start, vec3_t mins, vec3_t maxs,
                     vec3_t end, edict_t *passent, int contentmask); /* 0x030 — 95 xrefs */
    int     (*pointcontents)(vec3_t point);                     /* 0x034 */
    qboolean (*inPVS)(vec3_t p1, vec3_t p2);                   /* 0x038 */

    /* Fields 0x03C–0x050: entity linking, areas, Pmove */
    void    *_fields_03C_to_050[6];                             /* 0x03C–0x050 */

    void    (*Pmove)(pmove_t *pmove);                           /* 0x054 — 35 xrefs */

    /* Extended sound (SoF, 7 args) */
    void    *_field_058;                                        /* 0x058 */
    void    (*sound_extended)(edict_t *ent, int channel, int soundindex,
                     float volume, float attenuation, float timeofs,
                     int extra);                                /* 0x05C — 173 xrefs */

    /* Network messaging Write* functions */
    void    *_fields_060_to_07C[8];                             /* 0x060–0x07C */

    void    *_fields_080_to_084[2];                             /* 0x080–0x084 */
    void    *(*TagMalloc)(int size, int tag);                   /* 0x088 */
    void    (*TagFree)(void *block);                            /* 0x08C */
    void    (*FreeTags)(int tag);                               /* 0x090 */

    cvar_t  *(*cvar)(char *var_name, char *value, int flags);   /* 0x094 */
    cvar_t  *(*cvar_set)(char *var_name, char *value);          /* 0x098 */

    int     (*argc)(void);                                      /* 0x09C — 175 xrefs */
    char    *(*argv)(int n);                                    /* 0x0A0 */
    char    *(*args)(void);                                     /* 0x0A4 */

    void    *_field_0A8;                                        /* 0x0A8 */
    void    (*DebugGraph)(float value, int color);              /* 0x0AC */

    /* --- SoF-specific fields (0x0B0–0x188) --- */
    void    *_sof_fields_0B0_to_0E0[13];                       /* 0x0B0–0x0E0 */

    cvar_t  *(*cvar_register)(char *name, char *value, int flags, int extra); /* 0x0E4 — 90 xrefs */
    cvar_t  *(*cvar_set_string)(char *name, char *value);       /* 0x0E8 */
    void    (*cvar_set_float)(char *name, float value);         /* 0x0EC */

    void    *_sof_fields_0F0_to_11C[12];                        /* 0x0F0–0x118 */

    cvar_t  *(*cvar_get)(char *name, char *value, int flags, int extra); /* 0x11C */

    void    *_sof_fields_120_to_14C[12];                        /* 0x120–0x14C */

    /* GHOUL engine functions */
    void    *(*ghoul_func_1)(void *, void *, void *);           /* 0x150 — 149 xrefs */
    void    *(*ghoul_func_2)(void *, void *, void *, void *);   /* 0x154 — 138 xrefs */

    void    *_sof_fields_158_to_180[11];                        /* 0x158–0x180 */

    /* Random number generation */
    float   (*flrand)(float min, float max);                    /* 0x184 — 215 xrefs */
    int     (*irand)(int min, int max);                         /* 0x188 — 255 xrefs */

} game_import_t;

/* ==========================================================================
   game_export_t — Game functions provided to engine
   Static instance at 0x50140728 in gamex86.dll
   API version 3, edict_size 1104 (0x450)
   ========================================================================== */

#define SOF_GAME_API_VERSION    3
#define SOF_EDICT_SIZE          1104

typedef struct {
    int         apiversion;                                     /* +0x00 = 3 */

    void        (*Init)(void);                                  /* +0x04 */
    void        (*Shutdown)(void);                              /* +0x08 */
    void        (*SpawnEntities)(char *mapname, char *entstring,
                                 char *spawnpoint);             /* +0x0C */
    void        (*WriteGame)(char *filename, qboolean autosave);/* +0x10 */
    void        (*ReadGame)(char *filename);                    /* +0x14 */
    void        (*WriteLevel)(char *filename);                  /* +0x18 */
    void        (*ReadLevel)(char *filename);                   /* +0x1C */

    qboolean    (*ClientConnect)(edict_t *ent, char *userinfo); /* +0x20 */
    void        (*ClientDisconnect)(edict_t *ent);              /* +0x24 */
    void        (*ClientBegin)(edict_t *ent);                   /* +0x28 */
    void        (*ClientUserinfoChanged)(edict_t *ent, char *userinfo); /* +0x2C */
    void        (*ClientCommand)(edict_t *ent);                 /* +0x30 */
    void        (*ClientThink)(edict_t *ent, usercmd_t *cmd);   /* +0x34 */

    void        (*RunFrame)(void);                              /* +0x38 */
    void        (*ServerCommand)(void);                         /* +0x3C */

    /* SoF additions beyond Q2 */
    void        (*GetGameTime)(void);                           /* +0x40 */
    void        (*RegisterWeapons)(void);                       /* +0x44 */
    char       *(*GetGameVersion)(void);                        /* +0x48 */
    qboolean    (*GetCheatsEnabled)(void);                      /* +0x4C */
    void        (*SetCheatsEnabled)(qboolean enabled);          /* +0x50 */
    void        (*RunAI)(void);                                 /* +0x54 */

    /* Entity array (filled by Init) */
    struct edict_s  *edicts;                                    /* +0x58 */
    int         edict_size;                                     /* +0x5C = 1104 */
    int         num_edicts;                                     /* +0x60 */
    int         max_edicts;                                     /* +0x64 */
} game_export_t;

/* ==========================================================================
   refimport_t — Engine functions provided to renderer DLL
   27 function pointers (108 bytes), stored in ref_gl.dll at 0x3008CBE8
   ========================================================================== */

typedef struct {
    /* Q2-compatible fields */
    void    (*Sys_Error)(int err_level, char *str, ...);        /* 0 */
    void    (*Cmd_AddCommand)(char *name, void(*cmd)(void));    /* 1 */
    void    (*Cmd_RemoveCommand)(char *name);                   /* 2 */
    int     (*Cmd_Argc)(void);                                  /* 3 */
    char   *(*Cmd_Argv)(int i);                                 /* 4 */
    void    (*Cmd_ExecuteText)(int exec_when, char *text);      /* 5 */
    void    (*Con_Printf)(int print_level, char *str, ...);     /* 6 */
    int     (*FS_LoadFile)(char *name, void **buf);             /* 7 */
    void    (*FS_FreeFile)(void *buf);                          /* 8 — 90 xrefs */
    char   *(*FS_Gamedir)(void);                                /* 9 */
    cvar_t *(*Cvar_Get)(char *name, char *value, int flags);    /* 10 */
    cvar_t *(*Cvar_Set)(char *name, char *value);               /* 11 */
    void    (*Cvar_SetValue)(char *name, float value);          /* 12 — 107 xrefs */
    qboolean (*Vid_GetModeInfo)(int *width, int *height, int mode); /* 13 */
    void    (*Vid_MenuInit)(void);                              /* 14 */
    void    (*Vid_NewWindow)(int width, int height);            /* 15 */

    /* SoF additions */
    void   *_sof_fields_16_to_19[4];                            /* 16–19 */
    void   *(*Z_Malloc)(int size);                              /* 20 */
    void    (*Z_Free)(void *ptr);                               /* 21 */
    void   *(*Z_Realloc)(void *ptr, int size);                  /* 22 */
    void    (*Z_MemInfo)(void);                                 /* 23 */
    void   *(*Z_TagMalloc)(int size, int tag);                  /* 24 */
    void    (*Z_TagFree)(void *ptr);                            /* 25 */
    int     (*Sys_GetTime)(void);                               /* 26 */
} refimport_t;

/* ==========================================================================
   refexport_t — Renderer functions provided to engine
   54 fields (216 bytes / 0xD8)
   NOTE: SoF uses a different calling convention than Q2!
     Q2:  refexport_t  GetRefAPI(refimport_t rimp);
     SoF: refexport_t* GetRefAPI(refexport_t* out, refimport_t* rimp);
   ========================================================================== */

#define SOF_REF_API_VERSION     3

typedef struct {
    int         api_version;                                    /* +0x000 = 3 */

    /* Lifecycle */
    qboolean    (*Init)(void *hinstance, void *wndproc);        /* +0x004 */
    void        (*Shutdown)(void);                              /* +0x008 */
    void        (*ChangeDisplaySettings)(void);                 /* +0x00C — SoF */

    /* Asset registration */
    void        (*BeginRegistration)(char *map);                /* +0x010 */
    model_s    *(*RegisterModel)(char *name);                   /* +0x014 */
    image_s    *(*RegisterSkin)(char *name);                    /* +0x018 */
    image_s    *(*RegisterPic)(char *name);                     /* +0x01C */
    void        (*SetSky)(char *name, float rotate, vec3_t axis);/* +0x020 */
    void        (*EndRegistration)(void);                       /* +0x024 */

    /* Rendering */
    void        (*RenderFrame)(refdef_t *fd);                   /* +0x028 */
    void        (*DrawGetPicSize)(int *w, int *h, char *name);  /* +0x02C */
    void        (*DrawPic)(int x, int y, char *name);           /* +0x030 */
    void        (*DrawStretchPic)(int x, int y, int w, int h, char *name); /* +0x034 — STUB */
    void        (*DrawChar)(int x, int y, int c);               /* +0x038 */
    void        (*DrawTileClear)(int x, int y, int w, int h, char *name); /* +0x03C */
    void        (*DrawFill)(int x, int y, int w, int h, int c); /* +0x040 */
    void        (*DrawFadeScreen)(void);                        /* +0x044 */
    void        (*DrawStretchRaw)(int x, int y, int w, int h,
                                  int cols, int rows, byte *data);/* +0x048 */
    void        (*CinematicSetPalette)(const unsigned char *palette);/* +0x04C */
    void        (*BeginFrame)(float camera_separation);         /* +0x050 */
    void        (*EndFrame)(void);                              /* +0x054 */
    void        (*AppActivate)(qboolean activate);              /* +0x058 */

    /* SoF additions */
    void        (*SetMode)(void);                               /* +0x05C */
    void        (*SetMode2)(void);                              /* +0x060 */
    void        *_reserved_064;                                 /* +0x064 — NULL */
    void        (*SetGamma)(void);                              /* +0x068 */
    void        (*GetRefConfig)(void);                          /* +0x06C */
    void        (*SetViewport)(void);                           /* +0x070 */
    void        (*SetScissor)(void);                            /* +0x074 */
    void        (*SetOrtho)(void);                              /* +0x078 */
    void        *_reserved_07C;                                 /* +0x07C — NULL */
    void        (*DrawStretchPic2)(void);                       /* +0x080 */
    void        (*DrawSetColor)(void);                          /* +0x084 */
    void        (*DrawFillRect)(void);                          /* +0x088 */
    void        *_sof_draw_08C_to_098[4];                       /* +0x08C–0x098 */
    void        (*DrawModel)(void);                             /* +0x09C */
    void        (*GetLightLevel)(void);                         /* +0x0A0 */
    void        (*GetModelBounds)(void);                        /* +0x0A4 */
    void        (*InitFonts)(void);                             /* +0x0A8 */
    void        (*GLimp_Init)(void);                            /* +0x0AC */
    void        (*GLimp_SetState)(void);                        /* +0x0B0 */
    void        (*GLimp_SetState2)(void);                       /* +0x0B4 */
    void        (*GLimp_EndFrame)(void);                        /* +0x0B8 */
    void        (*AddParticleEffect)(void);                     /* +0x0BC */
    void        (*SetGLFunction)(void);                         /* +0x0C0 */
    void        (*DrawShadow)(void);                            /* +0x0C4 */
    void        (*DrawDecal)(void);                             /* +0x0C8 */
    void        (*DrawDecal2)(void);                            /* +0x0CC */
    void        (*ReadPixels)(void);                            /* +0x0D0 */
    void        (*ClearScene)(void);                            /* +0x0D4 */
} refexport_t;

/* ==========================================================================
   player.dll Interface (SoF-specific, no Q2 equivalent)
   ========================================================================== */

/* Exact signatures TBD via analysis */
typedef struct {
    /* TBD */
    void *placeholder;
} player_client_api_t;

typedef struct {
    /* TBD */
    void *placeholder;
} player_server_api_t;

/* player.dll exports */
/* player_client_api_t* GetPlayerClientAPI(void); */
/* player_server_api_t* GetPlayerServerAPI(void); */

/* ==========================================================================
   Sound DLL Plugin Interface
   20 common exports across Defsnd.dll / EAXSnd.dll / A3Dsnd.dll
   All share image base 0x60000000 (mutually exclusive)
   ========================================================================== */

typedef struct {
    /* Lifecycle */
    qboolean    (*S_Init)(void);
    void        (*S_Shutdown)(void);
    void        (*S_Activate)(qboolean active);
    void        (*S_Update)(void);

    /* Registration (level load) */
    void        (*S_BeginRegistration)(void);
    void        (*S_EndRegistration)(void);
    int         (*S_RegisterSound)(char *name);
    void        (*S_RegisterAmbientSet)(char *name);
    void        (*S_RegisterMusicSet)(char *name);
    int         (*S_FindName)(char *name);
    void        (*S_FreeSound)(int handle);
    void        (*S_Touch)(int handle);

    /* Playback */
    void        (*S_StartSound)(vec3_t origin, edict_t *ent, int channel,
                                int soundindex, float volume,
                                float attenuation, float timeofs);
    void        (*S_StartLocalSound)(char *name);
    void        (*S_StopAllSounds)(void);

    /* Engine integration */
    void        (*S_SetSoundStruct)(void *sound_data);
    void        (*S_SetSoundProcType)(int type);

    /* Dynamic audio */
    void        (*S_SetGeneralAmbientSet)(char *name);
    void        (*S_SetMusicIntensity)(float intensity);
    void        (*S_SetMusicDesignerInfo)(void *info);
} sound_export_t;

/* EAX-specific extensions */
/* void SNDEAX_SetEnvironment(int env_type); */
/* void SNDEAX_SetEnvironmentLevel(float level); */

/* A3D-specific extensions */
/* void S_A3D_ExportRenderGeom(void *geom); */

#endif /* SOF_TYPES_H */
