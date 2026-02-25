/*
 * g_local.h - Game module local definitions
 *
 * Reconstructed from Soldier of Fortune gamex86.dll binary analysis.
 * API version 3, edict_size 1104 (0x450).
 *
 * Q2 base had ~44 game_import_t fields; SoF extends to 99 (+55 for GHOUL,
 * extended sound, SoF-specific cvars, random number generation).
 *
 * Original: gamex86.dll, 0x50000000 base, compiled 2000-03-10 MSVC 6.0
 */

#ifndef G_LOCAL_H
#define G_LOCAL_H

#include "../common/qcommon.h"

/* ==========================================================================
   Game API Version
   ========================================================================== */

#define GAME_API_VERSION    3

/* ==========================================================================
   Forward Declarations
   ========================================================================== */

typedef struct edict_s      edict_t;
typedef struct gclient_s    gclient_t;
typedef struct moveinfo_s   moveinfo_t;

/* ==========================================================================
   Multicast
   ========================================================================== */

typedef enum {
    MULTICAST_ALL,
    MULTICAST_PHS,
    MULTICAST_PVS,
    MULTICAST_ALL_R,
    MULTICAST_PHS_R,
    MULTICAST_PVS_R
} multicast_t;

/* ==========================================================================
   game_import_t — Engine functions provided to game
   99 function pointers (0x18C bytes)
   Stored at 0x50140820 in original gamex86.dll
   ========================================================================== */

typedef struct {
    /* --- Q2-compatible fields (0x000-0x0AC) --- */
    void    (*bprintf)(int printlevel, const char *fmt, ...);
    void    (*dprintf)(const char *fmt, ...);
    void    (*cprintf)(edict_t *ent, int printlevel, const char *fmt, ...);
    void    (*centerprintf)(edict_t *ent, const char *fmt, ...);
    void    (*sound)(edict_t *ent, int channel, int soundindex,
                     float volume, float attenuation, float timeofs);
    void    (*positioned_sound)(vec3_t origin, edict_t *ent, int channel,
                     int soundindex, float volume, float attenuation,
                     float timeofs);

    void    (*configstring)(int num, const char *string);
    void    (*error)(const char *fmt, ...);

    /* Model/image registration */
    int     (*modelindex)(const char *name);
    int     (*soundindex)(const char *name);
    int     (*imageindex)(const char *name);

    void    (*setmodel)(edict_t *ent, const char *name);

    /* Collision */
    trace_t (*trace)(vec3_t start, vec3_t mins, vec3_t maxs,
                     vec3_t end, edict_t *passent, int contentmask);
    int     (*pointcontents)(vec3_t point);
    qboolean (*inPVS)(vec3_t p1, vec3_t p2);
    qboolean (*inPHS)(vec3_t p1, vec3_t p2);

    /* Entity linking */
    void    (*setorigin)(edict_t *ent, vec3_t origin);
    void    (*linkentity)(edict_t *ent);
    void    (*unlinkentity)(edict_t *ent);
    int     (*BoxEdicts)(vec3_t mins, vec3_t maxs, edict_t **list,
                         int maxcount, int areatype);
    qboolean (*AreasConnected)(int area1, int area2);

    /* Player movement */
    void    (*Pmove)(void *pmove);

    /* Network messages */
    void    (*multicast)(vec3_t origin, multicast_t to);
    void    (*unicast)(edict_t *ent, qboolean reliable);
    void    (*WriteChar)(int c);
    void    (*WriteByte)(int c);
    void    (*WriteShort)(int c);
    void    (*WriteLong)(int c);
    void    (*WriteFloat)(float f);
    void    (*WriteString)(const char *s);
    void    (*WritePosition)(vec3_t pos);
    void    (*WriteDir)(vec3_t dir);
    void    (*WriteAngle)(float f);

    /* Memory management */
    void    *(*TagMalloc)(int size, int tag);
    void    (*TagFree)(void *block);
    void    (*FreeTags)(int tag);

    /* Cvars */
    cvar_t  *(*cvar)(const char *var_name, const char *value, int flags);
    cvar_t  *(*cvar_set)(const char *var_name, const char *value);
    void    (*cvar_forceset)(const char *var_name, const char *value);

    /* Command args */
    int     (*argc)(void);
    char    *(*argv)(int n);
    char    *(*args)(void);

    /* Console command execution */
    void    (*AddCommandString)(const char *text);

    void    (*DebugGraph)(float value, int color);

    /* --- SoF-specific fields (0x0B0-0x188) --- */

    /* Extended sound (7 args, 173 xrefs) */
    void    (*sound_extended)(edict_t *ent, int channel, int soundindex,
                     float volume, float attenuation, float timeofs,
                     int flags);

    /* GHOUL model system integration */
    void    *(*ghoul_load_model)(const char *name, int flags, int extra);
    void    *(*ghoul_attach_bolt)(void *model, const char *tag, void *bolt);
    void    (*ghoul_set_skin)(edict_t *ent, const char *skin);
    void    (*ghoul_set_anim)(edict_t *ent, const char *anim, int flags);
    void    (*ghoul_damage_zone)(edict_t *ent, int zone, int damage);
    void    (*ghoul_sever_zone)(edict_t *ent, int zone);

    /* Entity management */
    void    (*entity_set_flags)(edict_t *ent, int flags);

    /* Random number generation (SoF exports, highest xref counts) */
    float   (*flrand)(float min, float max);
    int     (*irand)(int min, int max);
} game_import_t;

