/*
 * g_main.c - Game module entry point and core logic
 *
 * In the original SoF, this was gamex86.dll loaded via GetGameAPI().
 * In our unified binary, GetGameAPI is called directly from the engine.
 *
 * Original addresses:
 *   GetGameAPI:       0x50095390
 *   InitGame:         0x50094590  (prints "==== InitGame ====", registers 85 cvars)
 *   ShutdownGame:     0x50095010  (prints "==== ShutdownGame ====")
 *   SpawnEntities:    0x500A59A0
 *   RunFrame:         0x500960E0
 *   RegisterWeapons:  0x50095280
 *   GetGameVersion:   0x500953D0
 */

#include "g_local.h"

/* ==========================================================================
   Globals
   ========================================================================== */

game_import_t   gi;         /* engine functions available to game */
game_export_t   globals;    /* game functions provided to engine */

static edict_t  *g_edicts;
static int      game_maxclients;
static qboolean cheats_enabled;
static int      game_framenum;
static float    game_frametime;

/* ==========================================================================
   Game CVars (85 total registered in original InitGame)
   ========================================================================== */

static cvar_t   *sv_rollspeed;
static cvar_t   *sv_rollangle;
static cvar_t   *sv_maxvelocity;
static cvar_t   *sv_gravity;
static cvar_t   *sv_gravity_x;
static cvar_t   *sv_gravity_y;
static cvar_t   *sv_gravity_z;

static cvar_t   *maxclients;
static cvar_t   *maxspectators;
static cvar_t   *maxentities;
static cvar_t   *deathmatch;
static cvar_t   *coop;
static cvar_t   *skill;
static cvar_t   *fraglimit;
static cvar_t   *timelimit;
static cvar_t   *dmflags;
static cvar_t   *cheats;
static cvar_t   *gamename;
static cvar_t   *gamedate;
static cvar_t   *freezeworld;

/* AI cvars */
static cvar_t   *ai_freeze;
static cvar_t   *ai_goretest;
static cvar_t   *ai_pathtest;
static cvar_t   *ai_dumb;
static cvar_t   *ai_maxcorpses;

/* GHOUL cvars (registered by game, not engine) */
static cvar_t   *ghl_specular;
static cvar_t   *ghl_light_method;
static cvar_t   *ghl_precache_verts;
static cvar_t   *ghl_precache_texture;
static cvar_t   *ghl_mip;

/* CTF cvars */
static cvar_t   *ctf_loops;
static cvar_t   *ctf_team_red;
static cvar_t   *ctf_team_blue;

/* ==========================================================================
   SoF Weapon Names (from RegisterWeapons at 0x50095280)
   ========================================================================== */

static const char *weapon_names[WEAP_COUNT] = {
    "none",
    "knife",
    "pistol1",
    "pistol2",
    "shotgun",
    "machinegun",
    "assault",
    "sniper",
    "slugger",
    "rocket",
    "flamegun",
    "mpg",
    "mpistol",
    "grenade",
    "c4",
    "medkit",
    "goggles",
    "fpak"
};

/* ==========================================================================
   InitGame — Called by engine after GetGameAPI
   Original at 0x50094590, prints "==== InitGame ===="
   Registers all 85 cvars, allocates edict array
   ========================================================================== */

