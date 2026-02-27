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
#include "../ghoul/ghoul.h"

/* Particle/light effects from renderer (unified binary) */
extern void R_ParticleEffect(vec3_t org, vec3_t dir, int type, int count);
extern void R_AddDecal(vec3_t origin, vec3_t normal, int type);
extern void R_AddDlight(vec3_t origin, float r, float g, float b,
                         float intensity, float duration);

/* HUD effects from engine (unified binary) */
extern void SCR_AddDamageNumber(int damage, int screen_x, int screen_y);
extern void SCR_AddPickupMessage(const char *text);
extern void SCR_AddKillFeed(const char *attacker, const char *victim, const char *weapon);
extern void SCR_AddScreenShake(float intensity, float duration);
extern void SCR_TriggerHitMarker(void);
extern void SCR_AddDamageDirection(float angle);
extern void SCR_AddBloodSplatter(int damage);
extern void SCR_AddScorePopup(int score);
extern void R_AddSprite(vec3_t origin, float size, float r, float g, float b,
                         float alpha, float lifetime, float rotation_speed);
extern void R_AddTracer(vec3_t start, vec3_t end, float r, float g, float b);

/* Sound constants now in g_local.h */

/* Forward declarations */
static void G_AngleVectors(vec3_t angles, vec3_t fwd, vec3_t rt, vec3_t up_out);
static void G_FireProjectile(edict_t *ent, qboolean is_grenade);
static void T_RadiusDamage(edict_t *inflictor, edict_t *attacker,
                            float damage, float radius);
void WriteGame(const char *filename, qboolean autosave);
static void ReadGame(const char *filename);
void WriteLevel(const char *filename);
static void ReadLevel(const char *filename);
static void G_UpdateDecals(void);
static void G_AddDecal(vec3_t origin, vec3_t normal, int type);
static void G_SpawnGibs(edict_t *ent, int count);
static void G_DamageDirectionToPlayer(edict_t *player, vec3_t source);
static void G_ExplosionShakeNearby(vec3_t origin, float max_intensity,
                                    float duration, float max_range);
static int weapon_damage[WEAP_COUNT];

/* ==========================================================================
   Globals
   ========================================================================== */

game_import_t   gi;         /* engine functions available to game */
game_export_t   globals;    /* game functions provided to engine */

static edict_t  *g_edicts;
static gclient_t *g_clients;    /* one per maxclients */
static int      game_maxclients;
static qboolean cheats_enabled;

level_t level;

/* Level transition state — preserves player state across map changes */
static struct {
    qboolean    valid;
    int         health;
    int         max_health;
    int         weapon;
    int         armor;
    int         armor_max;
    int         ammo[WEAP_COUNT];
    int         ammo_max[WEAP_COUNT];
    int         magazine[WEAP_COUNT];
    int         kills;
    int         deaths;
    int         score;
} level_transition;

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
cvar_t   *skill;  /* non-static: accessed by g_ai.c for difficulty scaling */
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
   Sound Indices — precached during RegisterWeapons
   ========================================================================== */

static int snd_weapons[WEAP_COUNT];     /* weapon fire sounds */
static int snd_ric1, snd_ric2, snd_ric3;  /* ricochet/impact */
static int snd_hit_flesh;               /* bullet-on-flesh impact */
static int snd_explode;                 /* explosion */
static int snd_noammo;                  /* empty click */
static int snd_weapon_switch;           /* weapon switch sound */
static int snd_footstep1, snd_footstep2, snd_footstep3, snd_footstep4;
static int snd_player_pain1, snd_player_pain2;
static int snd_player_die;
static int snd_drown;
static int snd_heartbeat;               /* low health warning */
static int snd_reload;                  /* reload sound */
static int snd_splash_in;              /* water entry splash */
static int snd_splash_out;             /* water exit splash */
static int snd_ambient_wind;           /* outdoor wind ambience */
static int snd_ambient_drip;           /* water area dripping */
static int snd_ladder_step;            /* ladder climbing sound */

#define WEAPON_SWITCH_TIME  0.5f        /* 500ms weapon switch delay */
static int player_prev_weapon;  /* previous weapon for quick-switch (Q key) */

/* Magazine capacity per weapon (0 = no magazine / unlimited) */
static int weapon_magazine_size[WEAP_COUNT] = {
    0,      /* WEAP_NONE */
    0,      /* WEAP_KNIFE (no reload) */
    7,      /* WEAP_PISTOL1 (.44 Desert Eagle) */
    12,     /* WEAP_PISTOL2 (Silver Talon) */
    8,      /* WEAP_SHOTGUN */
    30,     /* WEAP_MACHINEGUN (MP5) */
    30,     /* WEAP_ASSAULT (M4) */
    5,      /* WEAP_SNIPER (MSG90) */
    10,     /* WEAP_SLUGGER */
    4,      /* WEAP_ROCKET */
    100,    /* WEAP_FLAMEGUN */
    20,     /* WEAP_MPG */
    20,     /* WEAP_MPISTOL */
    0,      /* WEAP_GRENADE (no magazine) */
    0,      /* WEAP_C4 (no magazine) */
    0,      /* WEAP_MEDKIT */
    0,      /* WEAP_GOGGLES */
    0,      /* WEAP_FPAK */
};

/* Reload time per weapon in seconds */
static float weapon_reload_time[WEAP_COUNT] = {
    0,      /* WEAP_NONE */
    0,      /* WEAP_KNIFE */
    1.5f,   /* WEAP_PISTOL1 */
    1.2f,   /* WEAP_PISTOL2 */
    2.5f,   /* WEAP_SHOTGUN */
    2.0f,   /* WEAP_MACHINEGUN */
    2.0f,   /* WEAP_ASSAULT */
    3.0f,   /* WEAP_SNIPER */
    2.0f,   /* WEAP_SLUGGER */
    3.0f,   /* WEAP_ROCKET */
    3.5f,   /* WEAP_FLAMEGUN */
    2.5f,   /* WEAP_MPG */
    1.5f,   /* WEAP_MPISTOL */
    0,      /* WEAP_GRENADE */
    0,      /* WEAP_C4 */
    0,      /* WEAP_MEDKIT */
    0,      /* WEAP_GOGGLES */
    0,      /* WEAP_FPAK */
};

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

    g_clients = (gclient_t *)gi.TagMalloc(game_maxclients * sizeof(gclient_t), Z_TAG_GAME);
    memset(g_clients, 0, game_maxclients * sizeof(gclient_t));

    globals.edicts = g_edicts;
    globals.edict_size = sizeof(edict_t);
    globals.num_edicts = game_maxclients + 1;  /* world + clients */

    /* Mark client edicts as in-use and assign client structs */
    {
        int i;
        for (i = 0; i < game_maxclients; i++) {
            g_edicts[i + 1].inuse = qtrue;
            g_edicts[i + 1].entity_type = ET_PLAYER;
            g_edicts[i + 1].s.number = i + 1;
            g_edicts[i + 1].client = &g_clients[i];
            g_clients[i].ps.gravity = (short)sv_gravity->value;
            g_clients[i].pers_health = 100;
            g_clients[i].pers_max_health = 100;
        }
    }

    /* World entity */
    g_edicts[0].inuse = qtrue;
    g_edicts[0].s.number = 0;
    g_edicts[0].classname = "worldspawn";

    level.framenum = 0;
    level.frametime = 0.1f;  /* 10 Hz server tick */

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

/*
 * G_SaveTransitionState — Save player state before level change
 * Called from changelevel handlers before map switch.
 */
void G_SaveTransitionState(void)
{
    edict_t *player = &globals.edicts[1];
    if (!player->inuse || !player->client) return;

    /* Autosave before level change */
    {
        char gamefile[64], levelfile[64];
        snprintf(gamefile, sizeof(gamefile), "autosave.sav");
        snprintf(levelfile, sizeof(levelfile), "autosave.sv2");
        WriteGame(gamefile, qtrue);
        WriteLevel(levelfile);
        gi.dprintf("Autosave created\n");
        SCR_AddPickupMessage("Autosave...");
    }

    level_transition.valid = qtrue;
    level_transition.health = player->health;
    level_transition.max_health = player->max_health;
    level_transition.weapon = player->client->pers_weapon;
    level_transition.armor = player->client->armor;
    level_transition.armor_max = player->client->armor_max;
    memcpy(level_transition.ammo, player->client->ammo, sizeof(level_transition.ammo));
    memcpy(level_transition.ammo_max, player->client->ammo_max, sizeof(level_transition.ammo_max));
    memcpy(level_transition.magazine, player->client->magazine, sizeof(level_transition.magazine));
    level_transition.kills = player->client->kills;
    level_transition.deaths = player->client->deaths;
    level_transition.score = player->client->score;

    gi.dprintf("Transition state saved\n");
}

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

    /* Initialize level stats */
    level.level_start_time = level.time;
    level.killed_monsters = 0;
    level.found_secrets = 0;
    level.total_monsters = 0;
    level.total_secrets = 0;

    /* Count total monsters and secrets */
    {
        int i;
        for (i = 0; i < globals.num_edicts; i++) {
            edict_t *e = &globals.edicts[i];
            if (!e->inuse) continue;
            if (e->svflags & SVF_MONSTER)
                level.total_monsters++;
            if (e->classname && Q_stricmp(e->classname, "trigger_secret") == 0)
                level.total_secrets++;
        }
        gi.dprintf("Level: %d monsters, %d secrets\n",
                   level.total_monsters, level.total_secrets);
    }
}

/* ==========================================================================
   RunFrame — Server game tick
   Original at 0x500960E0, called every server frame (10 Hz)
   ========================================================================== */

