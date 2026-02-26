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
#include "../renderer/r_bsp.h"
#include "../renderer/r_local.h"
#include "../sound/snd_local.h"

/* ==========================================================================
   Game Module State
   ========================================================================== */

static game_export_t    *ge;        /* game functions */
static game_import_t    gi_impl;    /* engine functions for game */

/* ==========================================================================
   Configstring Table
   Q2 configstrings: indexed string table sent to clients.
   Ranges: CS_MODELS (32..287), CS_SOUNDS (288..543), CS_IMAGES (544..799)
   ========================================================================== */

static char sv_configstrings[MAX_CONFIGSTRINGS][MAX_QPATH];

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

extern void HUD_SetPickupMessage(const char *msg);

static void GI_cprintf(edict_t *ent, int printlevel, const char *fmt, ...)
{
    va_list argptr;
    char    msg[1024];

    (void)ent;
    (void)printlevel;

    va_start(argptr, fmt);
    vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    /* Route pickup messages to HUD */
    if (strncmp(msg, "Picked up", 9) == 0) {
        /* Strip trailing newline for display */
        char clean[64];
        Q_strncpyz(clean, msg, sizeof(clean));
        {
            int len = (int)strlen(clean);
            if (len > 0 && clean[len - 1] == '\n')
                clean[len - 1] = '\0';
        }
        HUD_SetPickupMessage(clean);
    }

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
    const char *name;
    sfx_t *sfx;
    vec3_t origin;
    int entnum = 0;

    if (soundindex <= 0 || soundindex >= MAX_SOUNDS)
        return;

    name = sv_configstrings[CS_SOUNDS + soundindex];
    if (!name[0])
        return;

    sfx = S_RegisterSound(name);
    if (!sfx)
        return;

    if (ent) {
        entnum = ent->s.number;
        VectorCopy(ent->s.origin, origin);
    } else {
        VectorClear(origin);
    }

    S_StartSound(origin, entnum, channel, sfx, volume, attenuation, timeofs);
}

static void GI_positioned_sound(vec3_t origin, edict_t *ent, int channel,
                     int soundindex, float volume, float attenuation,
                     float timeofs)
{
    const char *name;
    sfx_t *sfx;
    int entnum = 0;

    if (soundindex <= 0 || soundindex >= MAX_SOUNDS)
        return;

    name = sv_configstrings[CS_SOUNDS + soundindex];
    if (!name[0])
        return;

    sfx = S_RegisterSound(name);
    if (!sfx)
        return;

    if (ent)
        entnum = ent->s.number;

    S_StartSound(origin, entnum, channel, sfx, volume, attenuation, timeofs);
}

