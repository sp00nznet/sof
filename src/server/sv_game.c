/*
 * sv_game.c - Server-side game interface
 *
 * Bridges engine services into game_import_t function pointers,
 * calls GetGameAPI, and manages the game module lifecycle.
 *
 * In the original SoF, this loaded gamex86.dll via LoadLibrary.
 * In our unified binary, GetGameAPI is called directly.
 */

#include "../common/qcommon.h"
#include "../game/g_local.h"

/* ==========================================================================
   Game Module State
   ========================================================================== */

static game_export_t    *ge;        /* game functions */
static game_import_t    gi_impl;    /* engine functions for game */

/* ==========================================================================
   game_import_t implementations
   Bridge engine subsystems to the game_import_t function pointer table
   ========================================================================== */

static void GI_bprintf(int printlevel, const char *fmt, ...)
{
    va_list argptr;
    char    msg[1024];

    va_start(argptr, fmt);
    vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    Com_Printf("%s", msg);
}

static void GI_dprintf(const char *fmt, ...)
{
    va_list argptr;
    char    msg[1024];

    va_start(argptr, fmt);
    vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    Com_Printf("%s", msg);
}

static void GI_cprintf(edict_t *ent, int printlevel, const char *fmt, ...)
{
    va_list argptr;
    char    msg[1024];

    (void)ent;
    (void)printlevel;

    va_start(argptr, fmt);
    vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    Com_Printf("%s", msg);
}

static void GI_centerprintf(edict_t *ent, const char *fmt, ...)
{
    va_list argptr;
    char    msg[1024];

    (void)ent;

    va_start(argptr, fmt);
    vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    Com_Printf("[CENTER] %s", msg);
}

static void GI_sound(edict_t *ent, int channel, int soundindex,
                     float volume, float attenuation, float timeofs)
{
    (void)ent; (void)channel; (void)soundindex;
    (void)volume; (void)attenuation; (void)timeofs;
    /* TODO: route to sound system */
}

static void GI_positioned_sound(vec3_t origin, edict_t *ent, int channel,
                     int soundindex, float volume, float attenuation,
                     float timeofs)
{
    (void)origin; (void)ent; (void)channel; (void)soundindex;
    (void)volume; (void)attenuation; (void)timeofs;
}

static void GI_configstring(int num, const char *string)
{
    (void)num; (void)string;
    /* TODO: send configstring to clients */
}

static void GI_error(const char *fmt, ...)
{
    va_list argptr;
    char    msg[1024];

    va_start(argptr, fmt);
    vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    Com_Error(ERR_DROP, "Game Error: %s", msg);
}

/* Model/image/sound index stubs */
static int GI_modelindex(const char *name) { (void)name; return 0; }
static int GI_soundindex(const char *name) { (void)name; return 0; }
static int GI_imageindex(const char *name) { (void)name; return 0; }
static void GI_setmodel(edict_t *ent, const char *name) { (void)ent; (void)name; }

/* Collision stubs */
static trace_t GI_trace(vec3_t start, vec3_t mins, vec3_t maxs,
                        vec3_t end, edict_t *passent, int contentmask)
{
    trace_t tr;
    (void)mins; (void)maxs; (void)passent; (void)contentmask;

    memset(&tr, 0, sizeof(tr));
    tr.fraction = 1.0f;
    VectorCopy(end, tr.endpos);
    return tr;
}

static int GI_pointcontents(vec3_t point)
{
    (void)point;
    return 0;
}

static qboolean GI_inPVS(vec3_t p1, vec3_t p2)
{
    (void)p1; (void)p2;
    return qtrue;
}

static qboolean GI_inPHS(vec3_t p1, vec3_t p2)
{
    (void)p1; (void)p2;
    return qtrue;
}

/* Entity linking stubs */
static void GI_setorigin(edict_t *ent, vec3_t origin)
{
    VectorCopy(origin, ent->s.origin);
}

static void GI_linkentity(edict_t *ent) { (void)ent; }
static void GI_unlinkentity(edict_t *ent) { (void)ent; }