static void RunFrame(void)
{
    int i;
    edict_t *ent;

    level.framenum++;
    level.time = level.framenum * level.frametime;

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
        if (ent->nextthink > 0 && ent->nextthink <= level.time) {
            ent->nextthink = 0;
            if (ent->think)
                ent->think(ent);
            if (!ent->inuse)
                continue;
        }

        /* Run physics based on movetype */
        G_RunEntity(ent);
    }

    /* Update persistent decals */
    G_UpdateDecals();

    /* Process active vote */
    if (level.vote_active) {
        if (level.time >= level.vote_end) {
            /* Vote expired — check result */
            if (level.vote_yes > level.vote_no) {
                gi.bprintf(PRINT_ALL, "Vote PASSED: %s (%d yes, %d no)\n",
                           level.vote_display, level.vote_yes, level.vote_no);
                gi.AddCommandString(level.vote_command);
            } else {
                gi.bprintf(PRINT_ALL, "Vote FAILED: %s (%d yes, %d no)\n",
                           level.vote_display, level.vote_yes, level.vote_no);
            }
            level.vote_active = qfalse;
        }
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
        ent->client->armor = 0;
        ent->client->armor_max = 200;
        ent->client->air_finished = 0;
        ent->client->next_env_damage = 0;
        ent->client->next_pain_sound = 0;
        ent->client->reloading_weapon = 0;
        ent->client->reload_finish_time = 0;
        ent->client->goggles_on = qfalse;
        ent->client->goggles_battery = 100.0f;
        ent->client->fpak_count = 0;
        ent->client->fpak_heal_remaining = 0;
        ent->client->fpak_heal_end = 0;
        ent->client->invuln_time = 0;

        /* Initialize magazines to full for all weapons */
        {
            int w;
            for (w = 0; w < WEAP_COUNT; w++)
                ent->client->magazine[w] = weapon_magazine_size[w];
        }
    }

    ent->health = 100;
    ent->max_health = 100;
    ent->takedamage = DAMAGE_AIM;
    ent->entity_type = ET_PLAYER;
    ent->inuse = qtrue;

    /* Restore transition state if coming from a level change */
    if (level_transition.valid && ent->client) {
        ent->health = level_transition.health;
        ent->max_health = level_transition.max_health;
        ent->client->pers_health = level_transition.health;
        ent->client->pers_max_health = level_transition.max_health;
        ent->client->pers_weapon = level_transition.weapon;
        ent->weapon_index = level_transition.weapon;
        ent->client->armor = level_transition.armor;
        ent->client->armor_max = level_transition.armor_max;
        memcpy(ent->client->ammo, level_transition.ammo, sizeof(level_transition.ammo));
        memcpy(ent->client->ammo_max, level_transition.ammo_max, sizeof(level_transition.ammo_max));
        memcpy(ent->client->magazine, level_transition.magazine, sizeof(level_transition.magazine));
        ent->client->kills = level_transition.kills;
        ent->client->deaths = level_transition.deaths;
        ent->client->score = level_transition.score;

        level_transition.valid = qfalse;
        gi.dprintf("Transition state restored\n");
    }
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
        const char *item = gi.args();
        int i;

        /* Match weapon name to weapon_id */
        for (i = 1; i < WEAP_COUNT; i++) {
            if (Q_stricmp(item, weapon_names[i]) == 0) {
                ent->client->pers_weapon = i;
                ent->weapon_index = i;
                ent->client->weapon_change_time = level.time + WEAPON_SWITCH_TIME;
                if (snd_weapon_switch)
                    gi.sound(ent, CHAN_ITEM, snd_weapon_switch, 1.0f, ATTN_NORM, 0);
                gi.cprintf(ent, PRINT_ALL, "Switched to %s\n", weapon_names[i]);
                return;
            }
        }
        gi.cprintf(ent, PRINT_ALL, "Unknown weapon: %s\n", item);
        return;
    }

    if (Q_stricmp(cmd, "weapnext") == 0) {
        int w = ent->client->pers_weapon + 1;
        if (w >= WEAP_COUNT) w = 1;
        player_prev_weapon = ent->client->pers_weapon;
        ent->client->pers_weapon = w;
        ent->weapon_index = w;
        ent->client->weapon_change_time = level.time + WEAPON_SWITCH_TIME;
        /* Cancel zoom on weapon switch */
        if (ent->client->zoomed) {
            ent->client->zoomed = qfalse;
            ent->client->fov = 90.0f;
        }
        if (snd_weapon_switch)
            gi.sound(ent, CHAN_ITEM, snd_weapon_switch, 1.0f, ATTN_NORM, 0);
        gi.cprintf(ent, PRINT_ALL, "Weapon: %s\n", weapon_names[w]);
        return;
    }

    if (Q_stricmp(cmd, "weapprev") == 0) {
        int w = ent->client->pers_weapon - 1;
        if (w < 1) w = WEAP_COUNT - 1;
        player_prev_weapon = ent->client->pers_weapon;
        ent->client->pers_weapon = w;
        ent->weapon_index = w;
        ent->client->weapon_change_time = level.time + WEAPON_SWITCH_TIME;
        /* Cancel zoom on weapon switch */
        if (ent->client->zoomed) {
            ent->client->zoomed = qfalse;
            ent->client->fov = 90.0f;
        }
        if (snd_weapon_switch)
            gi.sound(ent, CHAN_ITEM, snd_weapon_switch, 1.0f, ATTN_NORM, 0);
        gi.cprintf(ent, PRINT_ALL, "Weapon: %s\n", weapon_names[w]);
        return;
    }

    if (Q_stricmp(cmd, "weaplast") == 0) {
        if (player_prev_weapon > 0 && player_prev_weapon < WEAP_COUNT &&
            player_prev_weapon != ent->client->pers_weapon) {
            int cur = ent->client->pers_weapon;
            ent->client->pers_weapon = player_prev_weapon;
            ent->weapon_index = player_prev_weapon;
            player_prev_weapon = cur;
            ent->client->weapon_change_time = level.time + WEAPON_SWITCH_TIME;
            if (ent->client->zoomed) { ent->client->zoomed = qfalse; ent->client->fov = 90.0f; }
            if (snd_weapon_switch) gi.sound(ent, CHAN_ITEM, snd_weapon_switch, 1.0f, ATTN_NORM, 0);
            gi.cprintf(ent, PRINT_ALL, "Weapon: %s\n", weapon_names[ent->client->pers_weapon]);
        }
        return;
    }

    if (Q_stricmp(cmd, "weapon") == 0 && gi.argc() >= 3) {
        int slot = atoi(gi.argv(2));
        if (slot > 0 && slot < WEAP_COUNT && slot != ent->client->pers_weapon) {
            player_prev_weapon = ent->client->pers_weapon;
            ent->client->pers_weapon = slot;
            ent->weapon_index = slot;
            ent->client->weapon_change_time = level.time + WEAPON_SWITCH_TIME;
            if (ent->client->zoomed) { ent->client->zoomed = qfalse; ent->client->fov = 90.0f; }
            if (snd_weapon_switch) gi.sound(ent, CHAN_ITEM, snd_weapon_switch, 1.0f, ATTN_NORM, 0);
            gi.cprintf(ent, PRINT_ALL, "Weapon: %s\n", weapon_names[slot]);
        }
        return;
    }

    if (Q_stricmp(cmd, "flashlight") == 0 || Q_stricmp(cmd, "light") == 0) {
        ent->client->flashlight_on = !ent->client->flashlight_on;
        gi.cprintf(ent, PRINT_ALL, "Flashlight: %s\n",
                   ent->client->flashlight_on ? "ON" : "OFF");
        return;
    }

    if (Q_stricmp(cmd, "weather") == 0) {
        const char *arg = gi.argv(1);
        if (arg && arg[0]) {
            if (Q_stricmp(arg, "rain") == 0) {
                level.weather = 1;
                level.weather_density = 1.0f;
            } else if (Q_stricmp(arg, "snow") == 0) {
                level.weather = 2;
                level.weather_density = 1.0f;
            } else if (Q_stricmp(arg, "off") == 0 || Q_stricmp(arg, "none") == 0) {
                level.weather = 0;
            } else {
                level.weather = atoi(arg);
                if (level.weather < 0) level.weather = 0;
                if (level.weather > 2) level.weather = 2;
                if (level.weather > 0 && level.weather_density < 0.1f)
                    level.weather_density = 1.0f;
            }
        } else {
            /* Toggle: none → rain → snow → none */
            level.weather = (level.weather + 1) % 3;
            if (level.weather > 0 && level.weather_density < 0.1f)
                level.weather_density = 1.0f;
        }
        gi.cprintf(ent, PRINT_ALL, "Weather: %s\n",
                   level.weather == 1 ? "rain" :
                   level.weather == 2 ? "snow" : "none");
        return;
    }

    if (Q_stricmp(cmd, "stats") == 0 || Q_stricmp(cmd, "levelstats") == 0) {
        float elapsed = level.time - level.level_start_time;
        int mins = (int)(elapsed / 60.0f);
        int secs = (int)elapsed % 60;
        gi.cprintf(ent, PRINT_ALL, "--- Level Stats ---\n");
        gi.cprintf(ent, PRINT_ALL, "Monsters: %d / %d\n",
                   level.killed_monsters, level.total_monsters);
        gi.cprintf(ent, PRINT_ALL, "Secrets:  %d / %d\n",
                   level.found_secrets, level.total_secrets);
        gi.cprintf(ent, PRINT_ALL, "Time:     %d:%02d\n", mins, secs);
        if (ent->client)
            gi.cprintf(ent, PRINT_ALL, "Score:    %d\n", ent->client->score);
        return;
    }

    if (Q_stricmp(cmd, "zoom") == 0 || Q_stricmp(cmd, "+zoom") == 0) {
        if (ent->client->pers_weapon == WEAP_SNIPER) {
            ent->client->zoomed = !ent->client->zoomed;
            if (ent->client->zoomed) {
                ent->client->zoom_fov = 30.0f;
                ent->client->fov = 30.0f;
                gi.cprintf(ent, PRINT_ALL, "Scope: ON\n");
            } else {
                ent->client->fov = 90.0f;
                gi.cprintf(ent, PRINT_ALL, "Scope: OFF\n");
            }
        } else {
            gi.cprintf(ent, PRINT_ALL, "No scope on this weapon\n");
        }
        return;
    }

    if (Q_stricmp(cmd, "-zoom") == 0) {
        if (ent->client->zoomed) {
            ent->client->zoomed = qfalse;
            ent->client->fov = 90.0f;
        }
        return;
    }

    if (Q_stricmp(cmd, "reload") == 0) {
        int w = ent->client->pers_weapon;
        int mag_size = (w > 0 && w < WEAP_COUNT) ? weapon_magazine_size[w] : 0;

        if (mag_size <= 0) {
            gi.cprintf(ent, PRINT_ALL, "This weapon doesn't use magazines\n");
            return;
        }
        if (ent->client->reloading_weapon) {
            gi.cprintf(ent, PRINT_ALL, "Already reloading\n");
            return;
        }
        if (ent->client->magazine[w] >= mag_size) {
            gi.cprintf(ent, PRINT_ALL, "Magazine full\n");
            return;
        }
        if (ent->client->ammo[w] <= 0) {
            gi.cprintf(ent, PRINT_ALL, "No ammo\n");
            return;
        }

        ent->client->reloading_weapon = w;
        ent->client->reload_finish_time = level.time + weapon_reload_time[w];
        if (snd_reload)
            gi.sound(ent, CHAN_WEAPON, snd_reload, 1.0f, ATTN_NORM, 0);
        gi.cprintf(ent, PRINT_ALL, "Reloading %s...\n", weapon_names[w]);
        return;
    }

    if (Q_stricmp(cmd, "dualwield") == 0) {
        /* Toggle dual wielding — requires ammo for both pistols */
        if (ent->client->ammo[WEAP_PISTOL1] > 0 && ent->client->ammo[WEAP_PISTOL2] > 0) {
            ent->client->dual_wield = !ent->client->dual_wield;
            if (ent->client->dual_wield) {
                ent->client->pers_weapon = WEAP_PISTOL1;
                gi.cprintf(ent, PRINT_ALL, "Dual wielding: ON\n");
                SCR_AddPickupMessage("Dual Wield!");
            } else {
                gi.cprintf(ent, PRINT_ALL, "Dual wielding: OFF\n");
                SCR_AddPickupMessage("Single Wield");
            }
        } else {
            gi.cprintf(ent, PRINT_ALL, "Need both pistols for dual wield\n");
        }
        return;
    }

    if (Q_stricmp(cmd, "holster") == 0) {
        if (ent->client->weapon_holstered) {
            /* Unholster — restore previous weapon */
            ent->client->weapon_holstered = qfalse;
            ent->client->pers_weapon = ent->client->holster_weapon;
            ent->weapon_index = ent->client->holster_weapon;
            ent->client->weapon_change_time = level.time + WEAPON_SWITCH_TIME;
            if (snd_weapon_switch)
                gi.sound(ent, CHAN_ITEM, snd_weapon_switch, 1.0f, ATTN_NORM, 0);
            gi.cprintf(ent, PRINT_ALL, "Weapon drawn\n");
        } else {
            /* Holster weapon — stow for faster movement */
            ent->client->holster_weapon = ent->client->pers_weapon;
            ent->client->weapon_holstered = qtrue;
            if (ent->client->zoomed) {
                ent->client->zoomed = qfalse;
                ent->client->fov = 90.0f;
            }
            gi.cprintf(ent, PRINT_ALL, "Weapon holstered — move faster\n");
        }
        return;
    }

    if (Q_stricmp(cmd, "attach") == 0 && gi.argc() >= 3) {
        const char *att_name = gi.argv(2);
        int weap = ent->client->pers_weapon;
        int flag = 0;

        if (Q_stricmp(att_name, "silencer") == 0)      flag = ATTACH_SILENCER;
        else if (Q_stricmp(att_name, "scope") == 0)     flag = ATTACH_SCOPE;
        else if (Q_stricmp(att_name, "extmag") == 0)    flag = ATTACH_EXTMAG;
        else if (Q_stricmp(att_name, "laser") == 0)     flag = ATTACH_LASER;

        if (flag && weap > 0 && weap < WEAP_COUNT) {
            ent->client->attachments[weap] ^= flag;  /* toggle */
            if (ent->client->attachments[weap] & flag) {
                gi.cprintf(ent, PRINT_ALL, "Attached %s\n", att_name);
                SCR_AddPickupMessage(att_name);
            } else {
                gi.cprintf(ent, PRINT_ALL, "Removed %s\n", att_name);
            }
        } else {
            gi.cprintf(ent, PRINT_ALL, "Usage: attach <silencer|scope|extmag|laser>\n");
        }
        return;
    }

    if (Q_stricmp(cmd, "speedrun") == 0) {
        if (level.speedrun_active) {
            float elapsed = level.time - level.speedrun_start;
            int minutes = (int)(elapsed / 60.0f);
            float seconds = elapsed - minutes * 60.0f;
            char timebuf[64];
            snprintf(timebuf, sizeof(timebuf), "Time: %d:%05.2f", minutes, seconds);
            level.speedrun_active = qfalse;
            level.speedrun_start = 0;
            gi.cprintf(ent, PRINT_ALL, "Speedrun stopped! %s\n", timebuf);
            SCR_AddPickupMessage(timebuf);
        } else {
            level.speedrun_active = qtrue;
            level.speedrun_start = level.time;
            gi.cprintf(ent, PRINT_ALL, "Speedrun timer started!\n");
            SCR_AddPickupMessage("SPEEDRUN GO!");
        }
        return;
    }

    if (Q_stricmp(cmd, "save") == 0) {
        const char *savename = gi.argc() > 1 ? gi.argv(1) : "quick";
        char gamefile[256], levelfile[256];
        snprintf(gamefile, sizeof(gamefile), "%s.sav", savename);
        snprintf(levelfile, sizeof(levelfile), "%s.sv2", savename);
        WriteGame(gamefile, qfalse);
        WriteLevel(levelfile);
        gi.cprintf(ent, PRINT_ALL, "Saved: %s\n", savename);
        SCR_AddPickupMessage("Game Saved");
        return;
    }

    if (Q_stricmp(cmd, "load") == 0) {
        const char *savename = gi.argc() > 1 ? gi.argv(1) : "quick";
        char gamefile[256], levelfile[256];
        snprintf(gamefile, sizeof(gamefile), "%s.sav", savename);
        snprintf(levelfile, sizeof(levelfile), "%s.sv2", savename);
        ReadGame(gamefile);
        ReadLevel(levelfile);
        gi.cprintf(ent, PRINT_ALL, "Loaded: %s\n", savename);
        SCR_AddPickupMessage("Game Loaded");
        return;
    }

    if (Q_stricmp(cmd, "spawn_monster") == 0) {
        /* Debug: spawn a soldier 128 units in front of player */
        extern void SP_monster_soldier(edict_t *ent, void *pairs, int num_pairs);
        int idx;

        for (idx = game_maxclients + 1; idx < globals.max_edicts; idx++) {
            if (!globals.edicts[idx].inuse)
                break;
        }
        if (idx < globals.max_edicts) {
            edict_t *monster = &globals.edicts[idx];
            vec3_t forward, right, up_dir;

            memset(monster, 0, (size_t)globals.edict_size);
            monster->inuse = qtrue;
            globals.num_edicts = (idx + 1 > globals.num_edicts) ? idx + 1 : globals.num_edicts;

            G_AngleVectors(ent->client->viewangles, forward, right, up_dir);
            VectorMA(ent->s.origin, 128, forward, monster->s.origin);

            SP_monster_soldier(monster, NULL, 0);
            gi.cprintf(ent, PRINT_ALL, "Spawned monster_soldier at (%.0f %.0f %.0f)\n",
                       monster->s.origin[0], monster->s.origin[1], monster->s.origin[2]);
        } else {
            gi.cprintf(ent, PRINT_ALL, "No free edicts\n");
        }
        return;
    }

    /* Chat: broadcast message to all clients */
    if (Q_stricmp(cmd, "say") == 0) {
        const char *msg = gi.args();
        if (msg && msg[0]) {
            char chat_buf[256];
            extern void Chat_AddMessage(const char *text);
            snprintf(chat_buf, sizeof(chat_buf), "Player: %s", msg);
            /* Broadcast to all players */
            gi.bprintf(PRINT_CHAT, "%s\n", chat_buf);
            /* Add to engine chat overlay */
            Chat_AddMessage(chat_buf);
        }
        return;
    }

    if (Q_stricmp(cmd, "say_team") == 0) {
        const char *msg = gi.args();
        if (msg && msg[0]) {
            char chat_buf[256];
            extern void Chat_AddMessage(const char *text);
            snprintf(chat_buf, sizeof(chat_buf), "[TEAM] Player: %s", msg);
            gi.bprintf(PRINT_CHAT, "%s\n", chat_buf);
            Chat_AddMessage(chat_buf);
        }
        return;
    }

    /* Spectator mode toggle */
    if (Q_stricmp(cmd, "spectate") == 0 || Q_stricmp(cmd, "spectator") == 0) {
        if (ent->client->ps.pm_type == PM_SPECTATOR) {
            /* Leave spectator mode */
            ent->client->ps.pm_type = PM_NORMAL;
            ent->solid = SOLID_BBOX;
            ent->health = 100;
            ent->max_health = 100;
            ent->client->pers_health = 100;
            ent->takedamage = DAMAGE_AIM;
            gi.cprintf(ent, PRINT_ALL, "Spectator mode: OFF\n");
        } else {
            /* Enter spectator mode */
            ent->client->ps.pm_type = PM_SPECTATOR;
            ent->solid = SOLID_NOT;
            ent->takedamage = DAMAGE_NO;
            ent->deadflag = 0;
            gi.cprintf(ent, PRINT_ALL, "Spectator mode: ON\n");
        }
        gi.linkentity(ent);
        return;
    }

    /* Team selection for team deathmatch */
    if (Q_stricmp(cmd, "team") == 0) {
        const char *team_name = gi.argc() > 1 ? gi.argv(1) : "";
        if (Q_stricmp(team_name, "red") == 0 || Q_stricmp(team_name, "1") == 0) {
            ent->client->team = 1;
            gi.cprintf(ent, PRINT_ALL, "Joined Red Team\n");
        } else if (Q_stricmp(team_name, "blue") == 0 || Q_stricmp(team_name, "2") == 0) {
            ent->client->team = 2;
            gi.cprintf(ent, PRINT_ALL, "Joined Blue Team\n");
        } else {
            gi.cprintf(ent, PRINT_ALL, "Usage: team <red|blue>\n");
            gi.cprintf(ent, PRINT_ALL, "Current team: %s\n",
                       ent->client->team == 1 ? "Red" :
                       ent->client->team == 2 ? "Blue" : "None");
        }
        return;
    }

    /* Vote system: callvote <map|restart|kick> <arg> */
    if (Q_stricmp(cmd, "callvote") == 0) {
        const char *vote_type = gi.argc() > 1 ? gi.argv(1) : "";
        const char *vote_arg = gi.argc() > 2 ? gi.argv(2) : "";

        if (level.vote_active) {
            gi.cprintf(ent, PRINT_ALL, "A vote is already in progress.\n");
            return;
        }

        if (Q_stricmp(vote_type, "map") == 0 && vote_arg[0]) {
            snprintf(level.vote_command, sizeof(level.vote_command), "map %s", vote_arg);
            snprintf(level.vote_display, sizeof(level.vote_display), "Change map to %s", vote_arg);
        } else if (Q_stricmp(vote_type, "restart") == 0) {
            snprintf(level.vote_command, sizeof(level.vote_command), "map_restart");
            snprintf(level.vote_display, sizeof(level.vote_display), "Restart map");
        } else if (Q_stricmp(vote_type, "kick") == 0 && vote_arg[0]) {
            snprintf(level.vote_command, sizeof(level.vote_command), "kick %s", vote_arg);
            snprintf(level.vote_display, sizeof(level.vote_display), "Kick player %s", vote_arg);
        } else {
            gi.cprintf(ent, PRINT_ALL, "Usage: callvote <map|restart|kick> [arg]\n");
            return;
        }

        level.vote_active = qtrue;
        level.vote_end = level.time + 30.0f;  /* 30 second vote */
        level.vote_yes = 1;  /* caller votes yes */
        level.vote_no = 0;
        level.vote_caller = (int)(ent - globals.edicts);

        /* Reset all vote flags */
        {
            int i;
            for (i = 1; i <= game_maxclients; i++) {
                if (globals.edicts[i].inuse && globals.edicts[i].client)
                    globals.edicts[i].client->has_voted = qfalse;
            }
        }
        ent->client->has_voted = qtrue;

        gi.bprintf(PRINT_ALL, "Vote called: %s (30s to vote)\n", level.vote_display);
        gi.bprintf(PRINT_ALL, "Type 'vote yes' or 'vote no'\n");
        return;
    }

    if (Q_stricmp(cmd, "vote") == 0) {
        const char *choice = gi.argc() > 1 ? gi.argv(1) : "";

        if (!level.vote_active) {
            gi.cprintf(ent, PRINT_ALL, "No vote in progress.\n");
            return;
        }
        if (ent->client->has_voted) {
            gi.cprintf(ent, PRINT_ALL, "You already voted.\n");
            return;
        }

        ent->client->has_voted = qtrue;
        if (Q_stricmp(choice, "yes") == 0 || Q_stricmp(choice, "1") == 0) {
            level.vote_yes++;
            gi.cprintf(ent, PRINT_ALL, "Vote: YES (%d/%d)\n", level.vote_yes, level.vote_yes + level.vote_no);
        } else {
            level.vote_no++;
            gi.cprintf(ent, PRINT_ALL, "Vote: NO (%d/%d)\n", level.vote_no, level.vote_yes + level.vote_no);
        }
        return;
    }

    /* Armor-piercing rounds toggle */
    if (Q_stricmp(cmd, "ap") == 0 || Q_stricmp(cmd, "armorpiercing") == 0) {
        ent->client->ap_rounds = !ent->client->ap_rounds;
        gi.cprintf(ent, PRINT_ALL, "Armor-piercing: %s\n",
                   ent->client->ap_rounds ? "ON" : "OFF");
        if (ent->client->ap_rounds)
            SCR_AddPickupMessage("AP ROUNDS");
        return;
    }

    /* Revive downed teammate — look at them and press USE */
    if (Q_stricmp(cmd, "revive") == 0) {
        extern game_export_t globals;
        extern int game_maxclients;
        vec3_t forward2, rt2, up2, eye;
        trace_t rev_tr;
        int j;

        G_AngleVectors(ent->client->viewangles, forward2, rt2, up2);
        VectorCopy(ent->s.origin, eye);
        eye[2] += ent->client->viewheight;

        {
            vec3_t rev_end;
            VectorMA(eye, 96, forward2, rev_end);
            rev_tr = gi.trace(eye, NULL, NULL, rev_end, ent, MASK_SHOT);
        }

        if (rev_tr.ent && rev_tr.ent->client && rev_tr.ent->client->downed) {
            /* Revive this player */
            rev_tr.ent->client->downed = qfalse;
            rev_tr.ent->health = 30;
            rev_tr.ent->client->pers_health = 30;
            rev_tr.ent->deadflag = 0;
            rev_tr.ent->client->ps.pm_type = PM_NORMAL;
            rev_tr.ent->takedamage = DAMAGE_AIM;
            gi.cprintf(ent, PRINT_ALL, "Teammate revived!\n");
            gi.cprintf(rev_tr.ent, PRINT_ALL, "You have been revived!\n");
            SCR_AddPickupMessage("REVIVE!");
        } else {
            gi.cprintf(ent, PRINT_ALL, "No downed teammate in front of you.\n");
        }
        return;
    }

    /* Alternate fire mode toggle */
    if (Q_stricmp(cmd, "altfire") == 0 || Q_stricmp(cmd, "+altfire") == 0) {
        ent->client->alt_fire = !ent->client->alt_fire;
        gi.cprintf(ent, PRINT_ALL, "Fire mode: %s\n",
                   ent->client->alt_fire ? "ALTERNATE" : "PRIMARY");
        if (ent->client->alt_fire)
            SCR_AddPickupMessage("ALT FIRE");
        return;
    }

    /* Weapon inspect animation */
    if (Q_stricmp(cmd, "inspect") == 0) {
        int w = ent->client->pers_weapon;
        if (ent->client->inspect_end > level.time) {
            gi.cprintf(ent, PRINT_ALL, "Already inspecting.\n");
            return;
        }
        ent->client->inspect_end = level.time + 2.0f;

        /* Show detailed weapon info */
        if (w > 0 && w < WEAP_COUNT) {
            int att = ent->client->attachments[w];
            gi.cprintf(ent, PRINT_ALL,
                "--- %s ---\n"
                "Ammo: %d/%d  Mag: %d\n"
                "Damage: %d  Heat: %.0f%%\n"
                "Attachments:%s%s%s%s\n",
                weapon_names[w],
                ent->client->ammo[w], ent->client->ammo_max[w],
                ent->client->magazine[w],
                weapon_damage[w],
                ent->client->weapon_heat * 100.0f,
                (att & ATTACH_SILENCER) ? " Silencer" : "",
                (att & ATTACH_SCOPE) ? " Scope" : "",
                (att & ATTACH_EXTMAG) ? " ExtMag" : "",
                (att & ATTACH_LASER) ? " Laser" : "");
        }
        return;
    }

    /* Slow-motion dive: bullet time + jump for cinematic dive */
    if (Q_stricmp(cmd, "flashbang") == 0) {
        if (ent->deadflag)
            return;

        /* Throw a flashbang — blinds all entities within 400 units in LOS */
        {
            extern game_export_t globals;
            vec3_t fwd_f;
            vec3_t bang_origin;
            int snd, i;

            G_AngleVectors(ent->client->viewangles, fwd_f, NULL, NULL);
            bang_origin[0] = ent->s.origin[0] + fwd_f[0] * 200.0f;
            bang_origin[1] = ent->s.origin[1] + fwd_f[1] * 200.0f;
            bang_origin[2] = ent->s.origin[2] + fwd_f[2] * 200.0f + 40.0f;

            /* Flash effect — bright white particles */
            {
                vec3_t up = {0, 0, 1};
                R_ParticleEffect(bang_origin, up, 0, 20);  /* white burst */
            }

            snd = gi.soundindex("weapons/flashbang.wav");
            if (snd)
                gi.sound(ent, CHAN_WEAPON, snd, 1.0f, ATTN_NONE, 0);

            /* Blind nearby entities */
            for (i = 1; i < globals.num_edicts; i++) {
                edict_t *e = &globals.edicts[i];
                vec3_t d;
                float dist;
                trace_t tr;

                if (e == ent || !e->inuse || e->health <= 0)
                    continue;

                VectorSubtract(e->s.origin, bang_origin, d);
                dist = VectorLength(d);
                if (dist > 400.0f)
                    continue;

                /* LOS check */
                tr = gi.trace(bang_origin, NULL, NULL, e->s.origin,
                               ent, MASK_OPAQUE);
                if (tr.fraction < 1.0f && tr.ent != e)
                    continue;

                /* Scale duration by distance (closer = longer) */
                {
                    float dur = 3.0f * (1.0f - dist / 400.0f);
                    if (dur < 0.5f) dur = 0.5f;

                    if (e->client) {
                        e->client->flash_end = level.time + dur;
                    } else if (e->svflags & SVF_MONSTER) {
                        /* Stun monsters: stop them briefly */
                        e->nextthink = level.time + dur;
                    }
                }
            }

            SCR_AddPickupMessage("Flashbang!");
        }
        return;
    }

    if (Q_stricmp(cmd, "nightvision") == 0 || Q_stricmp(cmd, "nv") == 0) {
        ent->client->nightvision = !ent->client->nightvision;
        {
            int snd = gi.soundindex(ent->client->nightvision ?
                                     "items/nv_on.wav" : "items/nv_off.wav");
            if (snd)
                gi.sound(ent, CHAN_ITEM, snd, 1.0f, ATTN_NORM, 0);
        }
        gi.cprintf(ent, PRINT_ALL, "Night vision %s\n",
                   ent->client->nightvision ? "ON" : "OFF");
        return;
    }

    if (Q_stricmp(cmd, "+leanleft") == 0) {
        ent->client->lean_state = -1;
        return;
    }
    if (Q_stricmp(cmd, "+leanright") == 0) {
        ent->client->lean_state = 1;
        return;
    }
    if (Q_stricmp(cmd, "-leanleft") == 0 || Q_stricmp(cmd, "-leanright") == 0) {
        ent->client->lean_state = 0;
        return;
    }

    if (Q_stricmp(cmd, "dive") == 0) {
        if (ent->client->dive_end > level.time || ent->deadflag)
            return;
        if (ent->client->bullet_time_charge < 20.0f) {
            gi.cprintf(ent, PRINT_ALL, "Not enough bullet time charge (need 20).\n");
            return;
        }

        /* Calculate dive direction from view */
        {
            vec3_t fwd_d, rt_d, up_d;
            G_AngleVectors(ent->client->viewangles, fwd_d, rt_d, up_d);

            ent->client->dive_end = level.time + 0.6f;
            VectorCopy(fwd_d, ent->client->dive_dir);
            ent->client->dive_dir[2] = 0;
            VectorNormalize(ent->client->dive_dir);

            /* Launch player forward + slightly up */
            ent->velocity[0] = ent->client->dive_dir[0] * 400.0f;
            ent->velocity[1] = ent->client->dive_dir[1] * 400.0f;
            ent->velocity[2] = 150.0f;

            /* Activate slow-motion */
            level.time_scale = 0.3f;
            ent->client->bullet_time_charge -= 20.0f;
            SCR_AddPickupMessage("DIVE!");
        }
        return;
    }

    /* Weapon quick-slots: bind and recall */
    if (Q_stricmp(cmd, "quickslot") == 0) {
        int slot = atoi(gi.argv(1));
        if (slot >= 1 && slot <= 4) {
            /* Bind current weapon to this slot */
            ent->client->quickslot[slot - 1] = ent->client->pers_weapon;
            gi.cprintf(ent, PRINT_ALL, "Slot %d: %s\n", slot,
                       weapon_names[ent->client->pers_weapon]);
        } else {
            gi.cprintf(ent, PRINT_ALL, "Usage: quickslot <1-4>\n");
        }
        return;
    }

    if (Q_stricmp(cmd, "slot1") == 0 || Q_stricmp(cmd, "slot2") == 0 ||
        Q_stricmp(cmd, "slot3") == 0 || Q_stricmp(cmd, "slot4") == 0) {
        int slot = cmd[4] - '1';  /* 0-3 */
        if (slot >= 0 && slot < 4 && ent->client->quickslot[slot] > 0) {
            int w = ent->client->quickslot[slot];
            if (w < WEAP_COUNT && w != ent->client->pers_weapon) {
                player_prev_weapon = ent->client->pers_weapon;
                ent->client->pers_weapon = w;
                ent->weapon_index = w;
                ent->client->weapon_change_time = level.time + WEAPON_SWITCH_TIME;
                if (snd_weapon_switch)
                    gi.sound(ent, CHAN_ITEM, snd_weapon_switch, 1.0f, ATTN_NORM, 0);
                gi.cprintf(ent, PRINT_ALL, "Slot %d: %s\n", slot + 1, weapon_names[w]);
            }
        } else {
            gi.cprintf(ent, PRINT_ALL, "Slot %d is empty. Use 'quickslot %d' to bind.\n",
                       slot + 1, slot + 1);
        }
        return;
    }

    if (Q_stricmp(cmd, "status") == 0 || Q_stricmp(cmd, "stats") == 0) {
        float accuracy = (ent->client->shots_fired > 0) ?
            (float)ent->client->shots_hit / (float)ent->client->shots_fired * 100.0f : 0;
        gi.cprintf(ent, PRINT_ALL,
            "--- Player Status ---\n"
            "Health: %d/%d  Armor: %d/%d\n"
            "Kills: %d  Deaths: %d  Score: %d\n"
            "Accuracy: %.1f%% (%d/%d)  Headshots: %d\n"
            "Streak: %d  Rank: %d  XP: %d\n",
            ent->health, ent->client->pers_max_health,
            ent->client->armor, ent->client->armor_max,
            ent->client->kills, ent->client->deaths, ent->client->score,
            accuracy, ent->client->shots_hit, ent->client->shots_fired,
            ent->client->headshots,
            ent->client->streak_count, ent->client->rank, ent->client->xp);
        return;
    }

    if (Q_stricmp(cmd, "objective") == 0 || Q_stricmp(cmd, "obj") == 0) {
        if (level.objective_active) {
            gi.cprintf(ent, PRINT_ALL, "Objective: %s (%d/%d complete)\n",
                       level.objective_text,
                       level.objectives_completed, level.objectives_total);
        } else {
            gi.cprintf(ent, PRINT_ALL, "No active objective.\n");
        }
        return;
    }

    if (Q_stricmp(cmd, "commands") == 0 || Q_stricmp(cmd, "cmdlist") == 0) {
        gi.cprintf(ent, PRINT_ALL,
            "--- Available Commands ---\n"
            "use <weapon>    - Switch to weapon\n"
            "weapnext/prev   - Cycle weapons\n"
            "weaplast        - Last weapon\n"
            "flashlight      - Toggle flashlight\n"
            "zoom            - Toggle scope\n"
            "reload          - Reload weapon\n"
            "holster         - Holster/unholster\n"
            "bullettime      - Activate bullet time\n"
            "sprint          - Toggle sprint\n"
            "slide           - Power slide\n"
            "dive            - Slow-mo dive\n"
            "inspect         - Inspect weapon\n"
            "nightvision     - Toggle NV goggles\n"
            "flashbang       - Throw flashbang\n"
            "+leanleft/right - Lean\n"
            "altfire         - Toggle alt fire\n"
            "quickslot <1-4> - Bind weapon\n"
            "slot1-4         - Recall weapon\n"
            "status          - Show stats\n"
            "objective       - Show objective\n");
        return;
    }

    if (Q_stricmp(cmd, "unjam") == 0 || Q_stricmp(cmd, "clearjam") == 0) {
        if (ent->client->weapon_jammed) {
            ent->client->weapon_jammed = qfalse;
            ent->client->jam_clear_time = level.time + 1.5f;  /* 1.5s unjam animation */
            gi.cprintf(ent, PRINT_ALL, "Clearing weapon jam...\n");
            {
                int snd = gi.soundindex("weapons/reload.wav");
                if (snd)
                    gi.sound(ent, CHAN_WEAPON, snd, 1.0f, ATTN_NORM, 0);
            }
        } else {
            gi.cprintf(ent, PRINT_ALL, "Weapon is not jammed.\n");
        }
        return;
    }

    if (Q_stricmp(cmd, "plantc4") == 0 || Q_stricmp(cmd, "c4") == 0) {
        if (ent->deadflag)
            return;
        if (ent->client->c4_entity) {
            /* Detonate existing C4 */
            edict_t *c4 = ent->client->c4_entity;
            if (c4->inuse) {
                vec3_t up = {0, 0, 1};
                R_ParticleEffect(c4->s.origin, up, 2, 32);
                R_ParticleEffect(c4->s.origin, up, 10, 16);
                R_AddDlight(c4->s.origin, 1.0f, 0.6f, 0.1f, 500.0f, 0.5f);
                {
                    int snd = gi.soundindex("weapons/explode.wav");
                    if (snd)
                        gi.sound(c4, CHAN_AUTO, snd, 1.0f, ATTN_NONE, 0);
                }
                SCR_AddScreenShake(0.5f, 0.3f);
                T_RadiusDamage(c4, ent, 150.0f, 300.0f);
                c4->inuse = qfalse;
                c4->solid = SOLID_NOT;
                c4->svflags |= SVF_NOCLIENT;
                gi.linkentity(c4);
            }
            ent->client->c4_entity = NULL;
            SCR_AddPickupMessage("C4 DETONATED!");
        } else {
            /* Plant C4 at current position */
            edict_t *c4 = G_AllocEdict();
            if (c4) {
                c4->classname = "planted_c4";
                c4->solid = SOLID_NOT;
                c4->movetype = MOVETYPE_NONE;
                VectorCopy(ent->s.origin, c4->s.origin);
                c4->s.origin[2] -= 20.0f;  /* place on ground */
                c4->owner = ent;
                gi.linkentity(c4);

                ent->client->c4_entity = c4;
                ent->client->c4_arm_time = level.time;

                {
                    int snd = gi.soundindex("weapons/c4_plant.wav");
                    if (snd)
                        gi.sound(ent, CHAN_ITEM, snd, 1.0f, ATTN_NORM, 0);
                }
                SCR_AddPickupMessage("C4 planted. Use 'c4' again to detonate.");
            }
        }
        return;
    }

    if (Q_stricmp(cmd, "challenge") == 0) {
        if (ent->client->challenge_end > level.time) {
            gi.cprintf(ent, PRINT_ALL, "Challenge active: %d/%d kills, %.0fs remaining\n",
                       ent->client->challenge_kills, ent->client->challenge_target,
                       ent->client->challenge_end - level.time);
        } else {
            /* Start a new challenge: 10 kills in 60 seconds */
            ent->client->challenge_end = level.time + 60.0f;
            ent->client->challenge_kills = 0;
            ent->client->challenge_target = 10;
            gi.cprintf(ent, PRINT_ALL, "CHALLENGE: Get 10 kills in 60 seconds!\n");
            SCR_AddPickupMessage("CHALLENGE STARTED: 10 kills in 60s!");
        }
        return;
    }

    if (Q_stricmp(cmd, "taunt") == 0) {
        static const char *taunts[] = {
            "Come get some!", "Is that all you got?",
            "You call that a fight?", "Too easy.",
            "Bring it on!", "Nice try."
        };
        int idx = gi.irand(0, 5);
        gi.cprintf(ent, PRINT_ALL, "%s: %s\n",
                   ent->client->pers_netname, taunts[idx]);
        SCR_AddPickupMessage(taunts[idx]);
        {
            int snd = gi.soundindex("player/taunt.wav");
            if (snd)
                gi.sound(ent, CHAN_VOICE, snd, 1.0f, ATTN_NORM, 0);
        }
        return;
    }

    /* Bayonet melee — quick melee with bayonet attachment on assault/machinegun */
    if (Q_stricmp(cmd, "bayonet") == 0 || Q_stricmp(cmd, "melee") == 0) {
        int w = ent->client->pers_weapon;
        if (w > 0 && w < WEAP_COUNT &&
            (ent->client->attachments[w] & ATTACH_BAYONET)) {
            vec3_t fwd_b, start_b, end_b;
            trace_t tr_b;

            G_AngleVectors(ent->client->viewangles, fwd_b, NULL, NULL);
            VectorCopy(ent->s.origin, start_b);
            start_b[2] += ent->client->viewheight;
            VectorMA(start_b, 80.0f, fwd_b, end_b);

            tr_b = gi.trace(start_b, NULL, NULL, end_b, ent, MASK_SHOT);
            if (tr_b.fraction < 1.0f && tr_b.ent &&
                tr_b.ent->takedamage && tr_b.ent->health > 0) {
                int bay_dmg = 45;  /* bayonet does more than knife */
                tr_b.ent->health -= bay_dmg;
                R_ParticleEffect(tr_b.endpos, tr_b.plane.normal, 1, 10);
                SCR_AddDamageNumber(bay_dmg, 0, 0);
                SCR_TriggerHitMarker();
                if (tr_b.ent->health <= 0 && tr_b.ent->die)
                    tr_b.ent->die(tr_b.ent, ent, ent, bay_dmg, tr_b.endpos);
            }
            {
                int snd = gi.soundindex("weapons/knife_hit.wav");
                if (snd) gi.sound(ent, CHAN_WEAPON, snd, 1.0f, ATTN_NORM, 0);
            }
            ent->client->weapon_change_time = level.time + 0.5f; /* melee cooldown */
        } else {
            gi.cprintf(ent, PRINT_ALL, "No bayonet attached to this weapon.\n");
        }
        return;
    }

    /* Drop current weapon on the ground */
    if (Q_stricmp(cmd, "drop") == 0 || Q_stricmp(cmd, "dropweapon") == 0) {
        int w = ent->client->pers_weapon;
        if (w > WEAP_KNIFE && w < WEAP_COUNT) {
            extern edict_t *G_DropItem(vec3_t origin, const char *classname);
            char drop_name[64];
            snprintf(drop_name, sizeof(drop_name), "weapon_%s", weapon_names[w]);
            G_DropItem(ent->s.origin, drop_name);
            gi.cprintf(ent, PRINT_ALL, "Dropped %s\n", weapon_names[w]);
            ent->client->ammo[w] = 0;
            ent->client->magazine[w] = 0;
            /* Switch to knife */
            ent->client->pers_weapon = WEAP_KNIFE;
            ent->weapon_index = WEAP_KNIFE;
            ent->client->weapon_change_time = level.time + WEAPON_SWITCH_TIME;
            if (snd_weapon_switch)
                gi.sound(ent, CHAN_ITEM, snd_weapon_switch, 1.0f, ATTN_NORM, 0);
        } else {
            gi.cprintf(ent, PRINT_ALL, "Can't drop this weapon.\n");
        }
        return;
    }

    /* Lastweapon alias for weaplast */
    if (Q_stricmp(cmd, "lastweapon") == 0) {
        if (player_prev_weapon > 0 && player_prev_weapon < WEAP_COUNT &&
            player_prev_weapon != ent->client->pers_weapon) {
            int cur = ent->client->pers_weapon;
            ent->client->pers_weapon = player_prev_weapon;
            ent->weapon_index = player_prev_weapon;
            player_prev_weapon = cur;
            ent->client->weapon_change_time = level.time + WEAPON_SWITCH_TIME;
            if (snd_weapon_switch)
                gi.sound(ent, CHAN_ITEM, snd_weapon_switch, 1.0f, ATTN_NORM, 0);
            gi.cprintf(ent, PRINT_ALL, "Weapon: %s\n",
                       weapon_names[ent->client->pers_weapon]);
        }
        return;
    }

    gi.cprintf(ent, PRINT_ALL, "Unknown command: %s\n", cmd);
}