static void GI_configstring(int num, const char *string)
{
    if (num < 0 || num >= MAX_CONFIGSTRINGS) {
        Com_Error(ERR_DROP, "GI_configstring: bad index %d", num);
        return;
    }

    Q_strncpyz(sv_configstrings[num], string ? string : "", MAX_QPATH);
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

/*
 * SV_FindIndex — generic configstring index lookup/allocation
 * Searches the range [start, start+max) for an existing match or
 * allocates the next empty slot. Returns offset from start (1-based).
 */
static int SV_FindIndex(const char *name, int start, int max)
{
    int i;

    if (!name || !name[0])
        return 0;

    /* Search for existing */
    for (i = 1; i < max; i++) {
        if (sv_configstrings[start + i][0] == '\0')
            break;  /* hit empty slot — not found, allocate here */
        if (Q_stricmp(sv_configstrings[start + i], name) == 0)
            return i;
    }

    if (i >= max) {
        Com_Error(ERR_DROP, "SV_FindIndex: overflow (start=%d max=%d) for '%s'",
                  start, max, name);
        return 0;
    }

    /* Allocate new slot */
    Q_strncpyz(sv_configstrings[start + i], name, MAX_QPATH);
    return i;
}

static int GI_modelindex(const char *name)
{
    return SV_FindIndex(name, CS_MODELS, MAX_MODELS);
}

static int GI_soundindex(const char *name)
{
    return SV_FindIndex(name, CS_SOUNDS, MAX_SOUNDS);
}

static int GI_imageindex(const char *name)
{
    return SV_FindIndex(name, CS_IMAGES, MAX_IMAGES);
}

/* Forward declarations — renderer and entity linking */
extern bsp_world_t *R_GetWorldModel(void);
extern void SV_LinkEdict(edict_t *ent);
extern void SV_UnlinkEdict(edict_t *ent);
extern int  SV_AreaEdicts(vec3_t mins, vec3_t maxs, edict_t **list,
                          int maxcount, int areatype);
extern void SV_ClearWorld(void);

static void GI_setmodel(edict_t *ent, const char *name)
{
    bsp_world_t *world;

    if (!ent || !name)
        return;

    ent->s.modelindex = SV_FindIndex(name, CS_MODELS, MAX_MODELS);

    /* Inline BSP models (*1, *2, etc.) — set bounding box from submodel */
    if (name[0] == '*') {
        int submodel = atoi(name + 1);
        world = R_GetWorldModel();

        if (world && world->loaded && submodel > 0 &&
            submodel < world->num_models) {
            bsp_model_t *mod = &world->models[submodel];
            VectorCopy(mod->mins, ent->mins);
            VectorCopy(mod->maxs, ent->maxs);
            ent->solid = SOLID_BSP;
            SV_LinkEdict(ent);
        }
    }
}

/*
 * SV_ClipTraceToEntity — Clip a trace against a single entity's AABB
 * Returns qtrue if the entity was closer than the current trace.
 */
static qboolean SV_ClipTraceToEntity(trace_t *tr, vec3_t start, vec3_t mins, vec3_t maxs,
                                     vec3_t end, edict_t *ent)
{
    vec3_t ent_mins, ent_maxs;
    float t_entry, t_exit;
    int i;
    vec3_t dir, inv_dir;
    vec3_t expanded_mins, expanded_maxs;

    /* Expand entity bounds by trace AABB */
    for (i = 0; i < 3; i++) {
        expanded_mins[i] = ent->absmin[i] - (maxs ? maxs[i] : 0);
        expanded_maxs[i] = ent->absmax[i] - (mins ? mins[i] : 0);
    }

    VectorSubtract(end, start, dir);

    t_entry = 0.0f;
    t_exit = 1.0f;

    for (i = 0; i < 3; i++) {
        if (dir[i] == 0.0f) {
            /* Ray parallel to slab */
            if (start[i] < expanded_mins[i] || start[i] > expanded_maxs[i])
                return qfalse;
        } else {
            float t1 = (expanded_mins[i] - start[i]) / dir[i];
            float t2 = (expanded_maxs[i] - start[i]) / dir[i];

            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > t_entry) t_entry = t1;
            if (t2 < t_exit) t_exit = t2;

            if (t_entry > t_exit)
                return qfalse;
        }
    }

    /* Hit - check if closer than current trace */
    if (t_entry >= 0.0f && t_entry < tr->fraction) {
        tr->fraction = t_entry;
        for (i = 0; i < 3; i++)
            tr->endpos[i] = start[i] + dir[i] * t_entry;
        tr->ent = ent;

        /* Approximate normal from entry axis */
        memset(&tr->plane, 0, sizeof(tr->plane));
        {
            float best = -1;
            int best_axis = 0;
            for (i = 0; i < 3; i++) {
                float d1 = (float)fabs(tr->endpos[i] - expanded_mins[i]);
                float d2 = (float)fabs(tr->endpos[i] - expanded_maxs[i]);
                float d = d1 < d2 ? d1 : d2;
                if (best < 0 || d < best) {
                    best = d;
                    best_axis = i;
                }
            }
            tr->plane.normal[best_axis] = dir[best_axis] > 0 ? -1.0f : 1.0f;
        }
        return qtrue;
    }

    return qfalse;
}