static void InitGame(void)
{
    gi.dprintf("==== InitGame ====\n");

    /* Physics cvars */
    sv_rollspeed = gi.cvar("sv_rollspeed", "200", 0);
    sv_rollangle = gi.cvar("sv_rollangle", "2", 0);
    sv_maxvelocity = gi.cvar("sv_maxvelocity", "2000", 0);
    sv_gravity = gi.cvar("sv_gravity", "800", 0);
    sv_gravity_x = gi.cvar("sv_gravity_x", "0", 0);
    sv_gravity_y = gi.cvar("sv_gravity_y", "0", 0);
    sv_gravity_z = gi.cvar("sv_gravity_z", "-1", 0);

    /* Game rules */
    maxclients = gi.cvar("maxclients", "8", CVAR_SERVERINFO | CVAR_LATCH);
    maxspectators = gi.cvar("maxspectators", "4", CVAR_SERVERINFO);
    maxentities = gi.cvar("maxentities", "1024", CVAR_LATCH);
    deathmatch = gi.cvar("deathmatch", "0", CVAR_LATCH);
    coop = gi.cvar("coop", "0", CVAR_LATCH);
    skill = gi.cvar("skill", "1", CVAR_LATCH);
    fraglimit = gi.cvar("fraglimit", "0", CVAR_SERVERINFO);
    timelimit = gi.cvar("timelimit", "0", CVAR_SERVERINFO);
    dmflags = gi.cvar("dmflags", "0", CVAR_SERVERINFO);
    cheats = gi.cvar("cheats", "0", CVAR_SERVERINFO | CVAR_LATCH);
    gamename = gi.cvar("gamename", "base", CVAR_SERVERINFO | CVAR_LATCH);
    gamedate = gi.cvar("gamedate", "Mar 10 2000", CVAR_SERVERINFO | CVAR_NOSET);
    freezeworld = gi.cvar("freezeworld", "0", 0);

    /* AI cvars */
    ai_freeze = gi.cvar("ai_freeze", "0", 0);
    ai_goretest = gi.cvar("ai_goretest", "0", 0);
    ai_pathtest = gi.cvar("ai_pathtest", "0", 0);
    ai_dumb = gi.cvar("ai_dumb", "0", 0);
    ai_maxcorpses = gi.cvar("ai_maxcorpses", "8", 0);

    /* GHOUL engine cvars (game-side registration) */
    ghl_specular = gi.cvar("ghl_specular", "1", CVAR_ARCHIVE);
    ghl_light_method = gi.cvar("ghl_light_method", "0", CVAR_ARCHIVE);
    ghl_precache_verts = gi.cvar("ghl_precache_verts", "1", CVAR_ARCHIVE);
    ghl_precache_texture = gi.cvar("ghl_precache_texture", "1", CVAR_ARCHIVE);
    ghl_mip = gi.cvar("ghl_mip", "1", CVAR_ARCHIVE);

    /* CTF */
    ctf_loops = gi.cvar("ctf_loops", "0", CVAR_SERVERINFO);
    ctf_team_red = gi.cvar("ctf_team_red", "MeatWagon", CVAR_SERVERINFO);
    ctf_team_blue = gi.cvar("ctf_team_blue", "The Order", CVAR_SERVERINFO);

    cheats_enabled = (qboolean)cheats->value;

    /* Allocate entity array */
    game_maxclients = (int)maxclients->value;
    globals.max_edicts = (int)maxentities->value;

    g_edicts = (edict_t *)gi.TagMalloc(globals.max_edicts * sizeof(edict_t), Z_TAG_GAME);
    memset(g_edicts, 0, globals.max_edicts * sizeof(edict_t));

    globals.edicts = g_edicts;
    globals.edict_size = sizeof(edict_t);
    globals.num_edicts = game_maxclients + 1;  /* world + clients */

    /* Mark client edicts as in-use */
    {
        int i;
        for (i = 0; i < game_maxclients; i++) {
            g_edicts[i + 1].inuse = qtrue;
            g_edicts[i + 1].entity_type = ET_PLAYER;
            g_edicts[i + 1].s.number = i + 1;
        }
    }

    /* World entity */
    g_edicts[0].inuse = qtrue;
    g_edicts[0].s.number = 0;
    g_edicts[0].classname = "worldspawn";

    game_framenum = 0;
    game_frametime = 0.1f;  /* 10 Hz server tick */

    gi.dprintf("  maxclients: %d\n", game_maxclients);
    gi.dprintf("  maxentities: %d\n", globals.max_edicts);
    gi.dprintf("  edict_size: %d bytes\n", (int)sizeof(edict_t));
    gi.dprintf("==== InitGame Complete ====\n");
}