/* ==========================================================================
   Utility
   ========================================================================== */

static void G_AngleVectors(vec3_t angles, vec3_t fwd, vec3_t rt, vec3_t up_out)
{
    float angle, sr, sp, sy, cr, cp, cy;

    angle = angles[1] * (3.14159265f / 180.0f);
    sy = (float)sin(angle); cy = (float)cos(angle);
    angle = angles[0] * (3.14159265f / 180.0f);
    sp = (float)sin(angle); cp = (float)cos(angle);
    angle = angles[2] * (3.14159265f / 180.0f);
    sr = (float)sin(angle); cr = (float)cos(angle);

    if (fwd) { fwd[0] = cp * cy; fwd[1] = cp * sy; fwd[2] = -sp; }
    if (rt) { rt[0] = -sr * sp * cy + cr * -(-sy); rt[1] = -sr * sp * sy + cr * cy; rt[2] = -sr * cp; }
    if (up_out) { up_out[0] = cr * sp * cy + -sr * -(-sy); up_out[1] = cr * sp * sy + -sr * cy; up_out[2] = cr * cp; }
}

/* Weapon damage table (per hit) */
static int weapon_damage[WEAP_COUNT] = {
    0,      /* WEAP_NONE */
    30,     /* WEAP_KNIFE */
    40,     /* WEAP_PISTOL1 (.44 Desert Eagle) */
    25,     /* WEAP_PISTOL2 (Silver Talon) */
    80,     /* WEAP_SHOTGUN (Pump) */
    15,     /* WEAP_MACHINEGUN (MP5) */
    20,     /* WEAP_ASSAULT (M4) */
    90,     /* WEAP_SNIPER (MSG90) */
    60,     /* WEAP_SLUGGER */
    120,    /* WEAP_ROCKET */
    10,     /* WEAP_FLAMEGUN */
    50,     /* WEAP_MPG */
    12,     /* WEAP_MPISTOL */
    100,    /* WEAP_GRENADE */
    150,    /* WEAP_C4 */
    0,      /* WEAP_MEDKIT */
    0,      /* WEAP_GOGGLES */
    0,      /* WEAP_FPAK */
};

/* Fire rate: minimum time between shots (in game time seconds) */
static float weapon_firerate[WEAP_COUNT] = {
    0,      /* WEAP_NONE */
    0.4f,   /* WEAP_KNIFE */
    0.3f,   /* WEAP_PISTOL1 */
    0.15f,  /* WEAP_PISTOL2 */
    0.8f,   /* WEAP_SHOTGUN */
    0.1f,   /* WEAP_MACHINEGUN */
    0.1f,   /* WEAP_ASSAULT */
    1.0f,   /* WEAP_SNIPER */
    0.5f,   /* WEAP_SLUGGER */
    0.8f,   /* WEAP_ROCKET */
    0.05f,  /* WEAP_FLAMEGUN */
    0.4f,   /* WEAP_MPG */
    0.08f,  /* WEAP_MPISTOL */
    1.0f,   /* WEAP_GRENADE */
    1.0f,   /* WEAP_C4 */
    0.5f,   /* WEAP_MEDKIT */
    0,      /* WEAP_GOGGLES */
    0,      /* WEAP_FPAK */
};

/* Weapon recoil (vertical kick in degrees per shot) */
static float weapon_recoil[WEAP_COUNT] = {
    0,      /* WEAP_NONE */
    0.5f,   /* WEAP_KNIFE */
    3.0f,   /* WEAP_PISTOL1 */
    1.5f,   /* WEAP_PISTOL2 */
    6.0f,   /* WEAP_SHOTGUN */
    1.2f,   /* WEAP_MACHINEGUN */
    1.5f,   /* WEAP_ASSAULT */
    4.0f,   /* WEAP_SNIPER */
    3.0f,   /* WEAP_SLUGGER */
    5.0f,   /* WEAP_ROCKET */
    0.3f,   /* WEAP_FLAMEGUN */
    2.5f,   /* WEAP_MPG */
    0.8f,   /* WEAP_MPISTOL */
    0,      /* WEAP_GRENADE */
    0,      /* WEAP_C4 */
    0,      /* WEAP_MEDKIT */
    0,      /* WEAP_GOGGLES */
    0,      /* WEAP_FPAK */
};

/* Weapon spread (base inaccuracy in radians, increases with sustained fire) */
static float weapon_spread[WEAP_COUNT] = {
    0,      /* WEAP_NONE */
    0,      /* WEAP_KNIFE */
    0.01f,  /* WEAP_PISTOL1 */
    0.015f, /* WEAP_PISTOL2 */
    0,      /* WEAP_SHOTGUN (handled by pellet spread) */
    0.02f,  /* WEAP_MACHINEGUN */
    0.015f, /* WEAP_ASSAULT */
    0.002f, /* WEAP_SNIPER */
    0.01f,  /* WEAP_SLUGGER */
    0,      /* WEAP_ROCKET */
    0.03f,  /* WEAP_FLAMEGUN */
    0.01f,  /* WEAP_MPG */
    0.025f, /* WEAP_MPISTOL */
    0,      /* WEAP_GRENADE */
    0,      /* WEAP_C4 */
    0,      /* WEAP_MEDKIT */
    0,      /* WEAP_GOGGLES */
    0,      /* WEAP_FPAK */
};

static float player_next_fire;  /* level.time when player can fire again */
float player_last_fire_time;    /* level.time when player last fired (for view kick) */
static qboolean player_alt_fire;  /* true if alt-fire mode active */

/*
 * G_UseUtilityWeapon — Handle non-hitscan weapons (medkit, etc.)
 * Returns qtrue if handled (caller should not fire hitscan).
 */
static qboolean G_UseUtilityWeapon(edict_t *ent)
{
    int weap = ent->client->pers_weapon;

    if (weap == WEAP_MEDKIT) {
        if (ent->health >= ent->max_health)
            return qtrue;  /* already full */
        if (ent->client->ammo[WEAP_MEDKIT] <= 0)
            return qtrue;  /* no charges */

        ent->client->ammo[WEAP_MEDKIT]--;
        ent->health += 25;
        if (ent->health > ent->max_health)
            ent->health = ent->max_health;
        ent->client->pers_health = ent->health;

        /* Green heal flash */
        ent->client->blend[0] = 0.0f;
        ent->client->blend[1] = 0.8f;
        ent->client->blend[2] = 0.0f;
        ent->client->blend[3] = 0.2f;

        gi.cprintf(ent, PRINT_ALL, "Used medkit (+25 HP)\n");
        return qtrue;
    }

    /* C4 explosive: place on first use, detonate on second */
    if (weap == WEAP_C4) {
        if (ent->client->ammo[WEAP_C4] <= 0) {
            /* No C4 charges — try to detonate any placed ones */
            int i;
            qboolean detonated = qfalse;
            for (i = 0; i < globals.max_edicts; i++) {
                edict_t *c4 = &globals.edicts[i];
                if (c4->inuse && c4->owner == ent &&
                    c4->classname && Q_stricmp(c4->classname, "c4_charge") == 0) {
                    /* Detonate this C4 */
                    VectorCopy(c4->s.origin, c4->s.origin);
                    R_ParticleEffect(c4->s.origin, c4->s.angles, 2, 48);    /* fire burst */
                    R_ParticleEffect(c4->s.origin, c4->s.angles, 11, 20);   /* debris */
                    R_ParticleEffect(c4->s.origin, c4->s.angles, 10, 10);   /* smoke cloud */
                    R_ParticleEffect(c4->s.origin, c4->s.angles, 13, 12);   /* ground dust */
                    R_AddDlight(c4->s.origin, 1.0f, 0.6f, 0.1f, 500.0f, 0.6f);
                    R_AddSprite(c4->s.origin, 56.0f, 1.0f, 0.7f, 0.2f, 1.0f, 0.8f, 150.0f);  /* fireball */
                    R_AddSprite(c4->s.origin, 80.0f, 0.4f, 0.4f, 0.4f, 0.6f, 1.5f, 20.0f);   /* smoke */
                    if (snd_explode)
                        gi.positioned_sound(c4->s.origin, NULL, CHAN_AUTO,
                                            snd_explode, 1.0f, ATTN_NORM, 0);
                    T_RadiusDamage(c4, ent, (float)c4->dmg, (float)c4->dmg_radius);
                    G_ExplosionShakeNearby(c4->s.origin, 1.0f, 0.5f, 512.0f);
                    c4->inuse = qfalse;
                    gi.unlinkentity(c4);
                    detonated = qtrue;
                }
            }
            if (!detonated && snd_noammo)
                gi.sound(ent, CHAN_WEAPON, snd_noammo, 0.5f, ATTN_NORM, 0);
            return qtrue;
        }

        /* Place C4 charge in front of player */
        {
            edict_t *c4;
            vec3_t fwd, rt, up_v, place;

            ent->client->ammo[WEAP_C4]--;
            c4 = G_AllocEdict();
            if (!c4) return qtrue;

            G_AngleVectors(ent->client->viewangles, fwd, rt, up_v);
            VectorCopy(ent->s.origin, place);
            place[2] += ent->client->viewheight;
            VectorMA(place, 32, fwd, place);

            c4->classname = "c4_charge";
            c4->owner = ent;
            c4->solid = SOLID_NOT;
            c4->movetype = MOVETYPE_NONE;
            c4->dmg = weapon_damage[WEAP_C4];
            c4->dmg_radius = 250;
            VectorCopy(place, c4->s.origin);

            gi.linkentity(c4);
            gi.cprintf(ent, PRINT_ALL, "C4 placed. Use again to detonate.\n");
            if (snd_weapons[WEAP_C4])
                gi.sound(ent, CHAN_WEAPON, snd_weapons[WEAP_C4], 1.0f, ATTN_NORM, 0);
        }
        return qtrue;
    }

    /* Projectile weapons: rocket launcher, grenade */
    if (weap == WEAP_ROCKET || weap == WEAP_GRENADE) {
        if (ent->client->ammo[weap] <= 0) {
            if (snd_noammo)
                gi.sound(ent, CHAN_WEAPON, snd_noammo, 0.5f, ATTN_NORM, 0);
            return qtrue;
        }
        ent->client->ammo[weap]--;
        if (snd_weapons[weap])
            gi.sound(ent, CHAN_WEAPON, snd_weapons[weap], 1.0f, ATTN_NORM, 0);
        G_FireProjectile(ent, (qboolean)(weap == WEAP_GRENADE));
        return qtrue;
    }

    /* Flamethrower: short-range particle stream */
    if (weap == WEAP_FLAMEGUN) {
        vec3_t fwd, rt, up_v, start, end_pt;
        trace_t ftr;

        if (ent->client->ammo[WEAP_FLAMEGUN] <= 0) {
            if (snd_noammo)
                gi.sound(ent, CHAN_WEAPON, snd_noammo, 0.5f, ATTN_NORM, 0);
            return qtrue;
        }
        ent->client->ammo[WEAP_FLAMEGUN]--;

        G_AngleVectors(ent->client->viewangles, fwd, rt, up_v);
        VectorCopy(ent->s.origin, start);
        start[2] += ent->client->viewheight;
        VectorMA(start, 384, fwd, end_pt);  /* 384 unit range */

        /* Fire particles along path */
        {
            vec3_t flame_pt;
            VectorMA(start, 32, fwd, flame_pt);
            R_ParticleEffect(flame_pt, fwd, 4, 6);
            R_AddDlight(flame_pt, 1.0f, 0.5f, 0.0f, 150.0f, 0.1f);
        }

        if (snd_weapons[WEAP_FLAMEGUN])
            gi.sound(ent, CHAN_WEAPON, snd_weapons[WEAP_FLAMEGUN], 0.8f, ATTN_NORM, 0);

        ftr = gi.trace(start, NULL, NULL, end_pt, ent, MASK_SHOT);
        if (ftr.fraction < 1.0f && ftr.ent && ftr.ent->takedamage && ftr.ent->health > 0) {
            int fdmg = weapon_damage[WEAP_FLAMEGUN];
            R_ParticleEffect(ftr.endpos, ftr.plane.normal, 4, 4);
            ftr.ent->health -= fdmg;

            /* Flamethrower causes burning for 4 seconds */
            if (ftr.ent->client && ftr.ent->health > 0) {
                ftr.ent->client->burn_end = level.time + 4.0f;
                ftr.ent->client->burn_next_tick = level.time + 0.5f;
            }

            if (ftr.ent->health <= 0 && ftr.ent->die)
                ftr.ent->die(ftr.ent, ent, ent, fdmg, ftr.endpos);
        }
        return qtrue;
    }

    /* Night vision goggles: toggle on/off */
    if (weap == WEAP_GOGGLES) {
        if (ent->client->goggles_battery <= 0 && !ent->client->goggles_on) {
            gi.cprintf(ent, PRINT_ALL, "Goggles battery dead\n");
            return qtrue;
        }
        ent->client->goggles_on = !ent->client->goggles_on;
        gi.cprintf(ent, PRINT_ALL, "Night vision: %s\n",
                   ent->client->goggles_on ? "ON" : "OFF");
        return qtrue;
    }

    /* Field pack: start trickle heal */
    if (weap == WEAP_FPAK) {
        if (ent->client->fpak_count <= 0) {
            gi.cprintf(ent, PRINT_ALL, "No field packs\n");
            return qtrue;
        }
        if (ent->health >= ent->max_health) {
            gi.cprintf(ent, PRINT_ALL, "Health full\n");
            return qtrue;
        }
        if (ent->client->fpak_heal_remaining > 0) {
            gi.cprintf(ent, PRINT_ALL, "Already healing\n");
            return qtrue;
        }
        ent->client->fpak_count--;
        ent->client->fpak_heal_remaining = 25;
        ent->client->fpak_heal_end = level.time + 3.0f;

        /* Mild green flash */
        ent->client->blend[0] = 0.0f;
        ent->client->blend[1] = 0.6f;
        ent->client->blend[2] = 0.0f;
        ent->client->blend[3] = 0.15f;

        gi.cprintf(ent, PRINT_ALL, "Using field pack (+25 HP over 3s)\n");
        return qtrue;
    }

    if (weap == WEAP_NONE)
        return qtrue;

    return qfalse;
}

/*
 * G_DamageDirectionToPlayer — Show damage direction indicator on HUD
 * Computes angle from source to player relative to player's view.
 */
static void G_DamageDirectionToPlayer(edict_t *player, vec3_t source)
{
    vec3_t dir;
    float dx, dy, yaw, view_yaw, angle;

    if (!player || !player->client)
        return;

    dx = source[0] - player->s.origin[0];
    dy = source[1] - player->s.origin[1];

    /* World yaw from source to player (atan2 gives angle in Quake coords) */
    yaw = atan2f(dy, dx) * 180.0f / 3.14159265f;
    view_yaw = player->client->viewangles[1];

    /* Angle relative to player view: 0=front, 90=left, 180=behind, 270=right */
    angle = yaw - view_yaw + 180.0f;
    while (angle < 0) angle += 360.0f;
    while (angle >= 360.0f) angle -= 360.0f;

    SCR_AddDamageDirection(angle);
}