static trace_t GI_trace(vec3_t start, vec3_t mins, vec3_t maxs,
                        vec3_t end, edict_t *passent, int contentmask)
{
    trace_t tr;
    bsp_world_t *world = R_GetWorldModel();

    /* Trace against BSP world */
    if (world && world->loaded) {
        tr = CM_BoxTrace(world, start, mins, maxs, end, contentmask);
    } else {
        memset(&tr, 0, sizeof(tr));
        tr.fraction = 1.0f;
        VectorCopy(end, tr.endpos);
    }

    /* Trace against solid entities */
    {
        edict_t *touch[64];
        int num_touch, i;
        vec3_t trace_mins, trace_maxs;

        /* Build AABB encompassing entire trace */
        for (i = 0; i < 3; i++) {
            if (start[i] < end[i]) {
                trace_mins[i] = start[i] + (mins ? mins[i] : 0);
                trace_maxs[i] = end[i] + (maxs ? maxs[i] : 0);
            } else {
                trace_mins[i] = end[i] + (mins ? mins[i] : 0);
                trace_maxs[i] = start[i] + (maxs ? maxs[i] : 0);
            }
        }

        num_touch = SV_AreaEdicts(trace_mins, trace_maxs, touch, 64, AREA_SOLID);

        for (i = 0; i < num_touch; i++) {
            edict_t *ent = touch[i];

            if (!ent || ent == passent || !ent->inuse)
                continue;

            /* Skip non-solid entities */
            if (ent->solid == SOLID_NOT || ent->solid == SOLID_TRIGGER)
                continue;

            /* Skip owner */
            if (passent && ent->owner == passent)
                continue;

            SV_ClipTraceToEntity(&tr, start, mins, maxs, end, ent);
        }
    }

    return tr;
}