/* ==========================================================================
   game_export_t — Game functions provided to engine
   Static instance at 0x50140728 in original gamex86.dll
   ========================================================================== */

typedef struct {
    int         apiversion;

    /* Lifecycle */
    void        (*Init)(void);
    void        (*Shutdown)(void);

    /* Level management */
    void        (*SpawnEntities)(const char *mapname, const char *entstring,
                                 const char *spawnpoint);
    void        (*WriteGame)(const char *filename, qboolean autosave);
    void        (*ReadGame)(const char *filename);
    void        (*WriteLevel)(const char *filename);
    void        (*ReadLevel)(const char *filename);

    /* Client management */
    qboolean    (*ClientConnect)(edict_t *ent, char *userinfo);
    void        (*ClientBegin)(edict_t *ent);
    void        (*ClientUserinfoChanged)(edict_t *ent, char *userinfo);
    void        (*ClientCommand)(edict_t *ent);
    void        (*ClientThink)(edict_t *ent, usercmd_t *cmd);
    void        (*ClientDisconnect)(edict_t *ent);

    /* Frame */
    void        (*RunFrame)(void);
    void        (*ServerCommand)(void);

    /* SoF additions */
    void        (*GetGameTime)(void);
    void        (*RegisterWeapons)(void);
    const char  *(*GetGameVersion)(void);
    qboolean    (*GetCheatsEnabled)(void);
    void        (*SetCheatsEnabled)(qboolean enabled);
    void        (*RunAI)(void);

    /* Entity array (filled by Init) */
    struct edict_s  *edicts;
    int         edict_size;
    int         num_edicts;
    int         max_edicts;
} game_export_t;

/* pmove_state_t is defined in q_shared.h */

/* ==========================================================================
   gclient_s — Per-client game state
   ========================================================================== */

struct gclient_s {
    /* Communicated to server/client */
    pmove_state_t   ps;
    vec3_t          viewangles;
    vec3_t          kick_angles;
    vec3_t          kick_origin;
    float           blend[4];
    float           fov;
    int             rdflags;

    /* Private to game */
    int             ping;
    char            pers_netname[16];
    char            pers_userinfo[MAX_INFO_STRING];
    int             pers_health;
    int             pers_max_health;
    int             pers_connected;
    int             pers_weapon;

    /* SoF-specific */
    int             team;           /* 0=none, 1=red, 2=blue */
    int             class_id;
    int             gore_kills;     /* dismemberment kills count */
    int             last_damage_zone;

    /* Pad to known alignment */
    byte            _pad[128];
};

/* ==========================================================================
   edict_s — Game entity (1104 bytes = 0x450 in original)
   ========================================================================== */

/* Entity flags */
#define FL_FLY              0x00000001
#define FL_SWIM             0x00000002
#define FL_IMMUNE_LASER     0x00000004
#define FL_INWATER          0x00000008
#define FL_GODMODE          0x00000010
#define FL_NOTARGET         0x00000020
#define FL_IMMUNE_SLIME     0x00000040
#define FL_IMMUNE_LAVA      0x00000080
#define FL_PARTIALGROUND    0x00000100
#define FL_WATERJUMP        0x00000200
#define FL_TEAMSLAVE        0x00000400
#define FL_NO_KNOCKBACK     0x00000800
#define FL_POWER_ARMOR      0x00001000
#define FL_RESPAWN          0x80000000

/* SoF damage types */
#define DAMAGE_NO           0
#define DAMAGE_YES          1
#define DAMAGE_AIM          2

/* SoF entity types */
#define ET_GENERAL          0
#define ET_PLAYER           1
#define ET_ITEM             2
#define ET_MISSILE          3
#define ET_MOVER            4

struct edict_s {
    /* Entity state — communicated to clients */
    entity_state_t  s;