/*
 * G_ExplosionShakeNearby — Distance-based screen shake for explosions.
 * Closer explosions produce stronger shakes; beyond max_range, no shake.
 */
static void G_ExplosionShakeNearby(vec3_t origin, float max_intensity,
                                    float duration, float max_range)
{
    edict_t *player = &globals.edicts[1];
    vec3_t diff;
    float dist, scale;

    if (!player || !player->inuse || !player->client)
        return;

    VectorSubtract(player->s.origin, origin, diff);
    dist = VectorLength(diff);

    if (dist >= max_range)
        return;

    /* Inverse-linear falloff: full shake at distance 0, none at max_range */
    scale = 1.0f - (dist / max_range);
    if (scale < 0.05f)
        return;

    SCR_AddScreenShake(max_intensity * scale, duration * scale);
}

/*
 * T_RadiusDamage — Deal damage to all entities within a radius
 */
static void T_RadiusDamage(edict_t *inflictor, edict_t *attacker,
                            float damage, float radius)
{
    edict_t *touch[64];
    int num, i;
    vec3_t dmg_mins, dmg_maxs;

    VectorSet(dmg_mins, inflictor->s.origin[0] - radius,
                        inflictor->s.origin[1] - radius,
                        inflictor->s.origin[2] - radius);
    VectorSet(dmg_maxs, inflictor->s.origin[0] + radius,
                        inflictor->s.origin[1] + radius,
                        inflictor->s.origin[2] + radius);

    num = gi.BoxEdicts(dmg_mins, dmg_maxs, touch, 64, AREA_SOLID);
    for (i = 0; i < num; i++) {
        edict_t *t = touch[i];
        vec3_t diff;
        float dist, dmg_applied;

        if (!t || t == inflictor || !t->inuse || !t->takedamage)
            continue;

        /* Distance-based falloff */
        VectorSubtract(t->s.origin, inflictor->s.origin, diff);
        dist = VectorLength(diff);
        if (dist > radius)
            continue;

        dmg_applied = damage * (1.0f - dist / radius);
        if (dmg_applied < 1)
            continue;

        /* Spawn invulnerability — skip damage */
        if (t->client && t->client->invuln_time > level.time)
            continue;

        /* Armor absorbs for players */
        if (t->client && t->client->armor > 0) {
            int absorb = (int)(dmg_applied * 0.66f);
            if (absorb > t->client->armor) absorb = t->client->armor;
            t->client->armor -= absorb;
            dmg_applied -= absorb;
        }

        t->health -= (int)dmg_applied;

        /* Explosion knockback — push entities away from blast */
        {
            vec3_t knockback_dir;
            float knockback_force;

            VectorCopy(diff, knockback_dir);
            if (dist < 1.0f) {
                knockback_dir[0] = 0; knockback_dir[1] = 0; knockback_dir[2] = 1.0f;
            } else {
                VectorScale(knockback_dir, 1.0f / dist, knockback_dir);
            }

            /* Force scales inversely with distance, proportional to damage */
            knockback_force = dmg_applied * 8.0f;
            if (knockback_force > 600.0f) knockback_force = 600.0f;

            t->velocity[0] += knockback_dir[0] * knockback_force;
            t->velocity[1] += knockback_dir[1] * knockback_force;
            t->velocity[2] += knockback_dir[2] * knockback_force * 0.5f + 80.0f;
        }

        /* Pain flash for players */
        if (t->client) {
            t->client->blend[0] = 1.0f;
            t->client->blend[1] = 0.0f;
            t->client->blend[2] = 0.0f;
            t->client->blend[3] = 0.3f;
            t->client->pers_health = t->health;
            G_DamageDirectionToPlayer(t, inflictor->s.origin);
            SCR_AddBloodSplatter((int)dmg_applied);

            /* Explosion causes burning for 3 seconds */
            if (t->health > 0 && dmg_applied > 10.0f) {
                t->client->burn_end = level.time + 3.0f;
                t->client->burn_next_tick = level.time + 0.5f;
            }

            /* Flashbang effect: very close explosions cause white flash */
            if (dist < radius * 0.3f && t->health > 0) {
                t->client->blend[0] = 1.0f;
                t->client->blend[1] = 1.0f;
                t->client->blend[2] = 1.0f;
                t->client->blend[3] = 0.8f;  /* intense white flash */
                /* View punch from concussion */
                t->client->kick_angles[0] += gi.flrand(-5.0f, 5.0f);
                t->client->kick_angles[1] += gi.flrand(-5.0f, 5.0f);
            }

            /* Concussion effect: medium-range explosions cause disorientation */
            if (dist < radius * 0.6f && t->health > 0) {
                t->client->concussion_end = level.time + 2.0f;  /* 2s of sway */
            }
        }

        if (t->health <= 0 && t->die) {
            /* Gib on overkill from explosions */
            if (t->health < -40)
                G_SpawnGibs(t, 4 + gi.irand(0, 4));
            t->die(t, inflictor, attacker, (int)dmg_applied, inflictor->s.origin);
        }
    }
}

/*
 * Rocket/missile think — move and check for impact
 */
static void rocket_think(edict_t *self)
{
    trace_t tr;
    vec3_t end;

    /* Move forward */
    VectorMA(self->s.origin, level.frametime, self->velocity, end);
    tr = gi.trace(self->s.origin, self->mins, self->maxs, end, self, MASK_SHOT);

    if (tr.fraction < 1.0f) {
        /* Impact — explode */
        R_ParticleEffect(tr.endpos, tr.plane.normal, 2, 32);   /* fire burst */
        R_ParticleEffect(tr.endpos, tr.plane.normal, 11, 12);  /* debris chunks */
        R_ParticleEffect(tr.endpos, tr.plane.normal, 10, 6);   /* smoke cloud */
        R_ParticleEffect(tr.endpos, tr.plane.normal, 13, 8);   /* ground dust */
        R_AddDlight(tr.endpos, 1.0f, 0.6f, 0.1f, 400.0f, 0.5f);
        R_AddSprite(tr.endpos, 48.0f, 1.0f, 0.6f, 0.1f, 0.9f, 0.6f, 120.0f);  /* fireball */
        R_AddSprite(tr.endpos, 64.0f, 0.3f, 0.3f, 0.3f, 0.5f, 1.2f, 30.0f);   /* smoke */
        if (snd_explode)
            gi.positioned_sound(tr.endpos, NULL, CHAN_AUTO, snd_explode, 1.0f, ATTN_NORM, 0);

        VectorCopy(tr.endpos, self->s.origin);
        T_RadiusDamage(self, self->owner, (float)self->dmg, (float)self->dmg_radius);

        /* Distance-based screen shake */
        G_ExplosionShakeNearby(tr.endpos, 0.8f, 0.4f, 512.0f);

        /* Direct hit bonus */
        if (tr.ent && tr.ent->takedamage && tr.ent->health > 0) {
            tr.ent->health -= self->dmg;
            if (tr.ent->health <= 0 && tr.ent->die)
                tr.ent->die(tr.ent, self, self->owner, self->dmg, tr.endpos);
        }

        self->inuse = qfalse;
        gi.unlinkentity(self);
        return;
    }

    VectorCopy(end, self->s.origin);

    /* Rocket trail: smoke + fire sparks + engine glow */
    {
        vec3_t trail_dir;
        VectorCopy(self->velocity, trail_dir);
        VectorNormalize(trail_dir);
        VectorNegate(trail_dir, trail_dir);
        R_ParticleEffect(self->s.origin, trail_dir, 3, 3);    /* fire sparks */
        R_ParticleEffect(self->s.origin, trail_dir, 10, 2);   /* lingering smoke puffs */
        R_ParticleEffect(self->s.origin, trail_dir, 4, 1);    /* small flame */
        R_AddDlight(self->s.origin, 1.0f, 0.5f, 0.1f, 120.0f, 0.15f);
    }

    /* Timeout after 5 seconds */
    if (level.time > self->teleport_time) {
        self->inuse = qfalse;
        gi.unlinkentity(self);
        return;
    }

    self->nextthink = level.time + level.frametime;
    gi.linkentity(self);
}

/*
 * Grenade think — bounces, then detonates after timer
 */
void grenade_explode(edict_t *self)
{
    vec3_t up = {0, 0, 1};

    R_ParticleEffect(self->s.origin, up, 2, 24);     /* fire burst */
    R_ParticleEffect(self->s.origin, up, 11, 16);    /* debris chunks */
    R_ParticleEffect(self->s.origin, up, 10, 8);     /* smoke cloud */
    R_ParticleEffect(self->s.origin, up, 13, 6);     /* ground dust */
    R_AddDlight(self->s.origin, 1.0f, 0.5f, 0.1f, 350.0f, 0.4f);
    R_AddSprite(self->s.origin, 40.0f, 1.0f, 0.5f, 0.1f, 0.8f, 0.5f, 100.0f);  /* fireball */
    R_AddSprite(self->s.origin, 56.0f, 0.3f, 0.3f, 0.3f, 0.4f, 1.0f, 25.0f);   /* smoke */
    if (snd_explode)
        gi.positioned_sound(self->s.origin, NULL, CHAN_AUTO, snd_explode, 1.0f, ATTN_NORM, 0);

    T_RadiusDamage(self, self->owner, (float)self->dmg, (float)self->dmg_radius);
    G_ExplosionShakeNearby(self->s.origin, 0.6f, 0.3f, 400.0f);

    self->inuse = qfalse;
    gi.unlinkentity(self);
}

/*
 * G_FireProjectile — Spawn a rocket or grenade projectile
 */
static void G_FireProjectile(edict_t *ent, qboolean is_grenade)
{
    edict_t *proj;
    vec3_t forward, right, up;
    vec3_t start;

    proj = G_AllocEdict();
    if (!proj) return;

    G_AngleVectors(ent->client->viewangles, forward, right, up);

    VectorCopy(ent->s.origin, start);
    start[2] += ent->client->viewheight;
    VectorMA(start, 16, forward, start);

    proj->classname = is_grenade ? "grenade" : "rocket";
    proj->owner = ent;
    proj->entity_type = ET_MISSILE;
    proj->clipmask = MASK_SHOT;
    proj->solid = SOLID_BBOX;

    VectorSet(proj->mins, -2, -2, -2);
    VectorSet(proj->maxs, 2, 2, 2);
    VectorCopy(start, proj->s.origin);
    VectorCopy(ent->client->viewangles, proj->s.angles);

    if (is_grenade) {
        float cook_time = 0;
        float fuse_remaining = 2.5f;

        /* Grenade cooking: if player was holding fire, reduce fuse */
        if (ent->client->grenade_cook_start > 0) {
            cook_time = level.time - ent->client->grenade_cook_start;
            if (cook_time > 2.3f) cook_time = 2.3f;  /* can't cook past fuse */
            fuse_remaining = 2.5f - cook_time;
            if (fuse_remaining < 0.2f) fuse_remaining = 0.2f;
            ent->client->grenade_cook_start = 0;
        }

        proj->movetype = MOVETYPE_BOUNCE;
        VectorScale(forward, 600, proj->velocity);
        proj->velocity[2] += 200;  /* arc upward */
        proj->dmg = weapon_damage[WEAP_GRENADE];
        proj->dmg_radius = 200;
        proj->think = grenade_explode;
        proj->nextthink = level.time + fuse_remaining;
        proj->teleport_time = level.time + 10.0f;
    } else {
        proj->movetype = MOVETYPE_FLYMISSILE;
        VectorScale(forward, 1000, proj->velocity);
        proj->dmg = weapon_damage[WEAP_ROCKET];
        proj->dmg_radius = 150;
        proj->think = rocket_think;
        proj->nextthink = level.time + level.frametime;
        proj->teleport_time = level.time + 5.0f;
    }

    /* Muzzle flash */
    {
        vec3_t muzzle;
        VectorMA(start, 16, forward, muzzle);
        R_ParticleEffect(muzzle, forward, 3, 4);
        R_AddDlight(muzzle, 1.0f, 0.8f, 0.3f, 250.0f, 0.15f);
    }

    gi.linkentity(proj);
}

/* ==========================================================================
   Gore Zone Damage System
   Maps hit location to GHOUL body zone based on Z-offset from target origin
   and horizontal offset for left/right discrimination.
   ========================================================================== */

/* Zone damage multipliers — headshots deal 3x, limbs deal less */
static float gore_zone_dmg_mult[GORE_NUM_ZONES] = {
    3.0f,   /* HEAD */
    2.5f,   /* NECK */
    1.0f,   /* CHEST_UPPER */
    1.0f,   /* CHEST_LOWER */
    0.9f,   /* STOMACH */
    1.2f,   /* GROIN */
    0.7f,   /* ARM_UPPER_R */
    0.6f,   /* ARM_LOWER_R */
    0.5f,   /* HAND_R */
    0.7f,   /* ARM_UPPER_L */
    0.6f,   /* ARM_LOWER_L */
    0.5f,   /* HAND_L */
    0.7f,   /* LEG_UPPER_R */
    0.6f,   /* LEG_LOWER_R */
    0.5f,   /* FOOT_R */
    0.7f,   /* LEG_UPPER_L */
    0.6f,   /* LEG_LOWER_L */
    0.5f,   /* FOOT_L */
    1.0f,   /* BACK_UPPER */
    0.9f,   /* BACK_LOWER */
    0.8f,   /* BUTT */
    0.8f,   /* SHOULDER_R */
    0.8f,   /* SHOULDER_L */
    0.7f,   /* HIP_R */
    0.7f,   /* HIP_L */
    3.0f,   /* FACE */
};

/* Zone sever thresholds — cumulative damage before limb severs */
static int gore_zone_sever_threshold[GORE_NUM_ZONES] = {
    150,    /* HEAD */
    120,    /* NECK */
    0,      /* CHEST_UPPER (can't sever) */
    0,      /* CHEST_LOWER */
    0,      /* STOMACH */
    0,      /* GROIN */
    80,     /* ARM_UPPER_R */
    60,     /* ARM_LOWER_R */
    40,     /* HAND_R */
    80,     /* ARM_UPPER_L */
    60,     /* ARM_LOWER_L */
    40,     /* HAND_L */
    100,    /* LEG_UPPER_R */
    70,     /* LEG_LOWER_R */
    50,     /* FOOT_R */
    100,    /* LEG_UPPER_L */
    70,     /* LEG_LOWER_L */
    50,     /* FOOT_L */
    0,      /* BACK_UPPER */
    0,      /* BACK_LOWER */
    0,      /* BUTT */
    80,     /* SHOULDER_R */
    80,     /* SHOULDER_L */
    90,     /* HIP_R */
    90,     /* HIP_L */
    150,    /* FACE */
};

/* Per-entity zone damage accumulation (indexed by edict index) */
#define MAX_GORE_EDICTS 256
static int gore_zone_damage[MAX_GORE_EDICTS][GORE_NUM_ZONES];

/* ==========================================================================
   Bullet Decal System — persistent impact marks on world surfaces
   Ring buffer of up to 256 decals, each with position, normal, and spawn time.
   Decals expire after 30 seconds.
   ========================================================================== */

#define MAX_DECALS  256
#define DECAL_LIFETIME  30.0f

typedef struct {
    vec3_t  origin;
    vec3_t  normal;
    float   spawn_time;
    int     type;       /* 0=bullet, 1=blood, 2=scorch */
    qboolean active;
} decal_t;

static decal_t  g_decals[MAX_DECALS];
static int      g_decal_index;  /* next write position */

static void G_AddDecal(vec3_t origin, vec3_t normal, int type)
{
    decal_t *d = &g_decals[g_decal_index];

    VectorCopy(origin, d->origin);
    VectorCopy(normal, d->normal);
    d->spawn_time = level.time;
    d->type = type;
    d->active = qtrue;

    g_decal_index = (g_decal_index + 1) % MAX_DECALS;

    /* Also add to the renderer's decal system for projected quad rendering */
    R_AddDecal(origin, normal, type);
}

/* Called from RunFrame to expire old decals and render persistent ones */
static void G_UpdateDecals(void)
{
    int i;
    for (i = 0; i < MAX_DECALS; i++) {
        decal_t *d = &g_decals[i];
        if (!d->active) continue;

        if (level.time - d->spawn_time > DECAL_LIFETIME) {
            d->active = qfalse;
            continue;
        }

        /* Spawn a faint persistent particle to represent the decal */
        if (((int)(level.time * 2.0f) % 4) == (i % 4)) {
            /* Stagger rendering to avoid spawning all 256 every frame */
            R_ParticleEffect(d->origin, d->normal, d->type, 1);
        }
    }
}

/* ==========================================================================
   Gib Physics — Physical chunks spawned on overkill deaths
   ========================================================================== */

static void gib_think(edict_t *self)
{
    /* Expire after 5 seconds */
    if (level.time > self->dmg_debounce_time) {
        self->inuse = qfalse;
        gi.unlinkentity(self);
        return;
    }
    self->nextthink = level.time + 0.1f;
}

static void gib_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)other; (void)surf;

    /* Splat effect on first bounce */
    if (self->dmg == 0) {
        vec3_t up = {0, 0, 1};
        if (plane) {
            trace_t *tr_plane = (trace_t *)plane;
            VectorCopy(tr_plane->plane.normal, up);
        }
        R_ParticleEffect(self->s.origin, up, 1, 4);  /* blood splash */
        self->dmg = 1;  /* flag: already bounced */
    }

    /* Reduce velocity on each bounce */
    self->velocity[0] *= 0.6f;
    self->velocity[1] *= 0.6f;
    self->velocity[2] *= -0.4f;
}

static void G_SpawnGibs(edict_t *ent, int count)
{
    int i;
    int snd_gib;

    snd_gib = gi.soundindex("player/udeath.wav");

    if (snd_gib)
        gi.sound(ent, CHAN_BODY, snd_gib, 1.0f, ATTN_NORM, 0);

    for (i = 0; i < count; i++) {
        edict_t *gib = G_AllocEdict();
        if (!gib) break;

        gib->classname = "gib";
        gib->movetype = MOVETYPE_BOUNCE;
        gib->solid = SOLID_NOT;
        gib->touch = gib_touch;
        gib->think = gib_think;
        gib->nextthink = level.time + 0.1f;
        gib->dmg_debounce_time = level.time + 5.0f;  /* 5s lifetime */
        gib->dmg = 0;  /* bounce flag: not yet bounced */

        VectorCopy(ent->s.origin, gib->s.origin);
        gib->s.origin[2] += 16;  /* spawn from torso height */

        /* Random outward velocity */
        gib->velocity[0] = gi.flrand(-300.0f, 300.0f);
        gib->velocity[1] = gi.flrand(-300.0f, 300.0f);
        gib->velocity[2] = gi.flrand(200.0f, 500.0f);

        VectorSet(gib->mins, -2, -2, -2);
        VectorSet(gib->maxs, 2, 2, 2);

        gi.linkentity(gib);
    }

    /* Big blood burst at origin */
    {
        vec3_t up = {0, 0, 1};
        R_ParticleEffect(ent->s.origin, up, 1, 64);
        R_AddDlight(ent->s.origin, 1.0f, 0.0f, 0.0f, 200.0f, 0.3f);
    }
}

/*
 * G_HitToGoreZone — Determine gore zone from hit point relative to target
 *
 * Uses Z-offset from entity origin (standing model with feet at origin):
 *   0-8:   feet
 *   8-24:  lower legs
 *   24-36: upper legs / hips
 *   36-40: groin
 *   40-48: stomach
 *   48-52: lower chest
 *   52-60: upper chest / shoulders
 *   60-64: neck
 *   64+:   head / face
 *
 * Horizontal offset determines left vs right for limbs.
 */
static gore_zone_id_t G_HitToGoreZone(edict_t *target, vec3_t hitpoint,
                                       vec3_t hit_dir)
{
    float z_off, x_off;
    qboolean right_side;

    z_off = hitpoint[2] - target->s.origin[2];
    x_off = hitpoint[0] - target->s.origin[0];

    /* Use forward direction cross with hit offset for left/right */
    {
        float yaw = target->s.angles[1] * (3.14159265f / 180.0f);
        float fwd_x = (float)cos(yaw);
        float fwd_y = (float)sin(yaw);
        float dx = hitpoint[0] - target->s.origin[0];
        float dy = hitpoint[1] - target->s.origin[1];
        /* Cross product Z = right-hand side positive */
        float cross = fwd_x * dy - fwd_y * dx;
        right_side = (cross >= 0);
    }

    /* Head region */
    if (z_off >= 64) {
        /* Determine face vs back of head using dot product with facing */
        float yaw = target->s.angles[1] * (3.14159265f / 180.0f);
        float dot = hit_dir[0] * (float)cos(yaw) + hit_dir[1] * (float)sin(yaw);
        if (dot > 0)
            return GORE_ZONE_FACE;  /* shot from front */
        return GORE_ZONE_HEAD;
    }

    /* Neck */
    if (z_off >= 60) return GORE_ZONE_NECK;

    /* Upper chest / shoulders */
    if (z_off >= 52) {
        float abs_lateral = (float)fabs(hitpoint[0] - target->s.origin[0]) +
                           (float)fabs(hitpoint[1] - target->s.origin[1]);
        if (abs_lateral > 12) {
            return right_side ? GORE_ZONE_SHOULDER_R : GORE_ZONE_SHOULDER_L;
        }
        return GORE_ZONE_CHEST_UPPER;
    }

    /* Lower chest */
    if (z_off >= 48) return GORE_ZONE_CHEST_LOWER;

    /* Stomach — check for arms at this height */
    if (z_off >= 40) {
        float abs_lateral = (float)fabs(hitpoint[0] - target->s.origin[0]) +
                           (float)fabs(hitpoint[1] - target->s.origin[1]);
        if (abs_lateral > 14) {
            /* Arms hang at stomach level */
            if (right_side) {
                return (z_off >= 44) ? GORE_ZONE_ARM_UPPER_R : GORE_ZONE_ARM_LOWER_R;
            } else {
                return (z_off >= 44) ? GORE_ZONE_ARM_UPPER_L : GORE_ZONE_ARM_LOWER_L;
            }
        }
        return GORE_ZONE_STOMACH;
    }

    /* Groin */
    if (z_off >= 36) return GORE_ZONE_GROIN;

    /* Upper legs / hips */
    if (z_off >= 24) {
        if (right_side)
            return (z_off >= 32) ? GORE_ZONE_HIP_R : GORE_ZONE_LEG_UPPER_R;
        else
            return (z_off >= 32) ? GORE_ZONE_HIP_L : GORE_ZONE_LEG_UPPER_L;
    }

    /* Lower legs */
    if (z_off >= 8) {
        return right_side ? GORE_ZONE_LEG_LOWER_R : GORE_ZONE_LEG_LOWER_L;
    }

    /* Feet */
    return right_side ? GORE_ZONE_FOOT_R : GORE_ZONE_FOOT_L;
}