static int GI_pointcontents(vec3_t point)
{
    bsp_world_t *world = R_GetWorldModel();

    if (world && world->loaded)
        return CM_PointContents(world, point);
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

static void GI_setorigin(edict_t *ent, vec3_t origin)
{
    VectorCopy(origin, ent->s.origin);
    SV_LinkEdict(ent);
}

static void GI_linkentity(edict_t *ent) { SV_LinkEdict(ent); }
static void GI_unlinkentity(edict_t *ent) { SV_UnlinkEdict(ent); }

static int GI_BoxEdicts(vec3_t mins, vec3_t maxs, edict_t **list,
                        int maxcount, int areatype)
{
    return SV_AreaEdicts(mins, maxs, list, maxcount, areatype);
}

static qboolean GI_AreasConnected(int area1, int area2)
{
    (void)area1; (void)area2;
    return qtrue;
}

/* Player movement — forward to engine Pmove */
static void GI_Pmove(void *pmove) { Pmove((pmove_t *)pmove); }

/* Network message buffer — accumulates game's Write* calls until multicast/unicast */
#define SV_MSG_SIZE     2048
static byte     sv_msg_data[SV_MSG_SIZE];
static sizebuf_t sv_msg_buf;
static qboolean sv_msg_initialized = qfalse;

static void SV_EnsureMsgBuf(void)
{
    if (!sv_msg_initialized) {
        SZ_Init(&sv_msg_buf, sv_msg_data, SV_MSG_SIZE);
        sv_msg_initialized = qtrue;
    }
}

static void GI_multicast(vec3_t origin, multicast_t to)
{
    (void)origin; (void)to;
    /* In full implementation, this would send sv_msg_buf to
     * appropriate clients based on multicast type (PVS/PHS/all) */
    SZ_Clear(&sv_msg_buf);
}

static void GI_unicast(edict_t *ent, qboolean reliable)
{
    (void)ent; (void)reliable;
    SZ_Clear(&sv_msg_buf);
}

static void GI_WriteChar(int c)     { SV_EnsureMsgBuf(); MSG_WriteChar(&sv_msg_buf, c); }
static void GI_WriteByte(int c)     { SV_EnsureMsgBuf(); MSG_WriteByte(&sv_msg_buf, c); }
static void GI_WriteShort(int c)    { SV_EnsureMsgBuf(); MSG_WriteShort(&sv_msg_buf, c); }
static void GI_WriteLong(int c)     { SV_EnsureMsgBuf(); MSG_WriteLong(&sv_msg_buf, c); }
static void GI_WriteFloat(float f)  { SV_EnsureMsgBuf(); MSG_WriteFloat(&sv_msg_buf, f); }
static void GI_WriteString(const char *s) { SV_EnsureMsgBuf(); MSG_WriteString(&sv_msg_buf, s); }
static void GI_WritePosition(vec3_t pos) { SV_EnsureMsgBuf(); MSG_WritePos(&sv_msg_buf, pos); }
static void GI_WriteDir(vec3_t dir) { SV_EnsureMsgBuf(); MSG_WriteDir(&sv_msg_buf, dir); }
static void GI_WriteAngle(float f)  { SV_EnsureMsgBuf(); MSG_WriteAngle(&sv_msg_buf, f); }

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

/* SoF extended sound — routes to standard sound with extra flags */
static void GI_sound_extended(edict_t *ent, int channel, int soundindex,
                     float volume, float attenuation, float timeofs,
                     int flags)
{
    (void)flags;  /* TODO: handle SoF-specific sound flags */
    GI_sound(ent, channel, soundindex, volume, attenuation, timeofs);
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
    /* Clear configstring table */
    memset(sv_configstrings, 0, sizeof(sv_configstrings));

    /* Initialize world spatial structure */
    SV_ClearWorld();

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

/* ==========================================================================
   Configstring Accessors (for engine/renderer use)
   ========================================================================== */

const char *SV_GetConfigstring(int index)
{
    if (index < 0 || index >= MAX_CONFIGSTRINGS)
        return "";
    return sv_configstrings[index];
}

void SV_SetConfigstring(int index, const char *val)
{
    if (index < 0 || index >= MAX_CONFIGSTRINGS)
        return;
    Q_strncpyz(sv_configstrings[index], val ? val : "", MAX_QPATH);
}

/*
 * SV_ExecuteClientCommand — Route console "cmd" to game's ClientCommand
 * Called when user types "cmd weapnext" etc.
 */
void SV_ExecuteClientCommand(void)
{
    edict_t *player;

    if (!ge || !ge->ClientCommand || !ge->edicts)
        return;

    player = (edict_t *)((byte *)ge->edicts + ge->edict_size);
    if (player->inuse)
        ge->ClientCommand(player);
}

/*
 * SV_ClientThink — Forward usercmd to game's ClientThink for local player
 * In the unified binary there's no networking, so we call directly.
 */
void SV_ClientThink(usercmd_t *cmd)
{
    edict_t *player;

    if (!ge || !ge->ClientThink || !ge->edicts)
        return;

    /* Player entity is always edict[1] */
    player = (edict_t *)((byte *)ge->edicts + ge->edict_size);
    if (player->inuse)
        ge->ClientThink(player, cmd);
}

/*
 * SV_GetPlayerOrigin — Get player entity position for camera
 */
qboolean SV_GetPlayerState(vec3_t origin, vec3_t angles, float *viewheight)
{
    edict_t *player;

    if (!ge || !ge->edicts)
        return qfalse;

    player = (edict_t *)((byte *)ge->edicts + ge->edict_size);
    if (!player->inuse || !player->client)
        return qfalse;

    VectorCopy(player->s.origin, origin);
    VectorCopy(player->client->viewangles, angles);
    *viewheight = player->client->viewheight;
    return qtrue;
}

/*
 * SV_GetPlayerHealth — Get player health for HUD display
 */
qboolean SV_GetPlayerHealth(int *health, int *max_health)
{
    edict_t *player;

    if (!ge || !ge->edicts)
        return qfalse;

    player = (edict_t *)((byte *)ge->edicts + ge->edict_size);
    if (!player->inuse)
        return qfalse;

    *health = player->health;
    *max_health = player->max_health;
    return qtrue;
}

/*
 * SV_GetPlayerBlend — Get player screen blend color for damage flash
 */
void SV_GetPlayerBlend(float *blend)
{
    edict_t *player;

    blend[0] = blend[1] = blend[2] = blend[3] = 0;

    if (!ge || !ge->edicts)
        return;

    player = (edict_t *)((byte *)ge->edicts + ge->edict_size);
    if (!player->inuse || !player->client)
        return;

    blend[0] = player->client->blend[0];
    blend[1] = player->client->blend[1];
    blend[2] = player->client->blend[2];
    blend[3] = player->client->blend[3];
}

/*
 * SV_GetPlayerWeapon — Get player weapon name for HUD display
 */
const char *SV_GetPlayerWeapon(void)
{
    edict_t *player;

    if (!ge || !ge->edicts)
        return NULL;

    player = (edict_t *)((byte *)ge->edicts + ge->edict_size);
    if (!player->inuse || !player->client)
        return NULL;

    /* Look up weapon name from configstring or return index-based name */
    {
        int weap = player->client->pers_weapon;
        static const char *weapon_hud_names[] = {
            "none", "Knife", ".44 Pistol", "Silver Talon", "Shotgun",
            "MP5", "M4 Assault", "Sniper", "Slugger", "Rocket",
            "Flamethrower", "MPG", "Mach Pistol", "Grenade", "C4",
            "Medkit", "Goggles", "Field Pack"
        };
        if (weap >= 0 && weap < 18)
            return weapon_hud_names[weap];
    }
    return "unknown";
}

/*
 * SV_GetPlayerAmmo — Get player ammo for HUD display
 */
qboolean SV_GetPlayerAmmo(int *ammo, int *ammo_max)
{
    edict_t *player;

    if (!ge || !ge->edicts)
        return qfalse;

    player = (edict_t *)((byte *)ge->edicts + ge->edict_size);
    if (!player->inuse || !player->client)
        return qfalse;

    {
        int weap = player->client->pers_weapon;
        if (weap >= 0 && weap < WEAP_COUNT) {
            *ammo = player->client->ammo[weap];
            *ammo_max = player->client->ammo_max[weap];
        } else {
            *ammo = 0;
            *ammo_max = 0;
        }
    }
    return qtrue;
}

/*
 * SV_RunGameFrame — Called from SV_Frame at 10Hz
 * Drives the game module's RunFrame which iterates all entities.
 */
void SV_RunGameFrame(void)
{
    if (ge && ge->RunFrame) {
        ge->RunFrame();
    }
}

/*
 * SV_SpawnMapEntities — Called by map command to spawn BSP entities
 */
void SV_SpawnMapEntities(const char *mapname, const char *entstring)
{
    /* Re-initialize spatial structure for new map */
    SV_ClearWorld();

    /* Clear configstrings from previous map */
    memset(sv_configstrings, 0, sizeof(sv_configstrings));

    if (ge && ge->SpawnEntities) {
        ge->SpawnEntities(mapname, entstring, "");
    }

    /* Apply sky settings from configstrings set by worldspawn */
    {
        const char *skyname = sv_configstrings[CS_SKY];
        if (skyname[0]) {
            float rotate = 0;
            vec3_t axis = {0, 0, 1};
            const char *rotstr = sv_configstrings[CS_SKYROTATE];
            const char *axisstr = sv_configstrings[CS_SKYAXIS];

            if (rotstr[0])
                rotate = (float)atof(rotstr);
            if (axisstr[0])
                sscanf(axisstr, "%f %f %f", &axis[0], &axis[1], &axis[2]);

            R_SetSky(skyname, rotate, axis);
        }
    }
}