/* ==========================================================================
   ShutdownGame — Cleanup on level change or quit
   Original at 0x50095010, prints "==== ShutdownGame ===="
   ========================================================================== */

static void ShutdownGame(void)
{
    gi.dprintf("==== ShutdownGame ====\n");
    gi.FreeTags(Z_TAG_GAME);
}

/* ==========================================================================
   SpawnEntities — Parse BSP entity string and spawn entities
   Original at 0x500A59A0
   ========================================================================== */

static void SpawnEntities(const char *mapname, const char *entstring,
                          const char *spawnpoint)
{
    /* Clear existing entities (except world + clients) */
    {
        int i;
        for (i = game_maxclients + 1; i < globals.max_edicts; i++) {
            if (g_edicts[i].inuse) {
                memset(&g_edicts[i], 0, sizeof(edict_t));
            }
        }
        globals.num_edicts = game_maxclients + 1;
    }

    /* Parse entity string and spawn entities */
    G_SpawnEntities(mapname, entstring, spawnpoint);
}

/* ==========================================================================
   RunFrame — Server game tick
   Original at 0x500960E0, called every server frame (10 Hz)
   ========================================================================== */

static void RunFrame(void)
{
    int i;
    edict_t *ent;
    float level_time;

    game_framenum++;
    level_time = game_framenum * game_frametime;

    /* Run think functions for all active entities */
    for (i = 0; i < globals.max_edicts; i++) {
        ent = &g_edicts[i];

        if (!ent->inuse)
            continue;

        VectorCopy(ent->s.origin, ent->s.old_origin);

        /* Run prethink (player entities) */
        if (ent->prethink)
            ent->prethink(ent);

        /* Run think function if time has come */
        if (ent->nextthink > 0 && ent->nextthink <= level_time) {
            ent->nextthink = 0;
            if (ent->think)
                ent->think(ent);
            if (!ent->inuse)
                continue;
        }

        /* Run physics based on movetype */
        G_RunEntity(ent);
    }
}

/* ==========================================================================
   Client Functions
   ========================================================================== */

static qboolean ClientConnect(edict_t *ent, char *userinfo)
{
    gi.dprintf("ClientConnect: entity %d\n", ent->s.number);

    /* Extract name from userinfo */
    {
        char *name = Info_ValueForKey(userinfo, "name");
        if (ent->client) {
            Q_strncpyz(ent->client->pers_netname, name,
                       sizeof(ent->client->pers_netname));
            Q_strncpyz(ent->client->pers_userinfo, userinfo,
                       sizeof(ent->client->pers_userinfo));
            ent->client->pers_connected = 1;
        }
    }

    return qtrue;
}

static void ClientBegin(edict_t *ent)
{
    gi.dprintf("ClientBegin: entity %d\n", ent->s.number);

    if (ent->client) {
        ent->client->pers_health = 100;
        ent->client->pers_max_health = 100;
    }

    ent->health = 100;
    ent->max_health = 100;
    ent->takedamage = DAMAGE_AIM;
    ent->entity_type = ET_PLAYER;
    ent->inuse = qtrue;
}

static void ClientUserinfoChanged(edict_t *ent, char *userinfo)
{
    if (ent->client) {
        char *name = Info_ValueForKey(userinfo, "name");
        Q_strncpyz(ent->client->pers_netname, name,
                   sizeof(ent->client->pers_netname));
    }
}

static void ClientCommand(edict_t *ent)
{
    const char *cmd = gi.argv(0);

    if (Q_stricmp(cmd, "say") == 0 || Q_stricmp(cmd, "say_team") == 0) {
        /* TODO: chat implementation */
        return;
    }

    if (Q_stricmp(cmd, "score") == 0) {
        /* TODO: scoreboard */
        return;
    }

    if (Q_stricmp(cmd, "use") == 0) {
        /* TODO: item/weapon use */
        return;
    }

    gi.cprintf(ent, PRINT_ALL, "Unknown command: %s\n", cmd);
}