/*
 * G_ApplyGoreZoneDamage — Apply damage to a specific zone, track accumulation,
 * trigger severing when threshold exceeded.
 */
static void G_ApplyGoreZoneDamage(edict_t *target, edict_t *attacker,
                                   gore_zone_id_t zone, int damage)
{
    int eidx = target->s.number;

    if (eidx < 0 || eidx >= MAX_GORE_EDICTS)
        return;

    /* Mark zone as damaged in entity bitmask */
    target->gore_zone_mask |= (1 << zone);
    if (target->client)
        target->client->last_damage_zone = zone;

    /* Accumulate zone damage */
    gore_zone_damage[eidx][zone] += damage;

    /* Call GHOUL damage_zone for visual wound effect */
    if (gi.ghoul_damage_zone)
        gi.ghoul_damage_zone(target, zone, damage);

    /* Check for severance */
    if (gore_zone_sever_threshold[zone] > 0 &&
        gore_zone_damage[eidx][zone] >= gore_zone_sever_threshold[zone] &&
        !(target->severed_zone_mask & (1 << zone)))
    {
        target->severed_zone_mask |= (1 << zone);

        /* Call GHOUL sever for visual dismemberment */
        if (gi.ghoul_sever_zone)
            gi.ghoul_sever_zone(target, zone);

        /* Bonus gore particles on severing */
        R_ParticleEffect(target->s.origin, target->s.angles, 1, 32);

        if (attacker && attacker->client)
            attacker->client->gore_kills++;

        gi.dprintf("Zone %d severed on entity %d!\n", zone, eidx);
    }
}

/*
 * G_FireHitscan — Fire a hitscan trace from the player's eye
 */
static void G_FireHitscan(edict_t *ent)
{
    vec3_t start, end, forward, right, up;
    trace_t tr;
    int weap = ent->client->pers_weapon;
    int damage;

    /* Fire rate limiter */
    if (level.time < player_next_fire)
        return;

    /* Weapon overheat check — can't fire while overheated */
    if (ent->client->weapon_overheated) {
        if (level.time >= ent->client->overheat_end) {
            ent->client->weapon_overheated = qfalse;
            ent->client->weapon_heat = 0;
        } else {
            return;
        }
    }

    /* Weapon jam check — can't fire while jammed */
    if (ent->client->weapon_jammed) {
        if (level.time >= ent->client->jam_clear_time && ent->client->jam_clear_time > 0) {
            ent->client->weapon_jammed = qfalse;
            ent->client->jam_clear_time = 0;
        } else {
            return;
        }
    }

    /* Weapon switch delay — can't fire while switching */
    if (ent->client->weapon_change_time > level.time)
        return;

    /* Dual wield: alternate between pistol 1 and pistol 2 */
    if (ent->client->dual_wield &&
        (weap == WEAP_PISTOL1 || weap == WEAP_PISTOL2)) {
        /* Alternate which pistol fires */
        if (ent->client->dual_fire_left) {
            weap = WEAP_PISTOL2;
        } else {
            weap = WEAP_PISTOL1;
        }
        ent->client->dual_fire_left = !ent->client->dual_fire_left;
    }

    /* Handle utility weapons (medkit, etc.) */
    if (G_UseUtilityWeapon(ent)) {
        player_next_fire = level.time + ((weap > 0 && weap < WEAP_COUNT) ? weapon_firerate[weap] : 0.5f);
        player_last_fire_time = level.time;
        return;
    }

    /* Check ammo / magazine (knife/melee weapons don't use ammo) */
    if (weap > 0 && weap < WEAP_COUNT && weap != WEAP_KNIFE) {
        int mag_size = weapon_magazine_size[weap];

        if (mag_size > 0) {
            /* Magazine weapon: consume from magazine */
            if (ent->client->magazine[weap] <= 0) {
                /* Auto-reload when magazine empty */
                if (ent->client->ammo[weap] > 0 && !ent->client->reloading_weapon) {
                    ent->client->reloading_weapon = weap;
                    ent->client->reload_finish_time = level.time + weapon_reload_time[weap];
                    if (snd_reload)
                        gi.sound(ent, CHAN_WEAPON, snd_reload, 1.0f, ATTN_NORM, 0);
                } else if (snd_noammo) {
                    gi.sound(ent, CHAN_WEAPON, snd_noammo, 0.5f, ATTN_NORM, 0);
                }
                return;
            }
            ent->client->magazine[weap]--;
        } else {
            /* Non-magazine weapon: consume from ammo directly */
            if (ent->client->ammo[weap] <= 0) {
                if (snd_noammo)
                    gi.sound(ent, CHAN_WEAPON, snd_noammo, 0.5f, ATTN_NORM, 0);
                return;
            }
            ent->client->ammo[weap]--;
        }
    }

    /* Weapon jam: small chance on sustained fire (high recoil = worn mechanism) */
    if (ent->client->recoil_accum > 0.6f && weap != WEAP_KNIFE) {
        float jam_chance = (ent->client->recoil_accum - 0.6f) * 0.05f;  /* up to 2% */
        if (gi.flrand(0, 1.0f) < jam_chance) {
            /* Weapon jammed! Brief delay before can fire again */
            player_next_fire = level.time + 1.0f;  /* 1s jam clear time */
            if (snd_noammo)
                gi.sound(ent, CHAN_WEAPON, snd_noammo, 0.7f, ATTN_NORM, 0);
            SCR_AddPickupMessage("WEAPON JAM!");
            return;
        }
    }

    damage = (weap > 0 && weap < WEAP_COUNT) ? weapon_damage[weap] : 15;

    /* Adrenaline rush damage boost */
    if (ent->client->adrenaline_end > level.time && ent->client->adrenaline_mult > 1.0f)
        damage = (int)(damage * ent->client->adrenaline_mult);

    {
        float firerate = (weap > 0 && weap < WEAP_COUNT) ? weapon_firerate[weap] : 0.2f;
        /* Dual wield: faster fire rate but less accurate */
        if (ent->client->dual_wield &&
            (weap == WEAP_PISTOL1 || weap == WEAP_PISTOL2))
            firerate *= 0.6f;  /* 40% faster fire rate */
        player_next_fire = level.time + firerate;
    }
    player_last_fire_time = level.time;

    /* Track shots fired for accuracy stats */
    if (ent->client)
        ent->client->shots_fired++;

    /* Fire from eye position */
    VectorCopy(ent->s.origin, start);
    start[2] += ent->client->viewheight;

    /* Direction from view angles */
    G_AngleVectors(ent->client->viewangles, forward, right, up);

    /* Muzzle flash particles and light */
    {
        vec3_t muzzle;
        VectorMA(start, 16, forward, muzzle);
        VectorMA(muzzle, 6, right, muzzle);
        R_ParticleEffect(muzzle, forward, 3, 3);
        R_AddDlight(muzzle, 1.0f, 0.8f, 0.4f, 200.0f, 0.1f);

        /* Shell casing ejection for ballistic weapons */
        if (weap == WEAP_PISTOL1 || weap == WEAP_PISTOL2 ||
            weap == WEAP_SHOTGUN || weap == WEAP_MACHINEGUN ||
            weap == WEAP_ASSAULT || weap == WEAP_SNIPER ||
            weap == WEAP_MPISTOL) {
            vec3_t eject_pos, eject_dir;
            VectorMA(muzzle, -8, forward, eject_pos);  /* behind muzzle */
            VectorMA(eject_pos, 8, right, eject_pos);  /* to the right */
            VectorCopy(right, eject_dir);
            R_ParticleEffect(eject_pos, eject_dir, 5, 1);
        }
    }

    /* Weapon fire sound — silencer reduces volume */
    if (weap > 0 && weap < WEAP_COUNT && snd_weapons[weap]) {
        float vol = 1.0f;
        float attn = ATTN_NORM;
        qboolean silenced = (ent->client->attachments[weap] & ATTACH_SILENCER) ? qtrue : qfalse;
        if (silenced) {
            vol = 0.3f;     /* much quieter */
            attn = ATTN_IDLE;  /* shorter range */
        }
        gi.sound(ent, CHAN_WEAPON, snd_weapons[weap], vol, attn, 0);

        /* Alert nearby idle monsters — hearing system */
        {
            extern void AI_HearGunshot(vec3_t origin, edict_t *shooter, qboolean silenced);
            AI_HearGunshot(ent->s.origin, ent, silenced);
        }
    }

    /* Weapon heat buildup — automatic weapons build heat faster */
    {
        float heat_per_shot = 0.02f;  /* base */
        if (weap == WEAP_MACHINEGUN || weap == WEAP_MPISTOL)
            heat_per_shot = 0.04f;
        else if (weap == WEAP_ASSAULT)
            heat_per_shot = 0.035f;
        else if (weap == WEAP_FLAMEGUN)
            heat_per_shot = 0.05f;

        ent->client->weapon_heat += heat_per_shot;
        if (ent->client->weapon_heat >= 1.0f) {
            ent->client->weapon_heat = 1.0f;
            ent->client->weapon_overheated = qtrue;
            ent->client->overheat_end = level.time + 2.0f;  /* 2s cooldown */
            SCR_AddPickupMessage("OVERHEATED!");
        }

        /* Weapon jam: small chance on high-heat sustained fire */
        if (ent->client->weapon_heat > 0.7f && !ent->client->weapon_jammed) {
            if (gi.irand(0, 100) < 3) {  /* 3% chance per shot when hot */
                ent->client->weapon_jammed = qtrue;
                gi.cprintf(ent, PRINT_ALL, "Weapon jammed! Use 'unjam' to clear.\n");
                SCR_AddPickupMessage("JAMMED!");
                {
                    int snd = gi.soundindex("weapons/noammo.wav");
                    if (snd)
                        gi.sound(ent, CHAN_WEAPON, snd, 1.0f, ATTN_NORM, 0);
                }
                return;  /* shot fails */
            }
        }
    }

    /* Weapon-specific trace parameters */
    {
        int num_pellets = 1;
        float trace_range = 8192;
        float spread = 0;
        int pellet;
        int att = (weap > 0 && weap < WEAP_COUNT) ? ent->client->attachments[weap] : 0;

        /* Apply base weapon spread */
        if (weap > 0 && weap < WEAP_COUNT)
            spread = weapon_spread[weap];

        /* Laser sight: 30% tighter spread */
        if (att & ATTACH_LASER)
            spread *= 0.7f;

        /* Recoil accumulation: sustained fire increases spread */
        {
            float recoil_add = 0.08f;  /* base recoil per shot */
            if (weap == WEAP_MACHINEGUN || weap == WEAP_ASSAULT)
                recoil_add = 0.12f;    /* full-auto weapons build faster */
            else if (weap == WEAP_SNIPER)
                recoil_add = 0.25f;    /* bolt-action: big kick per shot */
            else if (weap == WEAP_SHOTGUN)
                recoil_add = 0.2f;

            ent->client->recoil_accum += recoil_add;
            if (ent->client->recoil_accum > 1.0f)
                ent->client->recoil_accum = 1.0f;

            spread += ent->client->recoil_accum * 0.04f;
        }

        /* Zoomed sniper: drastically reduce spread for precision */
        if (ent->client->zoomed && weap == WEAP_SNIPER)
            spread *= 0.1f;

        /* Dual wielding: reduced accuracy (one-handed shooting) */
        if (ent->client->dual_wield &&
            (weap == WEAP_PISTOL1 || weap == WEAP_PISTOL2))
            spread *= 1.5f;

        /* Crouching reduces spread (steadier aim) */
        if (ent->client->viewheight < 20.0f)
            spread *= 0.6f;

        /* Moving increases spread (check velocity magnitude) */
        {
            float speed_sq = ent->velocity[0] * ent->velocity[0] +
                             ent->velocity[1] * ent->velocity[1];
            if (speed_sq > 100.0f * 100.0f)
                spread *= 1.3f;
        }

        /* Weapon-specific parameters */
        if (weap == WEAP_KNIFE) {
            trace_range = 96;  /* melee range */
        } else if (weap == WEAP_SHOTGUN) {
            if (player_alt_fire) {
                /* Alt: single accurate slug */
                num_pellets = 1;
                damage = 70;
                spread = 0.005f;
            } else {
                /* Primary: 8 pellet spread */
                num_pellets = 8;
                damage = 10;
                spread = 0.08f;
            }
        } else if (player_alt_fire) {
            /* Alt-fire modes for other weapons */
            if (weap == WEAP_PISTOL1) {
                /* Desert Eagle alt: rapid double-tap, less accurate */
                spread += 0.03f;
                player_next_fire = level.time + 0.1f;
            } else if (weap == WEAP_ASSAULT) {
                /* M4 alt: 3-round burst */
                num_pellets = 3;
                damage = 18;
                spread = 0.02f;
            }
        }

        for (pellet = 0; pellet < num_pellets; pellet++) {
            vec3_t pellet_dir;

            VectorCopy(forward, pellet_dir);
            if (spread > 0) {
                pellet_dir[0] += gi.flrand(-spread, spread);
                pellet_dir[1] += gi.flrand(-spread, spread);
                pellet_dir[2] += gi.flrand(-spread, spread);
            }

            VectorMA(start, trace_range, pellet_dir, end);
            tr = gi.trace(start, NULL, NULL, end, ent, MASK_SHOT);

            /* Bullet tracer from muzzle to impact */
            if (tr.fraction < 1.0f && weap != WEAP_KNIFE) {
                vec3_t tracer_start;
                VectorMA(start, 16, forward, tracer_start);
                VectorMA(tracer_start, 6, right, tracer_start);
                R_AddTracer(tracer_start, tr.endpos, 1.0f, 0.9f, 0.5f);
            }

    if (tr.fraction < 1.0f) {
        /* Spawn impact particles at hit point */
        if (tr.ent && tr.ent->takedamage && tr.ent->health > 0) {
            /* Spawn invulnerability check */
            if (tr.ent->client && tr.ent->client->invuln_time > level.time) {
                R_ParticleEffect(tr.endpos, tr.plane.normal, 3, 4);  /* shield spark */
                goto hitscan_impact_done;
            }

            /* Team friendly fire protection */
            if (ent->client && tr.ent->client &&
                ent->client->team > 0 && ent->client->team == tr.ent->client->team) {
                goto hitscan_impact_done;  /* no team damage */
            }

            /* Track hit for accuracy stats */
            if (ent->client)
                ent->client->shots_hit++;

            /* Blood effect + flesh hit sound */
            R_ParticleEffect(tr.endpos, tr.plane.normal, 1, 8);
            if (snd_hit_flesh)
                gi.sound(tr.ent, CHAN_BODY, snd_hit_flesh, 1.0f, ATTN_NORM, 0);

            /* Melee backstab bonus — 3x damage when knifing from behind */
            if (weap == WEAP_KNIFE && tr.ent->s.angles[1] != 0) {
                vec3_t victim_fwd, to_victim;
                float dot;
                victim_fwd[0] = cosf(tr.ent->s.angles[1] * 3.14159f / 180.0f);
                victim_fwd[1] = sinf(tr.ent->s.angles[1] * 3.14159f / 180.0f);
                victim_fwd[2] = 0;
                VectorSubtract(tr.ent->s.origin, ent->s.origin, to_victim);
                to_victim[2] = 0;
                VectorNormalize(to_victim);
                dot = DotProduct(victim_fwd, to_victim);
                if (dot > 0.5f) {
                    /* Attacking from behind — backstab! */
                    damage *= 3;
                    SCR_AddPickupMessage("BACKSTAB!");
                    ent->client->score += 15;
                    SCR_AddScorePopup(15);
                }
            }

            /* Knife bleed: melee hits cause bleeding DoT */
            if (weap == WEAP_KNIFE && tr.ent->client && tr.ent->health > 0) {
                tr.ent->client->bleed_end = level.time + 4.0f;
                tr.ent->client->bleed_next_tick = level.time + 1.0f;
            }

            /* Execution finisher — instant kill on low-HP enemy with melee */
            if (weap == WEAP_KNIFE && tr.ent->health > 0 &&
                tr.ent->max_health > 0 &&
                (float)tr.ent->health / (float)tr.ent->max_health < 0.25f) {
                damage = tr.ent->health + 50;  /* overkill for gib */
                SCR_AddPickupMessage("EXECUTION!");
                if (ent->client) {
                    ent->client->score += 25;
                    ent->client->xp += 15;  /* bonus XP */
                    SCR_AddScorePopup(25);
                }
            }

            /* Distance-based damage falloff (except sniper/slugger) */
            if (weap != WEAP_SNIPER && weap != WEAP_SLUGGER && weap != WEAP_KNIFE) {
                float hit_dist = tr.fraction * trace_range;
                if (hit_dist > 1024.0f) {
                    /* Linear falloff: 100% at 1024, 70% at max range */
                    float falloff = 1.0f - 0.3f * ((hit_dist - 1024.0f) /
                                    (trace_range - 1024.0f));
                    if (falloff < 0.5f) falloff = 0.5f;
                    damage = (int)(damage * falloff);
                }
            }

            /* Gore zone detection — map hit location to body zone */
            {
                vec3_t hit_dir;
                gore_zone_id_t zone;
                float zone_mult;
                int zone_dmg;

                VectorCopy(forward, hit_dir);
                zone = G_HitToGoreZone(tr.ent, tr.endpos, hit_dir);
                zone_mult = gore_zone_dmg_mult[zone];
                zone_dmg = (int)(damage * zone_mult);
                if (ent->client) ent->client->last_damage_zone = zone;

                /* Shield power-up damage reduction */
                if (tr.ent->client && tr.ent->client->shield_end > level.time)
                    zone_dmg = (int)(zone_dmg * tr.ent->client->shield_mult);

                /* Armor absorbs 66% of damage — unless AP rounds bypass it */
                if (tr.ent->client && tr.ent->client->armor > 0 &&
                    !(ent->client && ent->client->ap_rounds)) {
                    int armor_absorb = (int)(zone_dmg * 0.66f);
                    if (armor_absorb > tr.ent->client->armor)
                        armor_absorb = tr.ent->client->armor;
                    tr.ent->client->armor -= armor_absorb;
                    zone_dmg -= armor_absorb;
                }

                /* Apply gore zone damage tracking and GHOUL effects */
                G_ApplyGoreZoneDamage(tr.ent, ent, zone, zone_dmg);

                tr.ent->health -= zone_dmg;
                damage = zone_dmg;  /* update for death check */

                /* Floating damage number at crosshair area */
                SCR_AddDamageNumber(zone_dmg, 0, 0);  /* 0,0 = use screen center */
                SCR_TriggerHitMarker();

                /* Headshot notification */
                if (zone == GORE_ZONE_HEAD || zone == GORE_ZONE_FACE) {
                    SCR_AddPickupMessage("HEADSHOT!");
                    if (ent->client) {
                        ent->client->score += 5;  /* bonus for precision */
                        ent->client->headshots++;
                    }
                }

                gi.dprintf("Hit %s zone %d (x%.1f) for %d damage (health: %d)\n",
                           tr.ent->classname ? tr.ent->classname : "entity",
                           zone, zone_mult, zone_dmg, tr.ent->health);
            }

            /* Player pain sound + damage flash + blood splatter */
            if (tr.ent->client) {
                tr.ent->client->pers_health = tr.ent->health;
                tr.ent->client->blend[0] = 1.0f;
                tr.ent->client->blend[1] = 0.0f;
                tr.ent->client->blend[2] = 0.0f;
                tr.ent->client->blend[3] = 0.3f;

                SCR_AddBloodSplatter(damage);

                if (level.time >= tr.ent->client->next_pain_sound) {
                    int psnd = gi.irand(0, 1) ? snd_player_pain1 : snd_player_pain2;
                    if (psnd)
                        gi.sound(tr.ent, CHAN_VOICE, psnd, 1.0f, ATTN_NORM, 0);
                    tr.ent->client->next_pain_sound = level.time + 0.5f;
                }
            }

            if (tr.ent->health <= 0 && tr.ent->die) {
                /* Explosion particles, light, and sound on kill */
                R_ParticleEffect(tr.endpos, tr.plane.normal, 2, 16);
                R_AddDlight(tr.endpos, 1.0f, 0.5f, 0.1f, 300.0f, 0.3f);

                /* Player death sound */
                if (tr.ent->client && snd_player_die)
                    gi.sound(tr.ent, CHAN_VOICE, snd_player_die, 1.0f, ATTN_NORM, 0);
                else if (snd_explode)
                    gi.sound(tr.ent, CHAN_AUTO, snd_explode, 1.0f, ATTN_NORM, 0);

                /* Gib effect — overkill damage causes physical gibs */
                if (tr.ent->health < -40) {
                    int gib_count = 4 + gi.irand(0, 4);  /* 4-8 gibs */
                    G_SpawnGibs(tr.ent, gib_count);
                }

                tr.ent->die(tr.ent, ent, ent, damage, tr.endpos);

                /* Award kill to attacker */
                if (ent->client) {
                    static const int rank_xp_thresholds[] = {
                        0, 100, 300, 600, 1000, 1500, 2200, 3000, 4000, 5500
                    };
                    static const char *rank_names[] = {
                        "Recruit", "Private", "Corporal", "Sergeant",
                        "Lieutenant", "Captain", "Major", "Colonel",
                        "Commander", "General"
                    };
                    int xp_award = 25;
                    int score_award = 10;
                    int old_rank;

                    /* Score multiplier based on active streak */
                    if (ent->client->streak_count >= 4) {
                        score_award *= 3;  /* 3x for 4+ streak */
                        xp_award *= 2;
                    } else if (ent->client->streak_count >= 2) {
                        score_award *= 2;  /* 2x for 2-3 streak */
                        xp_award = (int)(xp_award * 1.5f);
                    }

                    ent->client->kills++;
                    ent->client->score += score_award;
                    SCR_AddScorePopup(score_award);

                    /* Challenge mode tracking */
                    if (ent->client->challenge_end > level.time) {
                        ent->client->challenge_kills++;
                        if (ent->client->challenge_kills >= ent->client->challenge_target) {
                            SCR_AddPickupMessage("CHALLENGE COMPLETE! +200 XP!");
                            ent->client->xp += 200;
                            ent->client->score += 100;
                            SCR_AddScorePopup(100);
                            ent->client->challenge_end = 0;
                        }
                    }

                    /* XP and rank progression */
                    old_rank = ent->client->rank;
                    ent->client->xp += xp_award;
                    if (ent->client->rank < 9 &&
                        ent->client->xp >= rank_xp_thresholds[ent->client->rank + 1]) {
                        ent->client->rank++;
                        {
                            char rankbuf[64];
                            snprintf(rankbuf, sizeof(rankbuf), "PROMOTED: %s",
                                     rank_names[ent->client->rank]);
                            SCR_AddPickupMessage(rankbuf);
                            ent->client->score += 50;
                            SCR_AddScorePopup(50);
                        }
                    }
                    (void)old_rank;

                    /* Headshot kill camera: brief slow-mo */
                    if (ent->client->last_damage_zone == GORE_ZONE_HEAD ||
                        ent->client->last_damage_zone == GORE_ZONE_FACE) {
                        ent->client->headshot_cam_end = level.time + 0.5f;
                        level.time_scale = 0.4f;
                    }

                    /* Bullet time charge: +15 per kill, capped at 100 */
                    ent->client->bullet_time_charge += 15.0f;
                    if (ent->client->bullet_time_charge > 100.0f)
                        ent->client->bullet_time_charge = 100.0f;

                    /* Kill streak tracking — 3s window between kills */
                    if (level.time - ent->client->streak_last_kill < 3.0f)
                        ent->client->streak_count++;
                    else
                        ent->client->streak_count = 1;
                    ent->client->streak_last_kill = level.time;

                    /* Streak announcements + bonus score */
                    switch (ent->client->streak_count) {
                    case 2:
                        SCR_AddPickupMessage("DOUBLE KILL!");
                        ent->client->score += 5;
                        SCR_AddScorePopup(5);
                        break;
                    case 3:
                        SCR_AddPickupMessage("TRIPLE KILL!");
                        ent->client->score += 10;
                        SCR_AddScorePopup(10);
                        break;
                    case 4:
                        SCR_AddPickupMessage("MULTI KILL!");
                        ent->client->score += 20;
                        SCR_AddScorePopup(20);
                        break;
                    default:
                        if (ent->client->streak_count >= 5) {
                            SCR_AddPickupMessage("RAMPAGE!");
                            ent->client->score += 30;
                            SCR_AddScorePopup(30);
                        }
                        break;
                    }

                    /* Adrenaline rush: 1.5x damage for 5s on triple+ kill */
                    if (ent->client->streak_count >= 3) {
                        ent->client->adrenaline_end = level.time + 5.0f;
                        ent->client->adrenaline_mult = 1.5f;
                    }

                    /* Kill feed notification */
                    {
                        const char *victim_name = tr.ent->classname ?
                            tr.ent->classname : "unknown";
                        int w = ent->client->pers_weapon;
                        const char *weap_name = (w > 0 && w < WEAP_COUNT) ?
                            weapon_names[w] : "unknown";
                        SCR_AddKillFeed("Player", victim_name, weap_name);
                    }
                }

                /* Drop victim's weapon as a pickup */
                if (tr.ent->weapon_index > 0 && tr.ent->weapon_index < WEAP_COUNT) {
                    extern edict_t *G_DropItem(vec3_t origin, const char *classname);
                    char drop_class[64];
                    snprintf(drop_class, sizeof(drop_class), "weapon_%s",
                             weapon_names[tr.ent->weapon_index]);
                    G_DropItem(tr.ent->s.origin, drop_class);
                }

                /* Track monster kills for level stats */
                if (tr.ent->svflags & SVF_MONSTER)
                    level.killed_monsters++;
            }
        hitscan_impact_done:
            (void)0;  /* label requires a statement */
        } else {
            /* Surface-type impact effects */
            {
                const char *sname = (tr.surface && tr.surface->name[0]) ?
                    tr.surface->name : "";
                int is_metal = (strstr(sname, "metal") || strstr(sname, "iron") ||
                                strstr(sname, "steel") || strstr(sname, "grate"));
                int is_wood = (strstr(sname, "wood") || strstr(sname, "plank") ||
                               strstr(sname, "crate"));
                int is_glass = (strstr(sname, "glass") || strstr(sname, "window"));
                int is_dirt = (strstr(sname, "dirt") || strstr(sname, "sand") ||
                               strstr(sname, "ground") || strstr(sname, "grass"));

                if (is_metal) {
                    /* Metal: bright orange sparks, metallic clang */
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 12, 8);
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 3, 2);
                } else if (is_wood) {
                    /* Wood: splinters, subtle dust */
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 11, 4);
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 0, 3);
                } else if (is_glass) {
                    /* Glass: shards + sparks */
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 12, 6);
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 3, 4);
                } else if (is_dirt) {
                    /* Dirt/sand: dust cloud, no sparks */
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 0, 10);
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 13, 3);
                } else {
                    /* Default: concrete/stone — dust + small sparks */
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 0, 6);
                    R_ParticleEffect(tr.endpos, tr.plane.normal, 12, 3);
                }

                G_AddDecal(tr.endpos, tr.plane.normal, 0);

                /* Ricochet sound */
                {
                    int ric_snd = 0;
                    int r = gi.irand(0, 2);
                    if (r == 0) ric_snd = snd_ric1;
                    else if (r == 1) ric_snd = snd_ric2;
                    else ric_snd = snd_ric3;
                    if (ric_snd)
                        gi.positioned_sound(tr.endpos, NULL, CHAN_AUTO,
                                            ric_snd, 0.7f, ATTN_NORM, 0);
                }

                /* Ricochet bullet — metal surfaces bounce a weaker projectile */
                if (is_metal && weap != WEAP_SHOTGUN && weap != WEAP_KNIFE) {
                    vec3_t reflect, ric_end;
                    trace_t ric_tr;
                    float dot_n;

                    /* Reflect: R = D - 2(D.N)N */
                    dot_n = DotProduct(pellet_dir, tr.plane.normal);
                    reflect[0] = pellet_dir[0] - 2.0f * dot_n * tr.plane.normal[0];
                    reflect[1] = pellet_dir[1] - 2.0f * dot_n * tr.plane.normal[1];
                    reflect[2] = pellet_dir[2] - 2.0f * dot_n * tr.plane.normal[2];

                    /* Add some randomness to reflect */
                    reflect[0] += gi.flrand(-0.05f, 0.05f);
                    reflect[1] += gi.flrand(-0.05f, 0.05f);
                    reflect[2] += gi.flrand(-0.05f, 0.05f);

                    VectorMA(tr.endpos, 2048, reflect, ric_end);
                    ric_tr = gi.trace(tr.endpos, NULL, NULL, ric_end, ent, MASK_SHOT);

                    /* Tracer for ricochet */
                    if (ric_tr.fraction < 1.0f) {
                        R_AddTracer(tr.endpos, ric_tr.endpos, 1.0f, 0.7f, 0.3f);
                        R_ParticleEffect(ric_tr.endpos, ric_tr.plane.normal, 12, 3);

                        /* Ricochet can hit entities for half damage */
                        if (ric_tr.ent && ric_tr.ent->takedamage && ric_tr.ent->health > 0) {
                            int ric_dmg = damage / 2;
                            if (ric_dmg < 1) ric_dmg = 1;
                            ric_tr.ent->health -= ric_dmg;
                            R_ParticleEffect(ric_tr.endpos, ric_tr.plane.normal, 1, 4);
                            SCR_AddDamageNumber(ric_dmg, 0, 0);

                            if (ric_tr.ent->health <= 0 && ric_tr.ent->die) {
                                ric_tr.ent->die(ric_tr.ent, ent, ent, ric_dmg, ric_tr.endpos);
                                if (ent->client) {
                                    ent->client->kills++;
                                    ent->client->score += 10;
                                    SCR_AddScorePopup(10);
                                    SCR_AddPickupMessage("RICOCHET KILL!");
                                    ent->client->score += 10;  /* bonus */
                                    SCR_AddScorePopup(10);
                                }
                            }
                        }
                    }
                }

                /* Wall penetration: high-power weapons punch through thin surfaces */
                if ((weap == WEAP_SNIPER || weap == WEAP_SLUGGER ||
                     weap == WEAP_ASSAULT) && !is_metal) {
                    vec3_t pen_start, pen_end;
                    trace_t pen_tr;
                    int pen_dmg = damage / 3;  /* 33% damage after penetration */

                    /* Start trace 8 units past the wall surface */
                    VectorMA(tr.endpos, 8.0f, pellet_dir, pen_start);
                    VectorMA(pen_start, 1024.0f, pellet_dir, pen_end);
                    pen_tr = gi.trace(pen_start, NULL, NULL, pen_end, ent, MASK_SHOT);

                    if (pen_tr.fraction < 1.0f && pen_tr.ent &&
                        pen_tr.ent->takedamage && pen_tr.ent->health > 0 &&
                        pen_dmg > 0) {
                        /* Penetration hit! */
                        R_AddTracer(tr.endpos, pen_tr.endpos, 0.8f, 0.6f, 0.3f);
                        R_ParticleEffect(pen_tr.endpos, pen_tr.plane.normal, 1, 4);
                        pen_tr.ent->health -= pen_dmg;
                        SCR_AddDamageNumber(pen_dmg, 0, 0);
                        SCR_AddPickupMessage("WALLBANG!");

                        if (pen_tr.ent->health <= 0 && pen_tr.ent->die) {
                            pen_tr.ent->die(pen_tr.ent, ent, ent, pen_dmg, pen_tr.endpos);
                            if (ent->client) {
                                ent->client->kills++;
                                ent->client->score += 15;
                                SCR_AddScorePopup(15);
                            }
                        }
                    }
                }
            }
        }
    }

        } /* end for pellet */
    } /* end weapon-specific block */

    /* Apply weapon recoil kick */
    if (weap > 0 && weap < WEAP_COUNT && weapon_recoil[weap] > 0) {
        float recoil = weapon_recoil[weap];
        ent->client->kick_angles[0] -= recoil;  /* pitch up */
        ent->client->kick_angles[1] += gi.flrand(-recoil * 0.3f, recoil * 0.3f);  /* slight yaw */
    }
}