static int GI_BoxEdicts(vec3_t mins, vec3_t maxs, edict_t **list,
                        int maxcount, int areatype)
{
    (void)mins; (void)maxs; (void)list; (void)maxcount; (void)areatype;
    return 0;
}

static qboolean GI_AreasConnected(int area1, int area2)
{
    (void)area1; (void)area2;
    return qtrue;
}

/* Player movement stub */
static void GI_Pmove(void *pmove) { (void)pmove; }

/* Network message stubs */
static void GI_multicast(vec3_t origin, multicast_t to) { (void)origin; (void)to; }
static void GI_unicast(edict_t *ent, qboolean reliable) { (void)ent; (void)reliable; }
static void GI_WriteChar(int c) { (void)c; }
static void GI_WriteByte(int c) { (void)c; }
static void GI_WriteShort(int c) { (void)c; }
static void GI_WriteLong(int c) { (void)c; }
static void GI_WriteFloat(float f) { (void)f; }
static void GI_WriteString(const char *s) { (void)s; }
static void GI_WritePosition(vec3_t pos) { (void)pos; }
static void GI_WriteDir(vec3_t dir) { (void)dir; }
static void GI_WriteAngle(float f) { (void)f; }

/* Memory management — route to engine zone allocator */
static void *GI_TagMalloc(int size, int tag) { return Z_TagMalloc(size, tag); }
static void GI_TagFree(void *block) { Z_Free(block); }
static void GI_FreeTags(int tag) { Z_FreeTags(tag); }

/* Cvar wrappers */
static cvar_t *GI_cvar(const char *name, const char *value, int flags)
{
    return Cvar_Get(name, value, flags);
}

static cvar_t *GI_cvar_set(const char *name, const char *value)
{
    return Cvar_Set(name, value);
}

static void GI_cvar_forceset(const char *name, const char *value)
{
    Cvar_ForceSet(name, value);
}

/* Command args */
static int GI_argc(void) { return Cmd_Argc(); }
static char *GI_argv(int n) { return Cmd_Argv(n); }
static char *GI_args(void) { return Cmd_Args(); }

/* Console command execution */
static void GI_AddCommandString(const char *text)
{
    Cbuf_AddText(text);
}

static void GI_DebugGraph(float value, int color) { (void)value; (void)color; }

/* SoF extended sound */
static void GI_sound_extended(edict_t *ent, int channel, int soundindex,
                     float volume, float attenuation, float timeofs,
                     int flags)
{
    (void)ent; (void)channel; (void)soundindex;
    (void)volume; (void)attenuation; (void)timeofs; (void)flags;
}

/* GHOUL stubs */
static void *GI_ghoul_load_model(const char *name, int flags, int extra)
{
    (void)name; (void)flags; (void)extra;
    return NULL;
}

static void *GI_ghoul_attach_bolt(void *model, const char *tag, void *bolt)
{
    (void)model; (void)tag; (void)bolt;
    return NULL;
}

static void GI_ghoul_set_skin(edict_t *ent, const char *skin)
{
    (void)ent; (void)skin;
}

static void GI_ghoul_set_anim(edict_t *ent, const char *anim, int flags)
{
    (void)ent; (void)anim; (void)flags;
}

static void GI_ghoul_damage_zone(edict_t *ent, int zone, int damage)
{
    (void)ent; (void)zone; (void)damage;
}

static void GI_ghoul_sever_zone(edict_t *ent, int zone)
{
    (void)ent; (void)zone;
}

static void GI_entity_set_flags(edict_t *ent, int flags)
{
    if (ent)
        ent->flags = flags;
}

/* ==========================================================================
   SV_InitGameProgs — Initialize game module
   Called from engine initialization
   ========================================================================== */