static void ClientThink(edict_t *ent, usercmd_t *ucmd)
{
    (void)ucmd;

    /* TODO: Player movement
     * 1. Run Pmove with usercmd
     * 2. Update view angles
     * 3. Check weapon fire
     * 4. Update animation state
     */
}

static void ClientDisconnect(edict_t *ent)
{
    gi.dprintf("ClientDisconnect: entity %d\n", ent->s.number);

    if (ent->client) {
        ent->client->pers_connected = 0;
    }
}

/* ==========================================================================
   Save/Load (stubs)
   ========================================================================== */

static void WriteGame(const char *filename, qboolean autosave)
{
    (void)filename;
    (void)autosave;
    gi.dprintf("WriteGame: %s (autosave=%d)\n", filename, autosave);
}

static void ReadGame(const char *filename)
{
    (void)filename;
    gi.dprintf("ReadGame: %s\n", filename);
}

static void WriteLevel(const char *filename)
{
    (void)filename;
    gi.dprintf("WriteLevel: %s\n", filename);
}

static void ReadLevel(const char *filename)
{
    (void)filename;
    gi.dprintf("ReadLevel: %s\n", filename);
}

/* ==========================================================================
   SoF-specific exports
   ========================================================================== */

static void G_GetGameTime(void)
{
    /* Original at 0x50095050 */
}

static void G_RegisterWeapons(void)
{
    int i;
    gi.dprintf("RegisterWeapons:\n");
    for (i = 1; i < WEAP_COUNT; i++) {
        gi.dprintf("  %s\n", weapon_names[i]);
    }
}

static const char *G_GetGameVersion(void)
{
    return "SoF Recomp v0.1.0";
}

static qboolean G_GetCheatsEnabled(void)
{
    return cheats_enabled;
}

static void G_SetCheatsEnabled(qboolean enabled)
{
    cheats_enabled = enabled;
}

static void G_RunAI(void)
{
    /* Original at 0x50095D20 — runs AI/pathfinding tick */
}

/* ==========================================================================
   ServerCommand — "sv" console command handler
   Original at 0x50096160
   ========================================================================== */

static void ServerCommand(void)
{
    const char *cmd = gi.argv(1);

    if (Q_stricmp(cmd, "dumpuser") == 0) {
        /* TODO: dump userinfo for specified client */
    } else if (Q_stricmp(cmd, "addip") == 0) {
        /* TODO: IP ban list */
    } else if (Q_stricmp(cmd, "removeip") == 0) {
        /* TODO: IP ban removal */
    } else {
        gi.dprintf("Unknown server command: %s\n", cmd);
    }
}

/* ==========================================================================
   GetGameAPI — Entry point (was DLL export in original)
   Original at 0x50095390
   Engine passes function pointers in, game returns its function table
   ========================================================================== */

game_export_t *GetGameAPI(game_import_t *import)
{
    gi = *import;

    globals.apiversion = GAME_API_VERSION;
    globals.Init = InitGame;
    globals.Shutdown = ShutdownGame;
    globals.SpawnEntities = SpawnEntities;
    globals.WriteGame = WriteGame;
    globals.ReadGame = ReadGame;
    globals.WriteLevel = WriteLevel;
    globals.ReadLevel = ReadLevel;
    globals.ClientConnect = ClientConnect;
    globals.ClientBegin = ClientBegin;
    globals.ClientUserinfoChanged = ClientUserinfoChanged;
    globals.ClientCommand = ClientCommand;
    globals.ClientThink = ClientThink;
    globals.ClientDisconnect = ClientDisconnect;
    globals.RunFrame = RunFrame;
    globals.ServerCommand = ServerCommand;
    globals.GetGameTime = G_GetGameTime;
    globals.RegisterWeapons = G_RegisterWeapons;
    globals.GetGameVersion = G_GetGameVersion;
    globals.GetCheatsEnabled = G_GetCheatsEnabled;
    globals.SetCheatsEnabled = G_SetCheatsEnabled;
    globals.RunAI = G_RunAI;

    globals.edict_size = sizeof(edict_t);

    return &globals;
}