/* Pmove trace wrapper — uses player entity as passent */
static edict_t *pm_passent;

static trace_t PM_trace(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end)
{
    return gi.trace(start, mins, maxs, end, pm_passent, MASK_PLAYERSOLID);
}

static int PM_pointcontents(vec3_t point)
{
    return gi.pointcontents(point);
}

static void ClientThink(edict_t *ent, usercmd_t *ucmd)
{
    pmove_t pm;
    gclient_t *client;

    if (!ent || !ent->client || !ucmd)
        return;

    client = ent->client;

    /* Decay damage flash */
    if (client->blend[3] > 0) {
        client->blend[3] -= 0.05f;
        if (client->blend[3] < 0)
            client->blend[3] = 0;
    }

    /* Decay weapon kick/recoil */
    {
        int k;
        for (k = 0; k < 3; k++) {
            if (client->kick_angles[k] > 0) {
                client->kick_angles[k] -= 5.0f * level.frametime;
                if (client->kick_angles[k] < 0) client->kick_angles[k] = 0;
            } else if (client->kick_angles[k] < 0) {
                client->kick_angles[k] += 5.0f * level.frametime;
                if (client->kick_angles[k] > 0) client->kick_angles[k] = 0;
            }
        }
    }

    /* Dead — check for respawn on attack press */
    if (ent->deadflag) {
        if (ucmd->buttons & (BUTTON_ATTACK | BUTTON_USE)) {
            /* Count death on respawn */
            client->deaths++;

            /* Respawn */
            ent->health = 100;
            ent->max_health = 100;
            client->pers_health = 100;
            client->armor = 0;
            ent->deadflag = 0;
            client->ps.pm_type = PM_NORMAL;
            client->blend[3] = 0;
            client->invuln_time = level.time + 3.0f;  /* 3s spawn protection */
        }
        return;
    }

    /* Build pmove from entity and client state */
    memset(&pm, 0, sizeof(pm));
    pm.s = client->ps;
    pm.cmd = *ucmd;
    pm.trace = PM_trace;
    pm.pointcontents = PM_pointcontents;

    pm_passent = ent;

    /* Save old state for transition detection */
    {
        float old_z_vel = ent->velocity[2];
        int old_wl = client->old_waterlevel;

        /* Run player physics */
        gi.Pmove(&pm);

        /* Copy results back to entity */
        client->ps = pm.s;
        VectorCopy(pm.s.origin, ent->s.origin);
        VectorCopy(pm.s.velocity, ent->velocity);
        VectorCopy(pm.viewangles, client->viewangles);
        VectorCopy(pm.viewangles, ent->s.angles);
        ent->s.angles[0] = 0;  /* don't pitch the player model */

        /* Ladder climbing — override vertical velocity when on ladder */
        if (client->on_ladder) {
            float climb_speed = 200.0f;

            /* Forward/back input → vertical movement */
            if (ucmd->forwardmove > 0) {
                ent->velocity[2] = climb_speed;
            } else if (ucmd->forwardmove < 0) {
                ent->velocity[2] = -climb_speed;
            } else {
                /* Hold position — cancel gravity */
                ent->velocity[2] = 0;
            }

            /* Ladder climbing sound — metallic step every 0.4s while moving */
            if (ucmd->forwardmove != 0 && level.time >= client->next_footstep) {
                if (snd_ladder_step)
                    gi.sound(ent, CHAN_BODY, snd_ladder_step, 0.6f, ATTN_NORM, 0);
                client->next_footstep = level.time + 0.4f;
            }

            /* Jump off ladder */
            if (ucmd->upmove > 0) {
                vec3_t fwd, rt, u;
                G_AngleVectors(client->viewangles, fwd, rt, u);
                ent->velocity[0] = fwd[0] * 200.0f;
                ent->velocity[1] = fwd[1] * 200.0f;
                ent->velocity[2] = 250.0f;
                client->on_ladder = qfalse;
            }

            /* Sync back to pmove state */
            VectorCopy(ent->velocity, client->ps.velocity);

            /* Reset ladder flag — will be re-set by touch next frame */
            client->on_ladder = qfalse;
        }

        /* Fall damage — check if we were falling fast and just landed */
        if (pm.groundentity && !ent->groundentity && old_z_vel < -300) {
            float fall_speed = -old_z_vel;
            int fall_dmg = 0;

            if (fall_speed > 700)
                fall_dmg = (int)((fall_speed - 300) * 0.1f);
            else if (fall_speed > 500)
                fall_dmg = (int)((fall_speed - 300) * 0.05f);

            /* Crouch landing: reduce fall damage by 40% (roll technique) */
            if (fall_dmg > 0 && client->viewheight < 20.0f)
                fall_dmg = (int)(fall_dmg * 0.6f);

            if (fall_dmg > 0) {
                /* Armor absorbs some fall damage */
                if (client->armor > 0) {
                    int absorb = (int)(fall_dmg * 0.33f);
                    if (absorb > client->armor) absorb = client->armor;
                    client->armor -= absorb;
                    fall_dmg -= absorb;
                }
                ent->health -= fall_dmg;
                client->pers_health = ent->health;

                /* Red flash for fall damage */
                client->blend[0] = 1.0f;
                client->blend[1] = 0.0f;
                client->blend[2] = 0.0f;
                client->blend[3] = 0.3f;

                if (ent->health <= 0) {
                    ent->health = 0;
                    ent->deadflag = 1;
                    client->ps.pm_type = PM_DEAD;
                    SCR_AddKillFeed("Player", "falling", "environment");
                    client->deaths++;
                }
            }

            /* Landing camera dip — proportional to fall speed */
            {
                float dip = (fall_speed - 300) * 0.01f;
                if (dip > 3.0f) dip = 3.0f;
                client->kick_angles[0] += dip;  /* pitch down on landing */
            }
        } else if (pm.groundentity && !ent->groundentity && old_z_vel < -150) {
            /* Soft landing from small falls — subtle camera dip */
            client->kick_angles[0] += 0.5f;
        }

        ent->groundentity = pm.groundentity;
        client->viewheight = pm.viewheight;

        /* Water splash effects — detect water entry/exit transitions */
        {
            int new_wl = pm.waterlevel;

            if (old_wl == 0 && new_wl >= 1) {
                /* Entering water — big splash */
                vec3_t splash_org, splash_up;
                VectorCopy(ent->s.origin, splash_org);
                VectorSet(splash_up, 0, 0, 1);
                R_ParticleEffect(splash_org, splash_up, 6, 16);   /* water drips */
                R_ParticleEffect(splash_org, splash_up, 10, 4);   /* mist/spray */
                if (snd_splash_in)
                    gi.sound(ent, CHAN_BODY, snd_splash_in, 1.0f, ATTN_NORM, 0);
            } else if (old_wl >= 1 && new_wl == 0) {
                /* Exiting water — smaller splash + dripping */
                vec3_t splash_org, splash_up;
                VectorCopy(ent->s.origin, splash_org);
                VectorSet(splash_up, 0, 0, 1);
                R_ParticleEffect(splash_org, splash_up, 6, 8);    /* water drips */
                if (snd_splash_out)
                    gi.sound(ent, CHAN_BODY, snd_splash_out, 0.8f, ATTN_NORM, 0);
            } else if (old_wl < 3 && new_wl >= 3) {
                /* Going fully underwater — submerge */
                vec3_t splash_org, splash_up;
                VectorCopy(ent->s.origin, splash_org);
                VectorSet(splash_up, 0, 0, 1);
                R_ParticleEffect(splash_org, splash_up, 6, 12);
            }

            client->old_waterlevel = new_wl;
        }
    }

    /* Wall jump: jump off a wall while airborne near a wall */
    if (!ent->groundentity && !ent->deadflag && ucmd->upmove > 0 &&
        level.time > client->wall_jump_time + 0.5f) {
        /* Trace to the sides to find a wall */
        vec3_t wj_fwd, wj_rt, wj_up;
        vec3_t wj_start, wj_end;
        trace_t wj_tr;
        int wj_dir;

        G_AngleVectors(client->viewangles, wj_fwd, wj_rt, wj_up);
        VectorCopy(ent->s.origin, wj_start);
        wj_start[2] += client->viewheight;

        for (wj_dir = 0; wj_dir < 4; wj_dir++) {
            vec3_t dir;
            switch (wj_dir) {
            case 0: VectorCopy(wj_fwd, dir); break;
            case 1: VectorScale(wj_fwd, -1, dir); break;
            case 2: VectorCopy(wj_rt, dir); break;
            case 3: VectorScale(wj_rt, -1, dir); break;
            }
            VectorMA(wj_start, 32, dir, wj_end);
            wj_tr = gi.trace(wj_start, NULL, NULL, wj_end, ent, MASK_SOLID);

            if (wj_tr.fraction < 1.0f && !(wj_tr.surface && (wj_tr.surface->flags & SURF_SKY))) {
                /* Wall found — kick off it */
                ent->velocity[0] += wj_tr.plane.normal[0] * 250.0f;
                ent->velocity[1] += wj_tr.plane.normal[1] * 250.0f;
                ent->velocity[2] = 280.0f;  /* upward boost */
                client->wall_jump_time = level.time;
                VectorCopy(ent->velocity, client->ps.velocity);

                /* Dust puff at wall contact point */
                R_ParticleEffect(wj_tr.endpos, wj_tr.plane.normal, 13, 4);
                break;
            }
        }
    }

    /* Dash/dodge: double-tap movement direction for quick dodge */
    if (client->dash_end > level.time) {
        /* Dashing — apply dash velocity */
        float dash_frac = (client->dash_end - level.time) / 0.2f;
        float dash_speed = 500.0f * dash_frac;
        ent->velocity[0] = client->dash_dir[0] * dash_speed;
        ent->velocity[1] = client->dash_dir[1] * dash_speed;
    } else if (ent->groundentity && client->dash_cooldown < level.time &&
               client->stamina > 15.0f) {
        /* Check for BUTTON_SPRINT + strafe = dash sideways */
        if ((ucmd->buttons & BUTTON_SPRINT) && ucmd->forwardmove == 0 &&
            (ucmd->sidemove > 100 || ucmd->sidemove < -100)) {
            vec3_t dash_fwd, dash_rt, dash_up;
            G_AngleVectors(client->viewangles, dash_fwd, dash_rt, dash_up);
            if (ucmd->sidemove > 0) {
                VectorCopy(dash_rt, client->dash_dir);
            } else {
                VectorScale(dash_rt, -1.0f, client->dash_dir);
            }
            client->dash_dir[2] = 0;
            client->dash_end = level.time + 0.2f;
            client->dash_cooldown = level.time + 0.8f;
            client->stamina -= 15.0f;
        }
    }

    /* Slide mechanic: sprint + crouch = forward slide */
    if (client->slide_end > level.time) {
        /* Currently sliding — apply slide velocity and crouch */
        float slide_frac = (client->slide_end - level.time) / 0.6f;
        float slide_speed = 350.0f * slide_frac;  /* decelerating slide */
        ent->velocity[0] = client->slide_dir[0] * slide_speed;
        ent->velocity[1] = client->slide_dir[1] * slide_speed;
        client->viewheight = 10;  /* crouched during slide */
        ent->maxs[2] = 16;
        /* Slight camera tilt during slide */
        client->kick_angles[0] += slide_frac * 2.0f;
    } else if (client->sprinting && (ucmd->buttons & BUTTON_CROUCH) &&
               ucmd->forwardmove > 0 && ent->groundentity &&
               client->stamina > 20.0f) {
        /* Initiate slide */
        vec3_t slide_fwd, slide_rt, slide_up;
        G_AngleVectors(client->viewangles, slide_fwd, slide_rt, slide_up);
        client->slide_dir[0] = slide_fwd[0];
        client->slide_dir[1] = slide_fwd[1];
        client->slide_dir[2] = 0;
        client->slide_end = level.time + 0.6f;  /* 0.6 second slide */
        client->stamina -= 20.0f;  /* costs stamina */
        client->sprinting = qfalse;

        /* Dust trail particles */
        {
            vec3_t dust_down = {0, 0, -1};
            R_ParticleEffect(ent->s.origin, dust_down, 13, 6);
        }
    }

    /* Prone handling — lowest stance, nearly flat */
    if (ucmd->buttons & BUTTON_PRONE) {
        if (client->viewheight > 4) {
            client->viewheight = 4;    /* prone eye height */
            ent->maxs[2] = 8;         /* very low bbox */
        }
        /* Can't sprint while prone */
        client->sprinting = qfalse;
    }
    /* Crouch handling */
    else if (ucmd->buttons & BUTTON_CROUCH) {
        if (client->viewheight > 10) {
            client->viewheight = 10;  /* crouched eye height */
            ent->maxs[2] = 16;  /* reduced bbox height */
        }
    } else {
        if (client->viewheight < 22) {
            client->viewheight = 22;  /* standing eye height */
            ent->maxs[2] = 32;  /* full bbox height */
        }
    }

    /* Head bob — subtle view oscillation while moving */
    if (client->old_waterlevel >= 2 && !ent->deadflag) {
        /* Underwater floating bob — slow, gentle undulation */
        client->bob_time += level.frametime * 3.0f;
        client->viewheight += (int)(sinf(client->bob_time) * 1.5f);
        /* Gentle roll sway for floating feel */
        client->kick_angles[2] = sinf(client->bob_time * 0.7f) * 0.5f;
        /* Slight pitch drift */
        client->kick_angles[0] += sinf(client->bob_time * 1.3f) * 0.15f;

        /* Underwater bubble particles — periodic stream upward */
        if (client->old_waterlevel >= 3 && gi.irand(0, 10) == 0) {
            vec3_t bubble_org, bubble_up;
            VectorCopy(ent->s.origin, bubble_org);
            bubble_org[2] += client->viewheight;
            VectorSet(bubble_up, 0, 0, 1);
            R_ParticleEffect(bubble_org, bubble_up, 6, 2);  /* water bubbles */
        }

        /* Blue underwater tint */
        if (client->blend[3] < 0.15f) {
            client->blend[0] = 0.0f;
            client->blend[1] = 0.1f;
            client->blend[2] = 0.4f;
            client->blend[3] = 0.15f;
        }
    } else if (ent->groundentity && !ent->deadflag) {
        float speed = (float)sqrt(ent->velocity[0] * ent->velocity[0] +
                                   ent->velocity[1] * ent->velocity[1]);
        if (speed > 40.0f) {
            float bob_scale = (speed > 200.0f) ? 0.8f : speed / 250.0f;
            float bob_freq = (speed > 200.0f) ? 12.0f : 8.0f;
            client->bob_time += level.frametime * bob_freq;
            client->viewheight += (int)(sinf(client->bob_time) * bob_scale);

            /* Strafe lean: subtle roll when strafing */
            if (ucmd->sidemove > 100)
                client->kick_angles[2] += (-1.2f - client->kick_angles[2]) * 0.1f;
            else if (ucmd->sidemove < -100)
                client->kick_angles[2] += (1.2f - client->kick_angles[2]) * 0.1f;
            else
                client->kick_angles[2] *= 0.9f;  /* decay back to center */
        } else {
            /* Idle sway — breathing cycle affects pitch and roll */
            client->bob_time += level.frametime * 2.0f;
            client->kick_angles[2] = sinf(client->bob_time) * 0.1f;
            client->kick_angles[0] += sinf(client->bob_time * 0.5f) * 0.08f; /* breathe pitch */

            /* Scoped sway: amplified when zoomed (unsteady hold) */
            if (client->zoomed) {
                float sway_amp = 0.25f;
                /* Crouching reduces scope sway */
                if (client->viewheight < 20)
                    sway_amp = 0.12f;
                client->kick_angles[0] += sinf(client->bob_time * 1.7f) * sway_amp;
                client->kick_angles[1] += cosf(client->bob_time * 1.3f) * sway_amp * 0.7f;
            }
        }
    }

    /* Lean handling — offset view position laterally */
    {
        int target_lean = 0;
        float lean_speed = 4.0f * level.frametime;  /* lean rate */
        float max_lean = 12.0f;  /* max lateral offset in units */

        if (ucmd->buttons & BUTTON_LEAN_LEFT)
            target_lean = -1;
        else if (ucmd->buttons & BUTTON_LEAN_RIGHT)
            target_lean = 1;

        client->lean_state = target_lean;

        if (target_lean < 0) {
            if (client->lean_offset > -max_lean)
                client->lean_offset -= lean_speed * 40.0f;
            if (client->lean_offset < -max_lean)
                client->lean_offset = -max_lean;
        } else if (target_lean > 0) {
            if (client->lean_offset < max_lean)
                client->lean_offset += lean_speed * 40.0f;
            if (client->lean_offset > max_lean)
                client->lean_offset = max_lean;
        } else {
            /* Return to center */
            if (client->lean_offset > 0) {
                client->lean_offset -= lean_speed * 40.0f;
                if (client->lean_offset < 0) client->lean_offset = 0;
            } else if (client->lean_offset < 0) {
                client->lean_offset += lean_speed * 40.0f;
                if (client->lean_offset > 0) client->lean_offset = 0;
            }
        }

        /* Apply lean as kick_origin lateral offset */
        if (client->lean_offset != 0) {
            vec3_t right_dir, fwd_dir, up_dir;
            G_AngleVectors(client->viewangles, fwd_dir, right_dir, up_dir);
            VectorScale(right_dir, client->lean_offset, client->kick_origin);
        } else {
            VectorClear(client->kick_origin);
        }
    }

    /* Concussion effect — view sway and spread penalty */
    if (client->concussion_end > level.time) {
        float sway_t = level.time * 5.0f;
        float intensity = (client->concussion_end - level.time) / 2.0f;
        if (intensity > 1.0f) intensity = 1.0f;
        client->kick_angles[0] += sinf(sway_t) * 2.0f * intensity;
        client->kick_angles[1] += cosf(sway_t * 0.7f) * 3.0f * intensity;
        client->kick_angles[2] += sinf(sway_t * 1.3f) * 1.5f * intensity;
    }

    /* Slow-motion dive completion */
    if (client->dive_end > 0 && level.time >= client->dive_end) {
        level.time_scale = 1.0f;  /* restore normal speed */
        client->dive_end = 0;
    }

    /* Weapon inspect animation: sway weapon while inspecting */
    if (client->inspect_end > level.time) {
        float t = (client->inspect_end - level.time) / 2.0f;
        client->kick_angles[1] += sinf(t * 8.0f) * 5.0f;  /* yaw wobble */
        client->kick_angles[0] += cosf(t * 6.0f) * 3.0f;  /* pitch wobble */
    }

    /* Footstep sounds — on ground and moving, with surface material detection */
    if (ent->groundentity && !ent->deadflag) {
        float speed = (float)sqrt(ent->velocity[0] * ent->velocity[0] +
                                   ent->velocity[1] * ent->velocity[1]);
        if (speed > 50.0f && level.time >= client->next_footstep) {
            int step_snd = 0;
            int r = gi.irand(0, 3);
            float vol = 0.5f;

            /* Check surface material via downward trace */
            {
                vec3_t down_start, down_end;
                trace_t step_tr;

                VectorCopy(ent->s.origin, down_start);
                VectorCopy(ent->s.origin, down_end);
                down_end[2] -= 32;

                step_tr = gi.trace(down_start, NULL, NULL, down_end, ent, MASK_SOLID);

                if (step_tr.surface && step_tr.surface->name[0]) {
                    /* Check texture name for surface type hints */
                    const char *texname = step_tr.surface->name;
                    if (strstr(texname, "metal") || strstr(texname, "grate") ||
                        strstr(texname, "vent")) {
                        /* Metal surface — louder, sharper */
                        vol = 0.7f;
                    } else if (strstr(texname, "water") || strstr(texname, "mud") ||
                               strstr(texname, "slime")) {
                        /* Wet surface — splashy, quieter */
                        vol = 0.3f;
                    } else if (strstr(texname, "dirt") || strstr(texname, "grass") ||
                               strstr(texname, "sand")) {
                        /* Soft surface — muffled */
                        vol = 0.35f;
                    }
                    /* Default concrete/tile stays at 0.5 */
                }
            }

            if (r == 0) step_snd = snd_footstep1;
            else if (r == 1) step_snd = snd_footstep2;
            else if (r == 2) step_snd = snd_footstep3;
            else step_snd = snd_footstep4;
            if (step_snd)
                gi.sound(ent, CHAN_BODY, step_snd, vol, ATTN_NORM, 0);

            /* Footstep dust particles when running fast */
            if (speed > 150.0f) {
                vec3_t foot_pos, foot_dir;
                VectorCopy(ent->s.origin, foot_pos);
                foot_pos[2] -= 20;  /* at feet level */
                VectorSet(foot_dir, 0, 0, 1);
                R_ParticleEffect(foot_pos, foot_dir, 13,
                                 speed > 250.0f ? 3 : 1);
            }

            /* Faster footsteps at higher speed */
            client->next_footstep = level.time + (speed > 200.0f ? 0.3f : 0.5f);
        }
    }

    /* Ladder climbing — check if player is touching CONTENTS_LADDER */
    {
        int ladder_contents = gi.pointcontents(ent->s.origin);
        if (ladder_contents & CONTENTS_LADDER) {
            /* On ladder: override vertical velocity based on forward movement */
            float climb_speed = 200.0f;

            if (ucmd->forwardmove > 0) {
                /* Looking up + forward = climb up */
                ent->velocity[2] = climb_speed;
            } else if (ucmd->forwardmove < 0) {
                /* Back = climb down */
                ent->velocity[2] = -climb_speed;
            } else {
                /* No input = hang on ladder */
                ent->velocity[2] = 0;
            }

            /* Reduce horizontal speed on ladder */
            ent->velocity[0] *= 0.5f;
            ent->velocity[1] *= 0.5f;
        }
    }

    /* Environmental damage: lava, slime, drowning */
    {
        int contents = gi.pointcontents(ent->s.origin);

        /* Lava: rapid damage */
        if (contents & CONTENTS_LAVA) {
            if (level.time >= client->next_env_damage) {
                ent->health -= 10;
                client->pers_health = ent->health;
                client->blend[0] = 1.0f;
                client->blend[1] = 0.3f;
                client->blend[2] = 0.0f;
                client->blend[3] = 0.4f;
                client->next_env_damage = level.time + 0.2f;

                if (ent->health <= 0) {
                    ent->health = 0;
                    ent->deadflag = 1;
                    client->ps.pm_type = PM_DEAD;
                    SCR_AddKillFeed("Player", "lava", "environment");
                    client->deaths++;
                }
            }
        }
        /* Slime: slow damage */
        else if (contents & CONTENTS_SLIME) {
            if (level.time >= client->next_env_damage) {
                ent->health -= 4;
                client->pers_health = ent->health;
                client->blend[0] = 0.0f;
                client->blend[1] = 0.5f;
                client->blend[2] = 0.0f;
                client->blend[3] = 0.3f;
                client->next_env_damage = level.time + 1.0f;

                if (ent->health <= 0) {
                    ent->health = 0;
                    ent->deadflag = 1;
                    client->ps.pm_type = PM_DEAD;
                    SCR_AddKillFeed("Player", "slime", "environment");
                    client->deaths++;
                }
            }
        }

        /* Drowning: underwater without air */
        if (contents & CONTENTS_WATER) {
            if (client->air_finished == 0)
                client->air_finished = level.time + 12.0f;  /* 12 seconds of air */

            if (level.time > client->air_finished) {
                if (level.time >= client->next_env_damage) {
                    ent->health -= 5;
                    client->pers_health = ent->health;
                    client->blend[0] = 0.0f;
                    client->blend[1] = 0.0f;
                    client->blend[2] = 1.0f;
                    client->blend[3] = 0.3f;
                    client->next_env_damage = level.time + 1.0f;
                    if (snd_drown)
                        gi.sound(ent, CHAN_VOICE, snd_drown, 1.0f, ATTN_NORM, 0);

                    if (ent->health <= 0) {
                        ent->health = 0;
                        ent->deadflag = 1;
                        client->ps.pm_type = PM_DEAD;
                        SCR_AddKillFeed("Player", "drowning", "environment");
                        client->deaths++;
                    }
                }
            }
        } else {
            /* Above water — reset breath */
            client->air_finished = 0;
        }
    }

    /* Weather particle effects — spawn rain or snow around the player */
    if (level.weather > 0) {
        /* Check if player is outdoors by tracing up to sky */
        vec3_t sky_start, sky_end;
        trace_t sky_tr;
        VectorCopy(ent->s.origin, sky_start);
        sky_start[2] += 32;
        VectorCopy(sky_start, sky_end);
        sky_end[2] += 4096;
        sky_tr = gi.trace(sky_start, NULL, NULL, sky_end, ent, MASK_SOLID);

        if (sky_tr.surface && (sky_tr.surface->flags & SURF_SKY)) {
            /* Player is outdoors — spawn weather particles */
            vec3_t weather_org, weather_dir;
            int particle_count;
            VectorCopy(ent->s.origin, weather_org);
            VectorSet(weather_dir, 0, 0, -1);

            particle_count = (int)(level.weather_density * 8);
            if (particle_count < 1) particle_count = 1;

            if (level.weather == 1) {
                /* Rain */
                R_ParticleEffect(weather_org, weather_dir, 14, particle_count);
            } else if (level.weather == 2) {
                /* Snow */
                R_ParticleEffect(weather_org, weather_dir, 15, particle_count);
            }
        }
    }

    /* Process touch callbacks from Pmove */
    {
        int i;
        for (i = 0; i < pm.numtouch; i++) {
            edict_t *other = pm.touchents[i];
            if (other && other->touch)
                other->touch(other, ent, NULL, NULL);
        }
    }

    /* Proximity warning — subtle alert when enemy is very close behind */
    if (!ent->deadflag && level.time >= client->next_ambient + 2.0f) {
        edict_t *touch[16];
        int num, i;
        vec3_t prox_mins, prox_maxs;
        float prox_range = 200.0f;

        VectorSet(prox_mins, ent->s.origin[0] - prox_range,
                             ent->s.origin[1] - prox_range,
                             ent->s.origin[2] - prox_range);
        VectorSet(prox_maxs, ent->s.origin[0] + prox_range,
                             ent->s.origin[1] + prox_range,
                             ent->s.origin[2] + prox_range);

        num = gi.BoxEdicts(prox_mins, prox_maxs, touch, 16, AREA_SOLID);
        for (i = 0; i < num; i++) {
            if (touch[i] && (touch[i]->svflags & SVF_MONSTER) &&
                touch[i]->health > 0 && !touch[i]->deadflag) {
                /* Check if enemy is behind the player */
                vec3_t to_enemy, fwd, rt, up;
                float dot;
                VectorSubtract(touch[i]->s.origin, ent->s.origin, to_enemy);
                G_AngleVectors(client->viewangles, fwd, rt, up);
                dot = DotProduct(to_enemy, fwd);

                if (dot < -50.0f) {
                    /* Enemy behind player — warning flash */
                    client->blend[0] = 1.0f;
                    client->blend[1] = 0.8f;
                    client->blend[2] = 0.0f;
                    client->blend[3] = 0.05f;  /* very subtle yellow flash */
                    client->next_ambient = level.time;
                    break;
                }
            }
        }
    }

    /* Recoil decay — spread recovers when not firing */
    if (client->recoil_accum > 0) {
        client->recoil_accum -= 2.5f * level.frametime;  /* recover in ~0.4s */
        if (client->recoil_accum < 0) client->recoil_accum = 0;
    }

    /* Weapon heat passive decay */
    if (client->weapon_heat > 0 && !client->weapon_overheated) {
        client->weapon_heat -= 0.15f * level.frametime;  /* cools in ~6.5s idle */
        if (client->weapon_heat < 0) client->weapon_heat = 0;
    }

    /* Coop revive: downed player bleedout timer */
    if (client->downed) {
        if (level.time >= client->bleedout_end) {
            /* Bleedout — actually die */
            client->downed = qfalse;
            ent->health = -1;
            ent->deadflag = 1;
            client->ps.pm_type = PM_DEAD;
            client->pers_health = 0;
        }
        /* Can't move or shoot while downed */
        ent->velocity[0] = ent->velocity[1] = 0;
        return;
    }

    /* Reload completion check */
    if (client->reloading_weapon > 0) {
        if (level.time >= client->reload_finish_time) {
            int w = client->reloading_weapon;
            int mag_size = weapon_magazine_size[w];
            int need = mag_size - client->magazine[w];
            int avail = client->ammo[w];
            int load = (need < avail) ? need : avail;

            client->ammo[w] -= load;
            client->magazine[w] += load;
            client->reloading_weapon = 0;
            gi.cprintf(ent, PRINT_ALL, "Reloaded %s (%d/%d)\n",
                       weapon_names[w], client->magazine[w], mag_size);
        }
        /* Can't fire while reloading */
    } else {
        /* Can't fire while weapon is holstered — auto-unholster on attack */
        if (client->weapon_holstered && (ucmd->buttons & (BUTTON_ATTACK | BUTTON_ATTACK2))) {
            client->weapon_holstered = qfalse;
            client->pers_weapon = client->holster_weapon;
            ent->weapon_index = client->holster_weapon;
            client->weapon_change_time = level.time + WEAPON_SWITCH_TIME;
        }

        /* Grenade cooking: hold attack to cook, release to throw */
        if (client->pers_weapon == WEAP_GRENADE && client->ammo[WEAP_GRENADE] > 0) {
            if (ucmd->buttons & BUTTON_ATTACK) {
                /* Start/continue cooking */
                if (client->grenade_cook_start == 0)
                    client->grenade_cook_start = level.time;
                /* Held too long — explodes in hand at 2.5s */
                if (level.time - client->grenade_cook_start > 2.5f) {
                    client->grenade_cook_start = 0;
                    client->ammo[WEAP_GRENADE]--;
                    /* Explodes on the player — self damage */
                    {
                        vec3_t up = {0, 0, 1};
                        R_ParticleEffect(ent->s.origin, up, 2, 24);
                        R_AddDlight(ent->s.origin, 1.0f, 0.5f, 0.1f, 350.0f, 0.4f);
                    }
                    ent->health -= weapon_damage[WEAP_GRENADE];
                    client->pers_health = ent->health;
                    SCR_AddPickupMessage("Cooked too long!");
                    if (ent->health <= 0) {
                        ent->deadflag = 1;
                        client->ps.pm_type = PM_DEAD;
                        SCR_AddKillFeed("Player", "own grenade", "environment");
                        client->deaths++;
                    }
                }
            } else if (client->grenade_cook_start > 0) {
                /* Released — fire the cooked grenade */
                if (snd_weapons[WEAP_GRENADE])
                    gi.sound(ent, CHAN_WEAPON, snd_weapons[WEAP_GRENADE], 1.0f, ATTN_NORM, 0);
                client->ammo[WEAP_GRENADE]--;
                G_FireProjectile(ent, qtrue);
            }
        } else {
            /* Clear cook timer if not holding grenade */
            client->grenade_cook_start = 0;
        }

        /* Fire weapon on attack button (primary or alt-fire) */
        /* Skip normal fire when grenade cooking is active */
        if (client->pers_weapon != WEAP_GRENADE || client->ammo[WEAP_GRENADE] <= 0) {
        /* If holding a throwable object, throw it instead of firing */
        if (client->held_object && (ucmd->buttons & BUTTON_ATTACK)) {
            edict_t *obj = client->held_object;
            vec3_t throw_fwd, throw_rt, throw_up;

            G_AngleVectors(client->viewangles, throw_fwd, throw_rt, throw_up);

            /* Position object in front of player */
            VectorMA(ent->s.origin, 48, throw_fwd, obj->s.origin);
            obj->s.origin[2] += client->viewheight;

            /* Launch it */
            VectorScale(throw_fwd, 800.0f, obj->velocity);
            obj->velocity[2] += 100.0f;   /* slight upward arc */
            obj->movetype = MOVETYPE_BOUNCE;
            obj->solid = SOLID_BBOX;
            obj->owner = ent;
            obj->dmg = 15;  /* Reset impact damage */
            gi.linkentity(obj);

            client->held_object = NULL;
        } else if (ucmd->buttons & BUTTON_ATTACK) {
            player_alt_fire = qfalse;
            G_FireHitscan(ent);
        } else if (ucmd->buttons & BUTTON_ATTACK2) {
            player_alt_fire = qtrue;
            G_FireHitscan(ent);
        }
        } /* end grenade cook gate */
    }

    /* Update held object position — float in front of player */
    if (client->held_object) {
        edict_t *obj = client->held_object;
        vec3_t hold_fwd, hold_rt, hold_up;

        G_AngleVectors(client->viewangles, hold_fwd, hold_rt, hold_up);
        VectorMA(ent->s.origin, 48, hold_fwd, obj->s.origin);
        obj->s.origin[2] += client->viewheight - 8;
        VectorClear(obj->velocity);
        gi.linkentity(obj);
    }

    /* Use interaction — short-range trace to find usable entities */
    if (ucmd->buttons & BUTTON_USE) {
        vec3_t use_start, use_end, use_fwd, use_rt, use_up;
        trace_t use_tr;

        VectorCopy(ent->s.origin, use_start);
        use_start[2] += client->viewheight;
        G_AngleVectors(client->viewangles, use_fwd, use_rt, use_up);
        VectorMA(use_start, 96, use_fwd, use_end);  /* 96 unit range */

        use_tr = gi.trace(use_start, NULL, NULL, use_end, ent, MASK_SHOT);
        if (use_tr.fraction < 1.0f && use_tr.ent) {
            edict_t *target = use_tr.ent;
            if (target->use)
                target->use(target, ent, ent);
        }
    }

    /* Flashlight — project dynamic light in view direction */
    if (client->flashlight_on) {
        vec3_t fl_start, fl_end, fl_fwd, fl_rt, fl_up;
        trace_t fl_tr;

        VectorCopy(ent->s.origin, fl_start);
        fl_start[2] += client->viewheight;
        G_AngleVectors(client->viewangles, fl_fwd, fl_rt, fl_up);
        VectorMA(fl_start, 512, fl_fwd, fl_end);

        fl_tr = gi.trace(fl_start, NULL, NULL, fl_end, ent, MASK_SHOT);
        R_AddDlight(fl_tr.endpos, 1.0f, 1.0f, 0.9f, 300.0f, level.frametime + 0.05f);
    }

    /* Night vision goggles — green tint + ambient light boost */
    if (client->goggles_on) {
        client->goggles_battery -= level.frametime * 3.33f;  /* ~30s of battery */
        if (client->goggles_battery <= 0) {
            client->goggles_battery = 0;
            client->goggles_on = qfalse;
            gi.cprintf(ent, PRINT_ALL, "Goggles battery depleted\n");
        } else {
            /* Green screen tint */
            client->blend[0] = 0.0f;
            client->blend[1] = 0.4f;
            client->blend[2] = 0.0f;
            client->blend[3] = 0.15f;
            /* Ambient light around player */
            R_AddDlight(ent->s.origin, 0.2f, 0.8f, 0.2f, 400.0f, level.frametime + 0.05f);
        }
    }

    /* Field pack trickle heal */
    if (client->fpak_heal_remaining > 0) {
        int heal_per_tick = (int)(25.0f * level.frametime / 3.0f);
        if (heal_per_tick < 1) heal_per_tick = 1;
        if (heal_per_tick > client->fpak_heal_remaining)
            heal_per_tick = client->fpak_heal_remaining;

        ent->health += heal_per_tick;
        if (ent->health > ent->max_health)
            ent->health = ent->max_health;
        client->pers_health = ent->health;
        client->fpak_heal_remaining -= heal_per_tick;

        if (client->fpak_heal_remaining <= 0 || ent->health >= ent->max_health) {
            client->fpak_heal_remaining = 0;
            client->fpak_heal_end = 0;
        }
    }

    /* Environmental ambient sounds — periodic based on surroundings */
    if (level.time >= client->next_ambient && !ent->deadflag) {
        int contents = gi.pointcontents(ent->s.origin);

        if (contents & (CONTENTS_WATER | CONTENTS_SLIME)) {
            /* Near/in water — dripping/bubbling sound */
            if (snd_ambient_drip)
                gi.sound(ent, CHAN_AUTO, snd_ambient_drip, 0.2f, ATTN_IDLE, 0);
            client->next_ambient = level.time + 3.0f + (float)(rand() % 40) * 0.1f;
        } else {
            /* Check if outdoors */
            vec3_t sky_start, sky_end;
            trace_t sky_tr;
            VectorCopy(ent->s.origin, sky_start);
            sky_start[2] += 32;
            VectorCopy(sky_start, sky_end);
            sky_end[2] += 4096;
            sky_tr = gi.trace(sky_start, NULL, NULL, sky_end, ent, MASK_SOLID);

            if (sky_tr.surface && (sky_tr.surface->flags & SURF_SKY)) {
                /* Outdoors — wind sound */
                if (snd_ambient_wind)
                    gi.sound(ent, CHAN_AUTO, snd_ambient_wind, 0.15f, ATTN_IDLE, 0);
                client->next_ambient = level.time + 5.0f + (float)(rand() % 50) * 0.1f;
            } else {
                /* Indoors — no ambient, just set longer cooldown */
                client->next_ambient = level.time + 8.0f;
            }
        }
    }

    /* Low health heartbeat warning — plays periodically when health is critical */
    if (!ent->deadflag && ent->health > 0 && ent->health <= 25) {
        /* Heartbeat rate increases as health decreases */
        float interval = 1.5f;
        if (ent->health <= 10) interval = 0.8f;
        else if (ent->health <= 15) interval = 1.0f;

        if (level.time >= client->next_pain_sound + interval - 0.5f) {
            if (snd_heartbeat)
                gi.sound(ent, CHAN_AUTO, snd_heartbeat, 0.6f, ATTN_NORM, 0);

            /* Slight red screen pulse to match heartbeat */
            if (client->blend[3] < 0.15f) {
                client->blend[0] = 0.8f;
                client->blend[1] = 0.0f;
                client->blend[2] = 0.0f;
                client->blend[3] = 0.15f;
            }
        }
    }

    /* Status effects: burning */
    if (!ent->deadflag && client->burn_end > level.time) {
        if (level.time >= client->burn_next_tick) {
            ent->health -= 3;
            client->pers_health = ent->health;
            client->burn_next_tick = level.time + 0.5f;

            /* Orange screen flash */
            client->blend[0] = 1.0f;
            client->blend[1] = 0.5f;
            client->blend[2] = 0.0f;
            client->blend[3] = 0.2f;

            /* Fire particles on player */
            {
                vec3_t up = {0, 0, 1};
                R_ParticleEffect(ent->s.origin, up, 4, 3);  /* flame */
            }

            if (ent->health <= 0) {
                ent->deadflag = 1;
                client->ps.pm_type = PM_DEAD;
                SCR_AddKillFeed("Player", "fire", "environment");
                client->deaths++;
            }
        }
    }

    /* Status effects: bleeding */
    if (!ent->deadflag && client->bleed_end > level.time) {
        if (level.time >= client->bleed_next_tick) {
            ent->health -= 2;
            client->pers_health = ent->health;
            client->bleed_next_tick = level.time + 1.0f;

            /* Blood drip particles */
            {
                vec3_t down = {0, 0, -1};
                R_ParticleEffect(ent->s.origin, down, 1, 2);
            }

            if (ent->health <= 0) {
                ent->deadflag = 1;
                client->ps.pm_type = PM_DEAD;
                SCR_AddKillFeed("Player", "bleeding", "environment");
                client->deaths++;
            }
        }
    }

    /* Status effects: poison (from acid/environmental) */
    if (!ent->deadflag && client->poison_end > level.time) {
        if (level.time >= client->poison_next_tick) {
            ent->health -= 1;
            client->pers_health = ent->health;
            client->poison_next_tick = level.time + 0.8f;

            /* Green tint screen flash */
            client->blend[0] = 0.1f;
            client->blend[1] = 0.8f;
            client->blend[2] = 0.1f;
            client->blend[3] = 0.15f;

            /* Green particles */
            {
                vec3_t up = {0, 0, 1};
                R_ParticleEffect(ent->s.origin, up, 7, 2);  /* steam/gas */
            }

            if (ent->health <= 0) {
                ent->deadflag = 1;
                client->ps.pm_type = PM_DEAD;
                SCR_AddKillFeed("Player", "poison", "environment");
                client->deaths++;
            }
        }
    }

    /* Flashbang blindness */
    if (!ent->deadflag && client->flash_end > level.time) {
        /* White-out screen */
        float intensity = (client->flash_end - level.time) / 3.0f;
        if (intensity > 1.0f) intensity = 1.0f;
        client->blend[0] = 1.0f;
        client->blend[1] = 1.0f;
        client->blend[2] = 1.0f;
        client->blend[3] = intensity * 0.9f;
    }

    /* Night vision green tint */
    if (!ent->deadflag && client->nightvision) {
        client->blend[0] = 0.0f;
        client->blend[1] = 0.3f;
        client->blend[2] = 0.0f;
        client->blend[3] = 0.15f;
        /* Boost ambient light via rdflags */
        client->rdflags |= 0x04;  /* RDF_IRGOGGLES */
    }

    /* Lean system — smoothly interpolate lean offset */
    if (!ent->deadflag) {
        float target = (client->lean_state != 0) ? 1.0f : 0;

        if (client->lean_offset < target)
            client->lean_offset += 4.0f * level.frametime;
        else if (client->lean_offset > target)
            client->lean_offset -= 4.0f * level.frametime;

        if (client->lean_offset < 0) client->lean_offset = 0;
        if (client->lean_offset > 1.0f) client->lean_offset = 1.0f;

        /* Apply lean as roll angle */
        if (client->lean_offset > 0.01f) {
            float lean_angle = client->lean_offset * 15.0f * client->lean_state;
            client->viewangles[2] = lean_angle;  /* roll */
        } else {
            client->viewangles[2] = 0;
        }
    }

    /* Tactical sprint: activate on BUTTON_SPRINT when moving forward */
    if (!ent->deadflag) {
        /* Initialize stamina on first use */
        if (client->stamina == 0 && !client->sprinting)
            client->stamina = 100.0f;

        if (ucmd->buttons & BUTTON_SPRINT) {
            if (!client->sprinting && client->stamina > 10.0f &&
                ucmd->forwardmove > 0) {
                client->sprinting = qtrue;
            }
        } else {
            client->sprinting = qfalse;
        }

        if (client->sprinting) {
            /* Drain stamina while sprinting */
            client->stamina -= 15.0f * level.frametime;
            if (client->stamina <= 0) {
                client->stamina = 0;
                client->sprinting = qfalse;
            }
            /* Speed boost: 1.4x forward speed */
            if (client->sprinting) {
                ent->velocity[0] *= 1.4f;
                ent->velocity[1] *= 1.4f;
            }
        } else {
            /* Regen stamina when not sprinting */
            if (client->stamina < 100.0f) {
                client->stamina += 8.0f * level.frametime;
                if (client->stamina > 100.0f) client->stamina = 100.0f;
            }
        }
    }

    /* Holstered weapon — 15% movement speed boost */
    if (client->weapon_holstered && !client->sprinting) {
        ent->velocity[0] *= 1.15f;
        ent->velocity[1] *= 1.15f;
    }

    /* Bullet time: activate on button press if charge >= 50 */
    if (!ent->deadflag) {
        extern qboolean cl_bullet_time_active;
        if (cl_bullet_time_active && client->bullet_time_end <= level.time &&
            client->bullet_time_charge >= 50.0f) {
            /* Activate: 4 seconds of slow-mo */
            client->bullet_time_end = level.time + 4.0f;
            client->bullet_time_charge -= 50.0f;
        }

        if (client->bullet_time_end > level.time) {
            level.time_scale = 0.3f;
            /* Golden screen tint during bullet time */
            if (client->blend[3] < 0.1f) {
                client->blend[0] = 0.8f;
                client->blend[1] = 0.7f;
                client->blend[2] = 0.3f;
                client->blend[3] = 0.1f;
            }
        } else {
            /* Don't reset time_scale if headshot cam is still active */
            if (level.time_scale < 1.0f && client->headshot_cam_end <= level.time)
                level.time_scale = 1.0f;
        }
    }

    /* Headshot camera slow-mo: end after 0.5s */
    if (client->headshot_cam_end > 0 && client->headshot_cam_end <= level.time) {
        if (client->bullet_time_end <= level.time)
            level.time_scale = 1.0f;
        client->headshot_cam_end = 0;
    }

    /* Challenge mode timeout */
    if (client->challenge_end > 0 && level.time >= client->challenge_end) {
        if (client->challenge_kills < client->challenge_target) {
            SCR_AddPickupMessage("CHALLENGE FAILED!");
            gi.cprintf(ent, PRINT_ALL, "Challenge failed: %d/%d kills\n",
                       client->challenge_kills, client->challenge_target);
        }
        client->challenge_end = 0;
    }

    /* Shield power-up visual feedback */
    if (!ent->deadflag && client->shield_end > level.time) {
        /* Blue glow tint */
        client->blend[0] = 0.2f;
        client->blend[1] = 0.3f;
        client->blend[2] = 0.8f;
        client->blend[3] = 0.1f;
    }

    /* Last stand: survive one lethal hit per life (must be above 50 HP) */
    if (!ent->deadflag && ent->health <= 0 && !client->last_stand_used) {
        ent->health = 1;
        client->pers_health = 1;
        client->last_stand_used = qtrue;
        SCR_AddPickupMessage("LAST STAND!");
        /* Red flash */
        client->blend[0] = 1.0f;
        client->blend[1] = 0.0f;
        client->blend[2] = 0.0f;
        client->blend[3] = 0.5f;
    }

    /* Reset last stand on respawn */
    if (ent->health >= 50)
        client->last_stand_used = qfalse;

    /* Adrenaline rush screen effect — subtle red edge tint */
    if (client->adrenaline_end > level.time) {
        if (client->blend[3] < 0.08f) {
            client->blend[0] = 1.0f;
            client->blend[1] = 0.3f;
            client->blend[2] = 0.0f;
            client->blend[3] = 0.08f;
        }
    }

    gi.linkentity(ent);
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

/* ==========================================================================
   Save/Load System

   WriteGame: Saves persistent game state (client inventory, game settings).
   ReadGame:  Restores game state from a save file.
   WriteLevel: Saves all entity state for current level.
   ReadLevel:  Restores entity state for current level.

   File format: Simple binary with magic + version header.
   ========================================================================== */

#define SAVE_MAGIC      0x534F4653  /* "SOFS" */

/* Saved entity data — only fields that matter for restoration */
typedef struct {
    int         index;
    qboolean    inuse;
    vec3_t      origin;
    vec3_t      angles;
    vec3_t      velocity;
    int         health;
    int         max_health;
    int         solid;
    int         movetype;
    int         takedamage;
    int         deadflag;
    int         flags;
    int         count;          /* AI state for monsters */
    float       nextthink;
    int         ai_flags;
    int         dmg;
    int         style;
    float       speed;
    float       wait;
    char        classname[64];
    char        target[64];
    char        targetname[64];
} save_entity_t;

/* Saved client/game state */
#define SAVE_VERSION    4   /* bumped for entity target/dmg/speed/wait fields */
typedef struct {
    int         health;
    int         max_health;
    int         weapon;
    vec3_t      origin;
    vec3_t      angles;
    float       viewheight;
    float       game_time;
    int         num_entities;
    int         armor;
    int         armor_max;
    int         ammo[WEAP_COUNT];
    int         ammo_max[WEAP_COUNT];
    int         magazine[WEAP_COUNT];
} save_game_t;

void WriteGame(const char *filename, qboolean autosave)
{
    FILE *f;
    save_game_t sg;
    edict_t *player;
    int magic = SAVE_MAGIC, version = SAVE_VERSION;

    (void)autosave;

    f = fopen(filename, "wb");
    if (!f) {
        gi.dprintf("WriteGame: can't open %s\n", filename);
        return;
    }

    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);

    player = &globals.edicts[1];
    memset(&sg, 0, sizeof(sg));

    if (player->inuse && player->client) {
        sg.health = player->health;
        sg.max_health = player->max_health;
        sg.weapon = player->client->pers_weapon;
        VectorCopy(player->s.origin, sg.origin);
        VectorCopy(player->client->viewangles, sg.angles);
        sg.viewheight = player->client->viewheight;
        sg.armor = player->client->armor;
        sg.armor_max = player->client->armor_max;
        memcpy(sg.ammo, player->client->ammo, sizeof(sg.ammo));
        memcpy(sg.ammo_max, player->client->ammo_max, sizeof(sg.ammo_max));
        memcpy(sg.magazine, player->client->magazine, sizeof(sg.magazine));
    }
    sg.game_time = level.time;
    sg.num_entities = globals.num_edicts;

    fwrite(&sg, sizeof(sg), 1, f);
    fclose(f);

    gi.dprintf("Game saved: %s\n", filename);
}