    /* Shared between game and server */
    qboolean        inuse;
    int             linkcount;
    qboolean        linked;

    /* Area linking for efficient collision */
    int             areanum;
    int             areanum2;

    /* SVF_ flags */
    int             svflags;

    /* Bounding box */
    vec3_t          mins, maxs;
    vec3_t          absmin, absmax;
    vec3_t          size;

    /* Solid type */
    int             solid;

    /* Content/clip masks */
    int             clipmask;

    /* Owner */
    edict_t         *owner;

    /* --- Game-private fields below --- */

    /* Movement */
    int             movetype;
    int             flags;
    vec3_t          velocity;
    vec3_t          avelocity;

    /* Physics */
    float           mass;
    float           gravity;

    /* Combat */
    int             health;
    int             max_health;
    int             deadflag;
    int             takedamage;
    int             dmg;
    int             dmg_radius;
    float           dmg_debounce_time;

    /* Targeting / triggering */
    char            *classname;
    char            *model;
    char            *target;
    char            *targetname;
    char            *killtarget;
    char            *message;

    /* Think function */
    float           nextthink;
    void            (*prethink)(edict_t *self);
    void            (*think)(edict_t *self);
    void            (*blocked)(edict_t *self, edict_t *other);
    void            (*touch)(edict_t *self, edict_t *other,
                             void *plane, csurface_t *surf);
    void            (*use)(edict_t *self, edict_t *other, edict_t *activator);
    void            (*pain)(edict_t *self, edict_t *other, float kick, int damage);
    void            (*die)(edict_t *self, edict_t *inflictor, edict_t *attacker,
                           int damage, vec3_t point);

    /* Timing */
    float           wait;
    float           delay;
    float           random;
    float           teleport_time;

    /* Sound indices */
    int             noise_index;
    int             noise_index2;

    /* Animation */
    int             count;
    float           volume;
    float           attenuation;
    int             style;
    float           speed;

    /* Chain */
    edict_t         *chain;
    edict_t         *enemy;
    edict_t         *oldenemy;
    edict_t         *activator;
    edict_t         *groundentity;
    int             groundentity_linkcount;
    edict_t         *teamchain;
    edict_t         *teammaster;

    /* Client pointer (NULL for non-player entities) */
    gclient_t       *client;

    /* SoF extensions */
    int             gore_zone_mask;         /* bitfield: which zones are damaged */
    int             severed_zone_mask;      /* bitfield: which zones are severed */
    int             entity_type;            /* ET_GENERAL, ET_PLAYER, etc. */
    int             weapon_index;
    char            *weapon_model;

    /* AI state */
    int             ai_flags;
    float           ideal_yaw;
    float           yaw_speed;
    vec3_t          move_origin;
    vec3_t          move_angles;

    /* Pad to 1104 bytes (0x450) — exact size from binary analysis */
    byte            _pad[64];
};

/* ==========================================================================
   SoF Weapons (from RegisterWeapons at 0x50095280)
   ========================================================================== */

typedef enum {
    WEAP_NONE = 0,
    WEAP_KNIFE,
    WEAP_PISTOL1,       /* .44 Desert Eagle */
    WEAP_PISTOL2,       /* Silver Talon */
    WEAP_SHOTGUN,        /* Pump Shotgun */
    WEAP_MACHINEGUN,     /* HK MP5 */
    WEAP_ASSAULT,        /* M4 Assault Rifle */
    WEAP_SNIPER,         /* MSG90 Sniper */
    WEAP_SLUGGER,        /* Slugthrower */
    WEAP_ROCKET,         /* M202A2 Quad Rocket */
    WEAP_FLAMEGUN,       /* Flame Thrower */
    WEAP_MPG,            /* Microwave Pulse Gun */
    WEAP_MPISTOL,        /* Machine Pistol */
    WEAP_GRENADE,        /* Grenades */
    WEAP_C4,             /* C4 Explosive */
    WEAP_MEDKIT,         /* Medical Kit */
    WEAP_GOGGLES,        /* Night Vision / IR Goggles */
    WEAP_FPAK,           /* Field Pack */
    WEAP_COUNT
} weapon_id_t;

/* ==========================================================================
   Game Module API
   ========================================================================== */

/* Called by engine to initialize game module */
game_export_t *GetGameAPI(game_import_t *import);

/* Global game import interface (filled by GetGameAPI) */
extern game_import_t gi;

/* Entity spawning (g_spawn.c) */
void G_SpawnEntities(const char *mapname, const char *entstring,
                     const char *spawnpoint);

#endif /* G_LOCAL_H */