void SV_InitGameProgs(void)
{
    /* Populate engine function table */
    memset(&gi_impl, 0, sizeof(gi_impl));

    /* Q2-compatible */
    gi_impl.bprintf = GI_bprintf;
    gi_impl.dprintf = GI_dprintf;
    gi_impl.cprintf = GI_cprintf;
    gi_impl.centerprintf = GI_centerprintf;
    gi_impl.sound = GI_sound;
    gi_impl.positioned_sound = GI_positioned_sound;
    gi_impl.configstring = GI_configstring;
    gi_impl.error = GI_error;
    gi_impl.modelindex = GI_modelindex;
    gi_impl.soundindex = GI_soundindex;
    gi_impl.imageindex = GI_imageindex;
    gi_impl.setmodel = GI_setmodel;
    gi_impl.trace = GI_trace;
    gi_impl.pointcontents = GI_pointcontents;
    gi_impl.inPVS = GI_inPVS;
    gi_impl.inPHS = GI_inPHS;
    gi_impl.setorigin = GI_setorigin;
    gi_impl.linkentity = GI_linkentity;
    gi_impl.unlinkentity = GI_unlinkentity;
    gi_impl.BoxEdicts = GI_BoxEdicts;
    gi_impl.AreasConnected = GI_AreasConnected;
    gi_impl.Pmove = GI_Pmove;
    gi_impl.multicast = GI_multicast;
    gi_impl.unicast = GI_unicast;
    gi_impl.WriteChar = GI_WriteChar;
    gi_impl.WriteByte = GI_WriteByte;
    gi_impl.WriteShort = GI_WriteShort;
    gi_impl.WriteLong = GI_WriteLong;
    gi_impl.WriteFloat = GI_WriteFloat;
    gi_impl.WriteString = GI_WriteString;
    gi_impl.WritePosition = GI_WritePosition;
    gi_impl.WriteDir = GI_WriteDir;
    gi_impl.WriteAngle = GI_WriteAngle;
    gi_impl.TagMalloc = GI_TagMalloc;
    gi_impl.TagFree = GI_TagFree;
    gi_impl.FreeTags = GI_FreeTags;
    gi_impl.cvar = GI_cvar;
    gi_impl.cvar_set = GI_cvar_set;
    gi_impl.cvar_forceset = GI_cvar_forceset;
    gi_impl.argc = GI_argc;
    gi_impl.argv = GI_argv;
    gi_impl.args = GI_args;
    gi_impl.AddCommandString = GI_AddCommandString;
    gi_impl.DebugGraph = GI_DebugGraph;

    /* SoF-specific */
    gi_impl.sound_extended = GI_sound_extended;
    gi_impl.ghoul_load_model = GI_ghoul_load_model;
    gi_impl.ghoul_attach_bolt = GI_ghoul_attach_bolt;
    gi_impl.ghoul_set_skin = GI_ghoul_set_skin;
    gi_impl.ghoul_set_anim = GI_ghoul_set_anim;
    gi_impl.ghoul_damage_zone = GI_ghoul_damage_zone;
    gi_impl.ghoul_sever_zone = GI_ghoul_sever_zone;
    gi_impl.entity_set_flags = GI_entity_set_flags;
    gi_impl.flrand = flrand;
    gi_impl.irand = irand;

    /* Initialize game module */
    ge = GetGameAPI(&gi_impl);

    if (!ge) {
        Com_Error(ERR_FATAL, "SV_InitGameProgs: GetGameAPI returned NULL");
        return;
    }

    if (ge->apiversion != GAME_API_VERSION) {
        Com_Error(ERR_FATAL, "SV_InitGameProgs: game API version mismatch (%d vs %d)",
                  ge->apiversion, GAME_API_VERSION);
        return;
    }

    ge->Init();
}

/* ==========================================================================
   SV_ShutdownGameProgs — Shutdown game module
   ========================================================================== */

void SV_ShutdownGameProgs(void)
{
    if (ge) {
        ge->Shutdown();
        ge = NULL;
    }
}

/* ==========================================================================
   Game module accessors for engine use
   ========================================================================== */

game_export_t *SV_GetGameExport(void)
{
    return ge;
}

/*
 * SV_SpawnMapEntities — Called by map command to spawn BSP entities
 */
void SV_SpawnMapEntities(const char *mapname, const char *entstring)
{
    if (ge && ge->SpawnEntities) {
        ge->SpawnEntities(mapname, entstring, "");
    }
}