static void ReadGame(const char *filename)
{
    FILE *f;
    save_game_t sg;
    edict_t *player;
    int magic, version;

    f = fopen(filename, "rb");
    if (!f) {
        gi.dprintf("ReadGame: can't open %s\n", filename);
        return;
    }

    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);

    if (magic != SAVE_MAGIC || version != SAVE_VERSION) {
        gi.dprintf("ReadGame: bad save file %s\n", filename);
        fclose(f);
        return;
    }

    fread(&sg, sizeof(sg), 1, f);
    fclose(f);

    player = &globals.edicts[1];
    if (player->inuse && player->client) {
        player->health = sg.health;
        player->max_health = sg.max_health;
        player->client->pers_weapon = sg.weapon;
        player->weapon_index = sg.weapon;
        VectorCopy(sg.origin, player->s.origin);
        VectorCopy(sg.angles, player->client->viewangles);
        player->client->viewheight = sg.viewheight;
        player->client->armor = sg.armor;
        player->client->armor_max = sg.armor_max;
        memcpy(player->client->ammo, sg.ammo, sizeof(sg.ammo));
        memcpy(player->client->ammo_max, sg.ammo_max, sizeof(sg.ammo_max));
        memcpy(player->client->magazine, sg.magazine, sizeof(sg.magazine));
        player->deadflag = 0;
        player->client->ps.pm_type = PM_NORMAL;
    }

    gi.dprintf("Game loaded: %s\n", filename);
}

void WriteLevel(const char *filename)
{
    FILE *f;
    int magic = SAVE_MAGIC, version = SAVE_VERSION;
    int i;

    f = fopen(filename, "wb");
    if (!f) {
        gi.dprintf("WriteLevel: can't open %s\n", filename);
        return;
    }

    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&globals.num_edicts, 4, 1, f);

    for (i = 0; i < globals.num_edicts; i++) {
        edict_t *ent = &globals.edicts[i];
        save_entity_t se;

        memset(&se, 0, sizeof(se));
        se.index = i;
        se.inuse = ent->inuse;

        if (ent->inuse) {
            VectorCopy(ent->s.origin, se.origin);
            VectorCopy(ent->s.angles, se.angles);
            VectorCopy(ent->velocity, se.velocity);
            se.health = ent->health;
            se.max_health = ent->max_health;
            se.solid = ent->solid;
            se.movetype = ent->movetype;
            se.takedamage = ent->takedamage;
            se.deadflag = ent->deadflag;
            se.flags = ent->flags;
            se.count = ent->count;
            se.nextthink = ent->nextthink;
            se.ai_flags = ent->ai_flags;
            se.dmg = ent->dmg;
            se.style = ent->style;
            se.speed = ent->speed;
            se.wait = ent->wait;
            if (ent->classname)
                Q_strncpyz(se.classname, ent->classname, sizeof(se.classname));
            if (ent->target)
                Q_strncpyz(se.target, ent->target, sizeof(se.target));
            if (ent->targetname)
                Q_strncpyz(se.targetname, ent->targetname, sizeof(se.targetname));
        }

        fwrite(&se, sizeof(se), 1, f);
    }

    fclose(f);
    gi.dprintf("Level saved: %s (%d entities)\n", filename, globals.num_edicts);
}

static void ReadLevel(const char *filename)
{
    FILE *f;
    int magic, version, num_ents, i;

    f = fopen(filename, "rb");
    if (!f) {
        gi.dprintf("ReadLevel: can't open %s\n", filename);
        return;
    }

    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);

    if (magic != SAVE_MAGIC || version != SAVE_VERSION) {
        gi.dprintf("ReadLevel: bad save file %s\n", filename);
        fclose(f);
        return;
    }

    fread(&num_ents, 4, 1, f);

    for (i = 0; i < num_ents && i < globals.max_edicts; i++) {
        edict_t *ent = &globals.edicts[i];
        save_entity_t se;

        fread(&se, sizeof(se), 1, f);

        /* Skip player entity — restored by ReadGame */
        if (i == 0 || i == 1)
            continue;

        if (!se.inuse) {
            if (ent->inuse) {
                ent->inuse = qfalse;
                gi.unlinkentity(ent);
            }
            continue;
        }

        /* Restore saved state over existing entity */
        VectorCopy(se.origin, ent->s.origin);
        VectorCopy(se.angles, ent->s.angles);
        VectorCopy(se.velocity, ent->velocity);
        ent->health = se.health;
        ent->max_health = se.max_health;
        ent->solid = se.solid;
        ent->movetype = se.movetype;
        ent->takedamage = se.takedamage;
        ent->deadflag = se.deadflag;
        ent->flags = se.flags;
        ent->count = se.count;
        ent->nextthink = se.nextthink;
        ent->ai_flags = se.ai_flags;
        ent->dmg = se.dmg;
        ent->style = se.style;
        ent->speed = se.speed;
        ent->wait = se.wait;

        /* Restore think/callback functions based on classname */
        if (se.classname[0]) {
            if (strncmp(se.classname, "monster_", 8) == 0) {
                /* Monster entities: restore AI callbacks */
                extern void monster_think(edict_t *self);
                extern void monster_pain(edict_t *self, edict_t *other,
                                         float kick, int damage);
                extern void monster_die(edict_t *self, edict_t *inflictor,
                                        edict_t *attacker, int damage,
                                        vec3_t point);
                ent->think = monster_think;
                ent->pain = monster_pain;
                ent->die = monster_die;
                if (se.deadflag) {
                    /* Dead monster: schedule corpse removal */
                    ent->think = NULL;
                    ent->takedamage = DAMAGE_NO;
                    ent->solid = SOLID_NOT;
                } else if (se.nextthink <= level.time) {
                    ent->nextthink = level.time + 0.1f;
                }
            } else if (strcmp(se.classname, "func_door") == 0 ||
                       strcmp(se.classname, "func_plat") == 0) {
                /* Doors/plats keep their BSP-assigned callbacks */
            } else if (strncmp(se.classname, "item_", 5) == 0 ||
                       strncmp(se.classname, "weapon_", 7) == 0 ||
                       strncmp(se.classname, "ammo_", 5) == 0) {
                /* Pickup items: keep existing think from spawn */
            }
        }

        /* Re-link entity with updated position */
        if (ent->inuse)
            gi.linkentity(ent);
    }

    globals.num_edicts = num_ents > globals.num_edicts ? num_ents : globals.num_edicts;
    fclose(f);
    gi.dprintf("Level loaded: %s (%d entities)\n", filename, num_ents);
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

    /* Weapon fire sound paths — match original SoF data layout */
    static const char *weapon_fire_sounds[WEAP_COUNT] = {
        NULL,                           /* WEAP_NONE */
        "weapons/knife/swing.wav",      /* WEAP_KNIFE */
        "weapons/pistol/fire.wav",      /* WEAP_PISTOL1 */
        "weapons/silvtln/fire.wav",     /* WEAP_PISTOL2 */
        "weapons/shotgun/fire.wav",     /* WEAP_SHOTGUN */
        "weapons/mp5/fire.wav",         /* WEAP_MACHINEGUN */
        "weapons/m4/fire.wav",          /* WEAP_ASSAULT */
        "weapons/sniper/fire.wav",      /* WEAP_SNIPER */
        "weapons/slugger/fire.wav",     /* WEAP_SLUGGER */
        "weapons/rocket/fire.wav",      /* WEAP_ROCKET */
        "weapons/flame/fire.wav",       /* WEAP_FLAMEGUN */
        "weapons/mpg/fire.wav",         /* WEAP_MPG */
        "weapons/mpistol/fire.wav",     /* WEAP_MPISTOL */
        "weapons/grenade/throw.wav",    /* WEAP_GRENADE */
        "weapons/c4/place.wav",         /* WEAP_C4 */
        "items/medkit/use.wav",         /* WEAP_MEDKIT */
        NULL,                           /* WEAP_GOGGLES */
        NULL                            /* WEAP_FPAK */
    };

    gi.dprintf("RegisterWeapons:\n");
    for (i = 1; i < WEAP_COUNT; i++) {
        gi.dprintf("  %s\n", weapon_names[i]);
        if (weapon_fire_sounds[i])
            snd_weapons[i] = gi.soundindex(weapon_fire_sounds[i]);
    }

    /* Impact/environment sounds */
    snd_ric1 = gi.soundindex("world/ric1.wav");
    snd_ric2 = gi.soundindex("world/ric2.wav");
    snd_ric3 = gi.soundindex("world/ric3.wav");
    snd_hit_flesh = gi.soundindex("player/hit_flesh.wav");
    snd_explode = gi.soundindex("weapons/explode.wav");
    snd_noammo = gi.soundindex("weapons/noammo.wav");
    snd_weapon_switch = gi.soundindex("weapons/change.wav");
    snd_footstep1 = gi.soundindex("player/step1.wav");
    snd_footstep2 = gi.soundindex("player/step2.wav");
    snd_footstep3 = gi.soundindex("player/step3.wav");
    snd_footstep4 = gi.soundindex("player/step4.wav");

    /* Player pain/death sounds */
    snd_player_pain1 = gi.soundindex("player/pain25_1.wav");
    snd_player_pain2 = gi.soundindex("player/pain50_1.wav");
    snd_player_die = gi.soundindex("player/death1.wav");
    snd_drown = gi.soundindex("player/drown1.wav");
    snd_heartbeat = gi.soundindex("player/heartbeat.wav");

    /* Reload sound */
    snd_reload = gi.soundindex("weapons/reload.wav");

    /* Water splash sounds */
    snd_splash_in = gi.soundindex("player/watr_in.wav");
    snd_splash_out = gi.soundindex("player/watr_out.wav");

    /* Ambient environment sounds */
    snd_ambient_wind = gi.soundindex("world/wind1.wav");
    snd_ambient_drip = gi.soundindex("world/drip1.wav");

    /* Ladder climbing sound */
    snd_ladder_step = gi.soundindex("player/ladder1.wav");
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
