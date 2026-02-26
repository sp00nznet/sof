/*
 * g_spawn.c - Entity string parser and spawn dispatch
 *
 * Parses BSP entity strings (key/value pairs in braces) and spawns
 * game entities by matching classname to spawn function table.
 *
 * Entity string format (from BSP LUMP_ENTITIES):
 *   {
 *   "classname" "worldspawn"
 *   "message" "Map Title"
 *   "wad" "base.wad"
 *   }
 *   {
 *   "classname" "info_player_start"
 *   "origin" "0 64 24"
 *   "angle" "90"
 *   }
 *
 * Original SpawnEntities at 0x500A59A0 in gamex86.dll.
 *
 * SoF-specific entity types include:
 *   - info_player_start, info_player_deathmatch
 *   - weapon_*, item_*, ammo_*
 *   - monster_*, target_*, trigger_*
 *   - func_door, func_plat, func_rotating
 *   - light, misc_model
 *   - SoF additions: info_ctf_*, misc_ghoul_*
 */

#include "g_local.h"

/* Particle/light effects from renderer (unified binary) */
extern void R_ParticleEffect(vec3_t org, vec3_t dir, int type, int count);
extern void R_AddDlight(vec3_t origin, float r, float g, float b,
                         float intensity, float duration);

/* Monster spawn functions (g_ai.c) — use void* for epair_t since they don't parse it */
extern void SP_monster_soldier(edict_t *ent, void *pairs, int num_pairs);
extern void SP_monster_soldier_light(edict_t *ent, void *pairs, int num_pairs);
extern void SP_monster_soldier_ss(edict_t *ent, void *pairs, int num_pairs);
extern void SP_monster_guard(edict_t *ent, void *pairs, int num_pairs);
extern void SP_monster_boss(edict_t *ent, void *pairs, int num_pairs);

/* Maximum key/value pairs per entity */
#define MAX_ENTITY_FIELDS   64
#define MAX_FIELD_VALUE     256

/* ==========================================================================
   Entity Field Types
   ========================================================================== */

typedef enum {
    F_INT,
    F_FLOAT,
    F_STRING,
    F_VECTOR,
    F_ANGLEHACK    /* angle → angles conversion */
} fieldtype_t;

typedef struct {
    char    key[64];
    char    value[MAX_FIELD_VALUE];
} epair_t;

/* ==========================================================================
   Entity String Parser
   ========================================================================== */

/*
 * Skip whitespace in entity string
 */
static const char *ED_SkipWhitespace(const char *data)
{
    while (*data && *data <= ' ')
        data++;
    return data;
}

/*
 * Parse a quoted string from entity data
 * Returns pointer past the closing quote, fills out with the content
 */
static const char *ED_ParseQuotedString(const char *data, char *out, int outsize)
{
    int len = 0;

    if (*data != '"')
        return data;
    data++;

    while (*data && *data != '"' && len < outsize - 1) {
        out[len++] = *data++;
    }
    out[len] = '\0';

    if (*data == '"')
        data++;

    return data;
}

/*
 * Parse a single entity block from the entity string
 * Returns pointer past the closing brace, fills pairs[] and *num_pairs
 */
static const char *ED_ParseEntity(const char *data, epair_t *pairs, int *num_pairs)
{
    *num_pairs = 0;

    data = ED_SkipWhitespace(data);
    if (*data != '{')
        return NULL;
    data++;

    while (1) {
        data = ED_SkipWhitespace(data);

        if (*data == '}') {
            data++;
            return data;
        }

        if (*data == '\0')
            return NULL;

        if (*num_pairs >= MAX_ENTITY_FIELDS)
            return NULL;

        /* Parse key */
        data = ED_ParseQuotedString(data, pairs[*num_pairs].key,
                                     sizeof(pairs[*num_pairs].key));
        data = ED_SkipWhitespace(data);

        /* Parse value */
        data = ED_ParseQuotedString(data, pairs[*num_pairs].value,
                                     sizeof(pairs[*num_pairs].value));

        (*num_pairs)++;
    }
}

/*
 * Find a key's value in a parsed entity
 */
static const char *ED_FindValue(epair_t *pairs, int num_pairs, const char *key)
{
    int i;
    for (i = 0; i < num_pairs; i++) {
        if (Q_stricmp(pairs[i].key, key) == 0)
            return pairs[i].value;
    }
    return NULL;
}

/*
 * Parse a vector from a string "x y z"
 */
static void ED_ParseVector(const char *str, vec3_t v)
{
    VectorClear(v);
    if (str) {
        sscanf(str, "%f %f %f", &v[0], &v[1], &v[2]);
    }
}

/* ==========================================================================
   Spawn Functions
   ========================================================================== */

/* Forward declarations */
static void SP_worldspawn(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_info_player_start(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_info_player_deathmatch(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_light(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_door(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_plat(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_once(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_multiple(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_target_speaker(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_model(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_item_pickup(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_changelevel(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_rotating(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_button(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_target_changelevel(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_timer(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_breakable(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_explosive(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_push(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_hurt(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_teleport(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_teleport_dest(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_target_relay(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_wall(edict_t *ent, epair_t *pairs, int num_pairs);

/*
 * Spawn function dispatch table
 */
typedef struct {
    const char  *classname;
    void        (*spawn)(edict_t *ent, epair_t *pairs, int num_pairs);
} spawn_func_t;

static spawn_func_t spawn_funcs[] = {
    /* World */
    { "worldspawn",                 SP_worldspawn },

    /* Player spawns */
    { "info_player_start",          SP_info_player_start },
    { "info_player_deathmatch",     SP_info_player_deathmatch },
    { "info_player_coop",           SP_info_player_start },
    { "info_player_intermission",   SP_info_player_start },

    /* SoF CTF spawns */
    { "info_ctf_team1",             SP_info_player_deathmatch },
    { "info_ctf_team2",             SP_info_player_deathmatch },

    /* Lights */
    { "light",                      SP_light },
    { "light_mine1",                SP_light },
    { "light_mine2",                SP_light },

    /* Brush entities */
    { "func_door",                  SP_func_door },
    { "func_door_rotating",         SP_func_door },
    { "func_plat",                  SP_func_plat },
    { "func_rotating",              SP_func_rotating },
    { "func_button",                SP_func_button },
    { "func_wall",                  SP_func_wall },
    { "func_timer",                 SP_func_timer },
    { "func_train",                 SP_func_plat },
    { "func_conveyor",              SP_misc_model },
    { "func_group",                 SP_misc_model },

    /* Triggers */
    { "trigger_once",               SP_trigger_once },
    { "trigger_multiple",           SP_trigger_multiple },
    { "trigger_relay",              SP_target_relay },
    { "trigger_always",             SP_trigger_once },
    { "trigger_changelevel",        SP_trigger_changelevel },
    { "trigger_push",               SP_trigger_push },
    { "trigger_hurt",               SP_trigger_hurt },
    { "trigger_gravity",            SP_trigger_multiple },
    { "trigger_teleport",           SP_trigger_teleport },
    { "misc_teleport_dest",         SP_misc_teleport_dest },
    { "info_teleport_destination",  SP_misc_teleport_dest },

    /* Targets */
    { "target_speaker",             SP_target_speaker },
    { "target_changelevel",         SP_target_changelevel },
    { "target_relay",               SP_target_relay },
    { "target_explosion",           SP_target_speaker },
    { "target_temp_entity",         SP_target_speaker },

    /* Breakable/explosive */
    { "func_breakable",             SP_func_breakable },
    { "func_explosive",             SP_func_explosive },
    { "misc_explobox",              SP_func_explosive },

    /* Monsters (g_ai.c) */
    { "monster_soldier",            (void (*)(edict_t *, epair_t *, int))SP_monster_soldier },
    { "monster_soldier_light",      (void (*)(edict_t *, epair_t *, int))SP_monster_soldier_light },
    { "monster_soldier_ss",         (void (*)(edict_t *, epair_t *, int))SP_monster_soldier_ss },
    { "monster_guard",              (void (*)(edict_t *, epair_t *, int))SP_monster_guard },
    { "monster_boss",               (void (*)(edict_t *, epair_t *, int))SP_monster_boss },
    /* Q2-compatible monster names */
    { "monster_soldier_light",      (void (*)(edict_t *, epair_t *, int))SP_monster_soldier_light },
    { "monster_infantry",           (void (*)(edict_t *, epair_t *, int))SP_monster_soldier },
    { "monster_gunner",             (void (*)(edict_t *, epair_t *, int))SP_monster_soldier_ss },
    { "monster_enforcer",           (void (*)(edict_t *, epair_t *, int))SP_monster_soldier },

    /* Misc */
    { "misc_model",                 SP_misc_model },

    /* Weapons (SoF) */
    { "weapon_knife",               SP_item_pickup },
    { "weapon_pistol1",             SP_item_pickup },
    { "weapon_pistol2",             SP_item_pickup },
    { "weapon_shotgun",             SP_item_pickup },
    { "weapon_machinegun",          SP_item_pickup },
    { "weapon_assault",             SP_item_pickup },
    { "weapon_sniper",              SP_item_pickup },
    { "weapon_slugger",             SP_item_pickup },
    { "weapon_rocket",              SP_item_pickup },
    { "weapon_flamegun",            SP_item_pickup },
    { "weapon_mpg",                 SP_item_pickup },
    { "weapon_grenade",             SP_item_pickup },

    /* Ammo */
    { "ammo_pistol",                SP_item_pickup },
    { "ammo_shotgun",               SP_item_pickup },
    { "ammo_machinegun",            SP_item_pickup },
    { "ammo_assault",               SP_item_pickup },
    { "ammo_sniper",                SP_item_pickup },
    { "ammo_slugger",               SP_item_pickup },
    { "ammo_rockets",               SP_item_pickup },
    { "ammo_fuel",                  SP_item_pickup },
    { "ammo_cells",                 SP_item_pickup },
    { "ammo_grenades",              SP_item_pickup },

    /* Items */
    { "item_health",                SP_item_pickup },
    { "item_health_small",          SP_item_pickup },
    { "item_health_large",          SP_item_pickup },
    { "item_armor_body",            SP_item_pickup },
    { "item_armor_combat",          SP_item_pickup },
    { "item_armor_jacket",          SP_item_pickup },

    /* Sentinel */
    { NULL, NULL }
};

/* ==========================================================================
   Entity Allocation
   ========================================================================== */

extern game_export_t globals;

edict_t *G_AllocEdict(void)
{
    int i;
    edict_t *e;
    int maxclients;

    /* Get maxclients from cvar */
    {
        cvar_t *mc = gi.cvar("maxclients", "8", 0);
        maxclients = (int)mc->value;
    }

    /* Find first free edict after clients */
    for (i = maxclients + 1; i < globals.max_edicts; i++) {
        e = &globals.edicts[i];
        if (!e->inuse) {
            memset(e, 0, sizeof(*e));
            e->s.number = i;
            e->inuse = qtrue;

            if (i >= globals.num_edicts)
                globals.num_edicts = i + 1;

            return e;
        }
    }

    gi.error("G_AllocEdict: no free edicts (max=%d)", globals.max_edicts);
    return NULL;
}

/* ==========================================================================
   Common Entity Field Setter
   ========================================================================== */

static void ED_SetCommonFields(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *val;

    /* Origin */
    val = ED_FindValue(pairs, num_pairs, "origin");
    if (val) {
        ED_ParseVector(val, ent->s.origin);
    }

    /* Angles */
    val = ED_FindValue(pairs, num_pairs, "angles");
    if (val) {
        ED_ParseVector(val, ent->s.angles);
    }

    /* Single angle → yaw only */
    val = ED_FindValue(pairs, num_pairs, "angle");
    if (val) {
        VectorClear(ent->s.angles);
        ent->s.angles[1] = (float)atof(val);
    }

    /* Classname */
    val = ED_FindValue(pairs, num_pairs, "classname");
    if (val) {
        /* Store classname — allocated from game tag memory */
        int len = (int)strlen(val) + 1;
        ent->classname = (char *)gi.TagMalloc(len, Z_TAG_GAME);
        memcpy(ent->classname, val, len);
    }

    /* Target */
    val = ED_FindValue(pairs, num_pairs, "target");
    if (val) {
        int len = (int)strlen(val) + 1;
        ent->target = (char *)gi.TagMalloc(len, Z_TAG_GAME);
        memcpy(ent->target, val, len);
    }

    /* Targetname */
    val = ED_FindValue(pairs, num_pairs, "targetname");
    if (val) {
        int len = (int)strlen(val) + 1;
        ent->targetname = (char *)gi.TagMalloc(len, Z_TAG_GAME);
        memcpy(ent->targetname, val, len);
    }

    /* Model (for brush entities like func_door) */
    val = ED_FindValue(pairs, num_pairs, "model");
    if (val) {
        int len = (int)strlen(val) + 1;
        ent->model = (char *)gi.TagMalloc(len, Z_TAG_GAME);
        memcpy(ent->model, val, len);
    }

    /* Health */
    val = ED_FindValue(pairs, num_pairs, "health");
    if (val) {
        ent->health = atoi(val);
    }

    /* Speed */
    val = ED_FindValue(pairs, num_pairs, "speed");
    if (val) {
        ent->speed = (float)atof(val);
    }

    /* Wait */
    val = ED_FindValue(pairs, num_pairs, "wait");
    if (val) {
        ent->wait = (float)atof(val);
    }

    /* Delay */
    val = ED_FindValue(pairs, num_pairs, "delay");
    if (val) {
        ent->delay = (float)atof(val);
    }

    /* Message */
    val = ED_FindValue(pairs, num_pairs, "message");
    if (val) {
        int len = (int)strlen(val) + 1;
        ent->message = (char *)gi.TagMalloc(len, Z_TAG_GAME);
        memcpy(ent->message, val, len);
    }

    /* Spawnflags */
    val = ED_FindValue(pairs, num_pairs, "spawnflags");
    if (val) {
        ent->flags = atoi(val);
    }
}

/* ==========================================================================
   Spawn Function Implementations
   ========================================================================== */

static void SP_worldspawn(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *val;

    (void)ent;

    val = ED_FindValue(pairs, num_pairs, "message");
    if (val) {
        gi.dprintf("Map: %s\n", val);
    }

    val = ED_FindValue(pairs, num_pairs, "sky");
    if (val) {
        gi.dprintf("Sky: %s\n", val);
        gi.configstring(CS_SKY, val);
    }

    val = ED_FindValue(pairs, num_pairs, "skyrotate");
    if (val) {
        gi.configstring(CS_SKYROTATE, val);
    }

    val = ED_FindValue(pairs, num_pairs, "skyaxis");
    if (val) {
        gi.configstring(CS_SKYAXIS, val);
    }
}

static void SP_info_player_start(edict_t *ent, epair_t *pairs, int num_pairs)
{
    edict_t *player;

    (void)pairs; (void)num_pairs;

    gi.dprintf("  Player start at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);

    /* Place player entity (edict[1]) at this spawn point */
    player = &globals.edicts[1];
    if (player->inuse && player->client) {
        VectorCopy(ent->s.origin, player->s.origin);
        VectorCopy(ent->s.angles, player->s.angles);
        player->client->ps.origin[0] = ent->s.origin[0];
        player->client->ps.origin[1] = ent->s.origin[1];
        player->client->ps.origin[2] = ent->s.origin[2];
        player->client->ps.pm_type = PM_NORMAL;
        player->health = player->client->pers_health;
        player->max_health = player->client->pers_max_health;
        player->solid = SOLID_BBOX;
        player->clipmask = MASK_PLAYERSOLID;
        player->movetype = MOVETYPE_WALK;
        player->takedamage = DAMAGE_AIM;
        player->client->pers_weapon = WEAP_PISTOL1;
        player->weapon_index = WEAP_PISTOL1;

        /* Initialize ammo */
        {
            /* Max ammo per weapon */
            static const int default_max_ammo[WEAP_COUNT] = {
                0,    /* NONE */
                0,    /* KNIFE (melee) */
                24,   /* PISTOL1 */
                30,   /* PISTOL2 */
                32,   /* SHOTGUN */
                150,  /* MACHINEGUN */
                120,  /* ASSAULT */
                25,   /* SNIPER */
                40,   /* SLUGGER */
                12,   /* ROCKET */
                200,  /* FLAMEGUN */
                100,  /* MPG */
                60,   /* MPISTOL */
                5,    /* GRENADE */
                3,    /* C4 */
                5,    /* MEDKIT */
                0,    /* GOGGLES */
                0,    /* FPAK */
            };
            int w;
            for (w = 0; w < WEAP_COUNT; w++) {
                player->client->ammo_max[w] = default_max_ammo[w];
                player->client->ammo[w] = 0;
            }
            /* Start with pistol ammo */
            player->client->ammo[WEAP_PISTOL1] = 12;
            player->client->ammo[WEAP_KNIFE] = 999; /* melee = infinite */
        }

        VectorSet(player->mins, -16, -16, -24);
        VectorSet(player->maxs, 16, 16, 32);
        gi.linkentity(player);
    }

    /* info_player_start is a marker, not a runtime entity */
    ent->inuse = qfalse;
}

static void SP_info_player_deathmatch(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    gi.dprintf("  DM spawn at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

static void SP_light(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    /* Lights are compiled into lightmaps, no runtime entity needed */
    ent->inuse = qfalse;
}

/* ==========================================================================
   Door / Platform Sound Indices
   ========================================================================== */

static int snd_door_start;
static int snd_door_end;
static int snd_plat_start;
static int snd_plat_end;
static qboolean door_sounds_cached;

static void door_precache_sounds(void)
{
    if (door_sounds_cached)
        return;
    snd_door_start = gi.soundindex("doors/dr1_strt.wav");
    snd_door_end = gi.soundindex("doors/dr1_end.wav");
    snd_plat_start = gi.soundindex("plats/pt1_strt.wav");
    snd_plat_end = gi.soundindex("plats/pt1_end.wav");
    door_sounds_cached = qtrue;
}

/* ==========================================================================
   Door / Platform Movement Helpers
   ========================================================================== */

static void door_go_up(edict_t *self);
static void door_go_down(edict_t *self);

static void Move_Done(edict_t *ent)
{
    VectorClear(ent->velocity);
    if (ent->moveinfo.endfunc)
        ent->moveinfo.endfunc(ent);
}

static void Move_Begin(edict_t *ent)
{
    float dist;

    /* Compute remaining distance */
    vec3_t delta;
    VectorSubtract(ent->moveinfo.end_origin, ent->s.origin, delta);
    dist = VectorLength(delta);

    if (dist < 0.1f) {
        Move_Done(ent);
        return;
    }

    /* Set velocity toward destination */
    VectorScale(delta, ent->moveinfo.speed / dist, ent->velocity);

    /* Schedule think to check arrival */
    ent->nextthink = level.time + (dist / ent->moveinfo.speed);
    ent->think = Move_Done;
}

static void Move_Calc(edict_t *ent, vec3_t dest, void (*endfunc)(edict_t *))
{
    VectorCopy(dest, ent->moveinfo.end_origin);
    ent->moveinfo.endfunc = endfunc;
    Move_Begin(ent);
}

/* Door reached open position — wait then close */
static void door_hit_top(edict_t *self)
{
    self->moveinfo.state = MSTATE_TOP;
    VectorClear(self->velocity);

    if (snd_door_end)
        gi.sound(self, CHAN_AUTO, snd_door_end, 1.0f, 1, 0);

    if (self->moveinfo.wait >= 0) {
        self->nextthink = level.time + self->moveinfo.wait;
        self->think = door_go_down;
    }
}

/* Door reached closed position */
static void door_hit_bottom(edict_t *self)
{
    self->moveinfo.state = MSTATE_BOTTOM;
    VectorClear(self->velocity);

    if (snd_door_end)
        gi.sound(self, CHAN_AUTO, snd_door_end, 1.0f, 1, 0);
}

static void door_go_up(edict_t *self)
{
    if (self->moveinfo.state == MSTATE_UP || self->moveinfo.state == MSTATE_TOP)
        return;

    self->moveinfo.state = MSTATE_UP;
    if (snd_door_start)
        gi.sound(self, CHAN_AUTO, snd_door_start, 1.0f, 1, 0);
    Move_Calc(self, self->moveinfo.end_origin, door_hit_top);
}

static void door_go_down(edict_t *self)
{
    if (self->moveinfo.state == MSTATE_DOWN || self->moveinfo.state == MSTATE_BOTTOM)
        return;

    self->moveinfo.state = MSTATE_DOWN;
    if (snd_door_start)
        gi.sound(self, CHAN_AUTO, snd_door_start, 1.0f, 1, 0);
    Move_Calc(self, self->moveinfo.start_origin, door_hit_bottom);
}

/* Use callback — toggles door open/close */
static void door_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;

    if (self->moveinfo.state == MSTATE_BOTTOM || self->moveinfo.state == MSTATE_DOWN)
        door_go_up(self);
    else
        door_go_down(self);
}

/* Touch callback for doors without targetname (auto-open) */
static void door_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client)
        return;

    if (self->moveinfo.state == MSTATE_BOTTOM)
        door_go_up(self);
}

static void SP_func_door(edict_t *ent, epair_t *pairs, int num_pairs)
{
    vec3_t move_dir;
    float lip;
    const char *lip_str;

    door_precache_sounds();

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;

    if (!ent->speed)
        ent->speed = 100;

    /* Default: door moves up by its height minus lip */
    lip_str = ED_FindValue(pairs, num_pairs, "lip");
    lip = lip_str ? (float)atof(lip_str) : 8.0f;

    VectorSet(move_dir, 0, 0, 1);  /* default: up */

    /* Check for angle-based direction */
    if (ent->s.angles[1] != 0) {
        float angle = ent->s.angles[1] * (3.14159265f / 180.0f);
        move_dir[0] = (float)cos(angle);
        move_dir[1] = (float)sin(angle);
        move_dir[2] = 0;
        VectorClear(ent->s.angles);
    }

    /* Calculate movement distance from entity size */
    {
        float dist = fabs(DotProduct(ent->size, move_dir)) - lip;
        if (dist < 0) dist = 0;

        VectorCopy(ent->s.origin, ent->moveinfo.start_origin);
        VectorMA(ent->s.origin, dist, move_dir, ent->moveinfo.end_origin);
    }

    ent->moveinfo.speed = ent->speed;
    ent->moveinfo.wait = ent->wait ? ent->wait : 3.0f;
    ent->moveinfo.state = MSTATE_BOTTOM;

    /* If no targetname, door opens on touch */
    if (!ent->targetname) {
        ent->touch = door_touch;
    }
    ent->use = door_use;

    gi.linkentity(ent);
    gi.dprintf("  func_door '%s'\n", ent->targetname ? ent->targetname : "(auto)");
}

/* Platform: starts at top, descends when touched, returns */
static void plat_go_down(edict_t *self);
static void plat_go_up(edict_t *self);

static void plat_hit_top(edict_t *self)
{
    self->moveinfo.state = MSTATE_TOP;
    VectorClear(self->velocity);
    if (snd_plat_end)
        gi.sound(self, CHAN_AUTO, snd_plat_end, 1.0f, 1, 0);
}

static void plat_hit_bottom(edict_t *self)
{
    self->moveinfo.state = MSTATE_BOTTOM;
    VectorClear(self->velocity);
    if (snd_plat_end)
        gi.sound(self, CHAN_AUTO, snd_plat_end, 1.0f, 1, 0);
    /* Wait then return up */
    self->nextthink = level.time + 1.0f;
    self->think = plat_go_up;
}

static void plat_go_down(edict_t *self)
{
    if (self->moveinfo.state == MSTATE_DOWN || self->moveinfo.state == MSTATE_BOTTOM)
        return;
    self->moveinfo.state = MSTATE_DOWN;
    if (snd_plat_start)
        gi.sound(self, CHAN_AUTO, snd_plat_start, 1.0f, 1, 0);
    Move_Calc(self, self->moveinfo.end_origin, plat_hit_bottom);
}

static void plat_go_up(edict_t *self)
{
    if (self->moveinfo.state == MSTATE_UP || self->moveinfo.state == MSTATE_TOP)
        return;
    self->moveinfo.state = MSTATE_UP;
    if (snd_plat_start)
        gi.sound(self, CHAN_AUTO, snd_plat_start, 1.0f, 1, 0);
    Move_Calc(self, self->moveinfo.start_origin, plat_hit_top);
}

static void plat_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;
    if (!other || !other->client) return;
    if (self->moveinfo.state == MSTATE_TOP)
        plat_go_down(self);
}

static void SP_func_plat(edict_t *ent, epair_t *pairs, int num_pairs)
{
    float height;
    const char *height_str;

    (void)pairs; (void)num_pairs;
    door_precache_sounds();

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;

    if (!ent->speed)
        ent->speed = 200;

    /* Platforms descend by their height (or specified height) */
    height_str = ED_FindValue(pairs, num_pairs, "height");
    height = height_str ? (float)atof(height_str) : (ent->size[2] - 8);
    if (height < 0) height = 0;

    /* Start at top */
    VectorCopy(ent->s.origin, ent->moveinfo.start_origin);
    VectorCopy(ent->s.origin, ent->moveinfo.end_origin);
    ent->moveinfo.end_origin[2] -= height;

    ent->moveinfo.speed = ent->speed;
    ent->moveinfo.state = MSTATE_TOP;

    ent->touch = plat_touch;
    gi.linkentity(ent);
}

/* ==========================================================================
   Trigger System
   ========================================================================== */

/* Kill entities matching killtarget field */
static void G_KillTargets(const char *killtarget)
{
    int i;
    if (!killtarget || !killtarget[0]) return;

    for (i = 0; i < globals.max_edicts; i++) {
        edict_t *t = &globals.edicts[i];
        if (!t->inuse || !t->targetname)
            continue;
        if (Q_stricmp(t->targetname, killtarget) == 0) {
            t->inuse = qfalse;
            gi.unlinkentity(t);
        }
    }
}

/* Find entities by targetname and call their use() */
static void G_UseTargets(edict_t *activator, const char *target)
{
    int i;
    if (!target || !target[0]) return;

    for (i = 0; i < globals.max_edicts; i++) {
        edict_t *t = &globals.edicts[i];
        if (!t->inuse || !t->targetname)
            continue;
        if (Q_stricmp(t->targetname, target) == 0) {
            if (t->use)
                t->use(t, activator, activator);
        }
    }
}

/* Full target firing with killtarget + message (for entities with both fields) */
static void G_FireTargets(edict_t *self, edict_t *activator)
{
    if (self->killtarget)
        G_KillTargets(self->killtarget);
    if (self->target)
        G_UseTargets(activator, self->target);
    if (self->message)
        gi.centerprintf(activator, "%s", self->message);
}

static void trigger_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client)
        return;

    /* Debounce for trigger_multiple */
    if (self->dmg_debounce_time > level.time)
        return;

    self->dmg_debounce_time = level.time + self->wait;

    /* Fire targets and killtargets */
    self->activator = other;
    G_FireTargets(self, other);

    /* trigger_once removes itself after firing */
    if (!self->wait) {
        self->touch = NULL;
        self->nextthink = level.time + level.frametime;
        self->think = NULL;
        self->inuse = qfalse;
    }
}

static void SP_trigger_once(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = trigger_touch;
    ent->wait = 0;  /* fire once then remove */
    gi.linkentity(ent);
}

static void SP_trigger_multiple(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = trigger_touch;
    if (!ent->wait)
        ent->wait = 0.2f;
    gi.linkentity(ent);
}

/* ==========================================================================
   trigger_push — Applies velocity to entities that touch it
   ========================================================================== */

static void trigger_push_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->inuse)
        return;

    /* Apply push velocity from move_angles (set during spawn from "speed" + angle) */
    VectorCopy(self->move_angles, other->velocity);

    /* Clear groundentity so player goes airborne */
    other->groundentity = NULL;
}

static void SP_trigger_push(edict_t *ent, epair_t *pairs, int num_pairs)
{
    float push_speed;
    float angle;

    ent->solid = SOLID_TRIGGER;
    ent->touch = trigger_push_touch;

    /* Calculate push direction from angle + speed */
    push_speed = ent->speed ? ent->speed : 1000.0f;
    angle = ent->s.angles[1] * (3.14159265f / 180.0f);

    /* Default: push upward if no angle specified */
    if (ent->s.angles[1] == 0 && ent->s.angles[0] == 0) {
        VectorSet(ent->move_angles, 0, 0, push_speed);
    } else {
        float pitch = ent->s.angles[0] * (3.14159265f / 180.0f);
        ent->move_angles[0] = (float)cos(angle) * (float)cos(pitch) * push_speed;
        ent->move_angles[1] = (float)sin(angle) * (float)cos(pitch) * push_speed;
        ent->move_angles[2] = -(float)sin(pitch) * push_speed;
    }

    VectorClear(ent->s.angles);
    gi.linkentity(ent);
}

/* ==========================================================================
   trigger_hurt — Damages entities that touch it
   ========================================================================== */

static void trigger_hurt_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    int damage;

    (void)plane; (void)surf;

    if (!other || !other->inuse || other->health <= 0)
        return;

    if (!other->takedamage)
        return;

    /* Debounce — don't damage every frame */
    if (self->dmg_debounce_time > level.time)
        return;
    self->dmg_debounce_time = level.time + 1.0f;  /* 1 second between hits */

    damage = self->dmg ? self->dmg : 5;
    other->health -= damage;

    /* Damage flash for player */
    if (other->client) {
        other->client->blend[0] = 1.0f;
        other->client->blend[1] = 0.0f;
        other->client->blend[2] = 0.0f;
        other->client->blend[3] = 0.25f;
        other->client->pers_health = other->health;
    }

    if (other->health <= 0) {
        if (other->die)
            other->die(other, self, self, damage, other->s.origin);
        else if (other->client) {
            other->deadflag = 1;
            other->client->ps.pm_type = PM_DEAD;
        }
    }
}

static void SP_trigger_hurt(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *dmg_str;

    ent->solid = SOLID_TRIGGER;
    ent->touch = trigger_hurt_touch;

    /* Parse damage amount */
    dmg_str = ED_FindValue(pairs, num_pairs, "dmg");
    ent->dmg = dmg_str ? atoi(dmg_str) : 5;

    gi.linkentity(ent);
}

/* ==========================================================================
   trigger_teleport — Teleports touching entities to target destination
   ========================================================================== */

static edict_t *G_FindByTargetname(const char *targetname)
{
    extern game_export_t globals;
    int i;
    for (i = 0; i < globals.num_edicts; i++) {
        edict_t *e = &globals.edicts[i];
        if (e->inuse && e->targetname &&
            Q_stricmp(e->targetname, targetname) == 0)
            return e;
    }
    return NULL;
}

static void teleport_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    edict_t *dest;

    (void)plane; (void)surf;

    if (!other || !other->inuse)
        return;

    if (!self->target)
        return;

    dest = G_FindByTargetname(self->target);
    if (!dest) {
        gi.dprintf("trigger_teleport: can't find target %s\n", self->target);
        return;
    }

    /* Teleport: set position, clear velocity, set angles */
    VectorCopy(dest->s.origin, other->s.origin);
    other->s.origin[2] += 10;  /* slight lift to avoid stuck in floor */
    VectorClear(other->velocity);

    if (other->client) {
        VectorCopy(dest->s.angles, other->client->viewangles);
        other->client->ps.pm_type = PM_NORMAL;
    }
    VectorCopy(dest->s.angles, other->s.angles);

    gi.linkentity(other);
}

static void SP_trigger_teleport(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = teleport_touch;
    gi.linkentity(ent);
}

static void SP_misc_teleport_dest(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    /* Just a destination marker — needs targetname set by entity parser */
    ent->solid = SOLID_NOT;
    ent->movetype = MOVETYPE_NONE;
}

/* ==========================================================================
   Level Transition
   ========================================================================== */

static void changelevel_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client)
        return;

    /* Only trigger once */
    self->touch = NULL;

    if (self->message) {
        gi.bprintf(PRINT_ALL, "%s\n", self->message);
    }

    /* Queue the map change via console command (deferred to avoid re-entrant issues) */
    if (self->target) {
        gi.dprintf("trigger_changelevel: loading %s\n", self->target);
        gi.AddCommandString(va("map %s\n", self->target));
    }
}

static void SP_trigger_changelevel(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *map = ED_FindValue(pairs, num_pairs, "map");

    (void)pairs; (void)num_pairs;

    if (map) {
        /* Store map name in target field for the touch callback */
        ent->target = gi.TagMalloc((int)strlen(map) + 1, Z_TAG_GAME);
        strcpy(ent->target, map);
    }

    ent->solid = SOLID_TRIGGER;
    ent->touch = changelevel_touch;
    gi.linkentity(ent);

    gi.dprintf("  trigger_changelevel -> %s\n", ent->target ? ent->target : "???");
}

/* ==========================================================================
   func_rotating — Continuously rotating brush entity
   ========================================================================== */

static void SP_func_rotating(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *val;

    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BSP;
    ent->movetype = MOVETYPE_PUSH;

    /* Default rotation speed */
    if (!ent->speed)
        ent->speed = 100;

    /* Spawnflags: 1=X_AXIS, 2=Y_AXIS, default=Z_AXIS */
    if (!ent->avelocity[0] && !ent->avelocity[1] && !ent->avelocity[2]) {
        val = ED_FindValue(pairs, num_pairs, "spawnflags");
        if (val) {
            int sf = atoi(val);
            if (sf & 1)
                ent->avelocity[0] = ent->speed;    /* pitch */
            else if (sf & 2)
                ent->avelocity[2] = ent->speed;    /* roll */
            else
                ent->avelocity[1] = ent->speed;    /* yaw (default) */
        } else {
            ent->avelocity[1] = ent->speed;
        }
    }

    /* Damage on contact (optional) */
    if (!ent->dmg)
        ent->dmg = 2;

    gi.linkentity(ent);

    gi.dprintf("  func_rotating: speed=%.0f avel=(%.0f %.0f %.0f)\n",
               ent->speed, ent->avelocity[0], ent->avelocity[1], ent->avelocity[2]);
}

/* ==========================================================================
   func_button — Pressable button (like a door that returns)
   ========================================================================== */

static void button_return(edict_t *self);

static void button_done(edict_t *self)
{
    self->moveinfo.state = MSTATE_BOTTOM;
    self->s.angles[0] = self->moveinfo.start_angles[0];
    self->s.angles[1] = self->moveinfo.start_angles[1];
    self->s.angles[2] = self->moveinfo.start_angles[2];
}

static void button_return(edict_t *self)
{
    self->moveinfo.state = MSTATE_DOWN;
    Move_Calc(self, self->moveinfo.start_origin, button_done);
}

static void button_wait(edict_t *self)
{
    self->moveinfo.state = MSTATE_TOP;

    if (self->target)
        G_UseTargets(self->activator, self->target);

    if (self->wait >= 0) {
        self->nextthink = level.time + self->wait;
        self->think = button_return;
    }
}

static int snd_button;
static qboolean button_sounds_cached;

static void button_fire(edict_t *self, edict_t *activator)
{
    if (self->moveinfo.state == MSTATE_UP || self->moveinfo.state == MSTATE_TOP)
        return;

    self->moveinfo.state = MSTATE_UP;
    self->activator = activator;
    if (snd_button)
        gi.sound(self, CHAN_AUTO, snd_button, 1.0f, 1, 0);
    Move_Calc(self, self->moveinfo.end_origin, button_wait);
}

static void button_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;
    button_fire(self, activator);
}

static void button_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;
    if (!other || !other->client)
        return;
    button_fire(self, other);
}

static void SP_func_button(edict_t *ent, epair_t *pairs, int num_pairs)
{
    float dist;
    int angle_val;
    const char *angle_str;

    (void)pairs; (void)num_pairs;

    if (!button_sounds_cached) {
        snd_button = gi.soundindex("buttons/switch21.wav");
        button_sounds_cached = qtrue;
    }

    ent->solid = SOLID_BSP;
    ent->movetype = MOVETYPE_PUSH;

    if (!ent->speed)
        ent->speed = 40;
    if (!ent->wait)
        ent->wait = 1.0f;

    /* Calculate movement direction from "angle" key */
    angle_str = ED_FindValue(pairs, num_pairs, "angle");
    angle_val = angle_str ? atoi(angle_str) : 0;

    VectorCopy(ent->s.origin, ent->moveinfo.start_origin);
    VectorCopy(ent->s.angles, ent->moveinfo.start_angles);

    /* Calculate end position (default: move 4 units in angle direction) */
    dist = 4.0f;
    {
        const char *lip_str = ED_FindValue(pairs, num_pairs, "lip");
        if (lip_str) dist = (float)atof(lip_str);
    }

    if (angle_val == -1) {
        /* up */
        ent->moveinfo.end_origin[0] = ent->s.origin[0];
        ent->moveinfo.end_origin[1] = ent->s.origin[1];
        ent->moveinfo.end_origin[2] = ent->s.origin[2] + dist;
    } else if (angle_val == -2) {
        /* down */
        ent->moveinfo.end_origin[0] = ent->s.origin[0];
        ent->moveinfo.end_origin[1] = ent->s.origin[1];
        ent->moveinfo.end_origin[2] = ent->s.origin[2] - dist;
    } else {
        float rad = angle_val * (3.14159265f / 180.0f);
        ent->moveinfo.end_origin[0] = ent->s.origin[0] + (float)cos(rad) * dist;
        ent->moveinfo.end_origin[1] = ent->s.origin[1] + (float)sin(rad) * dist;
        ent->moveinfo.end_origin[2] = ent->s.origin[2];
    }

    VectorCopy(ent->s.angles, ent->moveinfo.end_angles);
    ent->moveinfo.speed = ent->speed;
    ent->moveinfo.state = MSTATE_BOTTOM;

    if (ent->targetname) {
        ent->use = button_use;
    } else {
        ent->touch = button_touch;
    }

    gi.linkentity(ent);
}

/* ==========================================================================
   target_changelevel — Use-triggered level transition
   ========================================================================== */

static void target_changelevel_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;

    if (self->message) {
        gi.bprintf(PRINT_ALL, "%s\n", self->message);
    }

    if (self->target) {
        gi.dprintf("target_changelevel: loading %s\n", self->target);
        gi.AddCommandString(va("map %s\n", self->target));
    }
}

static void SP_target_changelevel(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *map = ED_FindValue(pairs, num_pairs, "map");

    (void)pairs; (void)num_pairs;

    if (map) {
        ent->target = gi.TagMalloc((int)strlen(map) + 1, Z_TAG_GAME);
        strcpy(ent->target, map);
    }

    ent->use = target_changelevel_use;
}

/* ==========================================================================
   func_timer — Periodic trigger fire
   ========================================================================== */

static void func_timer_think(edict_t *self)
{
    if (self->target)
        G_UseTargets(self->activator, self->target);
    self->nextthink = level.time + self->wait + self->random * gi.flrand(-1.0f, 1.0f);
}

static void func_timer_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;
    self->activator = activator;

    /* Toggle */
    if (self->nextthink > 0) {
        self->nextthink = 0;
    } else {
        self->nextthink = level.time + self->wait;
        self->think = func_timer_think;
    }
}

static void SP_func_timer(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    if (!ent->wait)
        ent->wait = 1.0f;

    ent->use = func_timer_use;
    ent->think = func_timer_think;

    /* Start active by default (spawnflags & 1 = start off) */
    {
        const char *sf_str = ED_FindValue(pairs, num_pairs, "spawnflags");
        int sf = sf_str ? atoi(sf_str) : 0;
        if (!(sf & 1)) {
            ent->nextthink = level.time + 1.0f + ent->wait + ent->random * gi.flrand(-1.0f, 1.0f);
        }
    }
}

/* ==========================================================================
   Breakable / Explosive Entities
   ========================================================================== */

static void breakable_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                          int damage, vec3_t point)
{
    vec3_t up = {0, 0, 1};

    (void)inflictor; (void)damage; (void)point;

    /* Debris particles */
    R_ParticleEffect(self->s.origin, up, 0, 20);

    /* Fire targets on destruction */
    if (self->target)
        G_UseTargets(attacker, self->target);

    /* Remove the entity */
    self->takedamage = DAMAGE_NO;
    self->solid = SOLID_NOT;
    self->inuse = qfalse;
    gi.unlinkentity(self);
}

static void SP_func_breakable(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *health_str = ED_FindValue(pairs, num_pairs, "health");

    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BSP;
    ent->movetype = MOVETYPE_PUSH;
    ent->takedamage = DAMAGE_YES;
    ent->health = health_str ? atoi(health_str) : 50;
    ent->max_health = ent->health;
    ent->die = breakable_die;

    gi.linkentity(ent);
}

static void explosive_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                          int damage, vec3_t point)
{
    vec3_t up = {0, 0, 1};

    (void)inflictor; (void)damage; (void)point;

    /* Explosion particles and light */
    R_ParticleEffect(self->s.origin, up, 2, 32);
    R_AddDlight(self->s.origin, 1.0f, 0.6f, 0.1f, 400.0f, 0.5f);

    /* Fire targets */
    if (self->target)
        G_UseTargets(attacker, self->target);

    /* Damage nearby entities */
    {
        edict_t *touch[32];
        int num, i;
        vec3_t dmg_mins, dmg_maxs;
        float radius = self->dmg_radius ? (float)self->dmg_radius : 128.0f;

        VectorSet(dmg_mins, self->s.origin[0] - radius,
                            self->s.origin[1] - radius,
                            self->s.origin[2] - radius);
        VectorSet(dmg_maxs, self->s.origin[0] + radius,
                            self->s.origin[1] + radius,
                            self->s.origin[2] + radius);

        num = gi.BoxEdicts(dmg_mins, dmg_maxs, touch, 32, AREA_SOLID);
        for (i = 0; i < num; i++) {
            edict_t *t = touch[i];
            if (!t || t == self || !t->inuse || !t->takedamage)
                continue;
            {
                int explosion_dmg = self->dmg ? self->dmg : 100;
                t->health -= explosion_dmg;
                if (t->health <= 0 && t->die)
                    t->die(t, self, attacker, explosion_dmg, self->s.origin);
            }
        }
    }

    /* Remove self */
    self->takedamage = DAMAGE_NO;
    self->solid = SOLID_NOT;
    self->inuse = qfalse;
    gi.unlinkentity(self);
}

static void SP_func_explosive(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *health_str = ED_FindValue(pairs, num_pairs, "health");
    const char *dmg_str = ED_FindValue(pairs, num_pairs, "dmg");

    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BSP;
    ent->movetype = MOVETYPE_PUSH;
    ent->takedamage = DAMAGE_YES;
    ent->health = health_str ? atoi(health_str) : 30;
    ent->max_health = ent->health;
    ent->dmg = dmg_str ? atoi(dmg_str) : 100;
    ent->dmg_radius = ent->dmg * 2;
    ent->die = explosive_die;

    gi.linkentity(ent);
}

/* ==========================================================================
   target_relay — Receives use(), fires its own target chain
   ========================================================================== */

static void target_relay_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;
    G_FireTargets(self, activator);
}

static void SP_target_relay(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;
    ent->use = target_relay_use;
}

/* ==========================================================================
   func_wall — Toggleable solid BSP brush
   Spawnflags: 1 = start inactive (invisible + non-solid)
   ========================================================================== */

static void func_wall_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;

    if (self->solid == SOLID_BSP) {
        /* Turn off */
        self->solid = SOLID_NOT;
        self->svflags |= SVF_NOCLIENT;
        gi.unlinkentity(self);
    } else {
        /* Turn on */
        self->solid = SOLID_BSP;
        self->svflags &= ~SVF_NOCLIENT;
        gi.linkentity(self);
    }
}

static void SP_func_wall(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *sf_str = ED_FindValue(pairs, num_pairs, "spawnflags");
    int sf = sf_str ? atoi(sf_str) : 0;

    ent->movetype = MOVETYPE_PUSH;
    ent->use = func_wall_use;

    if (sf & 1) {
        /* Start off */
        ent->solid = SOLID_NOT;
        ent->svflags |= SVF_NOCLIENT;
    } else {
        ent->solid = SOLID_BSP;
    }

    gi.linkentity(ent);
}

static void target_speaker_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;
    if (self->noise_index)
        gi.sound(self, CHAN_AUTO, self->noise_index, self->volume ? self->volume : 1.0f,
                 self->attenuation ? self->attenuation : 1.0f, 0);
}

/* Looping speaker think — replays sound periodically */
static void target_speaker_loop_think(edict_t *self)
{
    if (self->noise_index)
        gi.sound(self, CHAN_AUTO, self->noise_index, self->volume ? self->volume : 1.0f,
                 self->attenuation ? self->attenuation : 1.0f, 0);
    self->nextthink = level.time + self->wait;
}

static void SP_target_speaker(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *noise = ED_FindValue(pairs, num_pairs, "noise");
    const char *vol_str = ED_FindValue(pairs, num_pairs, "volume");
    const char *atten_str = ED_FindValue(pairs, num_pairs, "attenuation");

    if (noise && noise[0]) {
        ent->noise_index = gi.soundindex(noise);
    }
    ent->volume = vol_str ? (float)atof(vol_str) : 1.0f;
    ent->attenuation = atten_str ? (float)atof(atten_str) : 1.0f;

    ent->use = target_speaker_use;

    /* Spawnflag 1 = looping: auto-play with repeat interval */
    if (ent->style & 1) {
        if (!ent->wait || ent->wait < 0.1f)
            ent->wait = 5.0f;  /* default loop every 5 seconds */
        ent->think = target_speaker_loop_think;
        ent->nextthink = level.time + 1.0f;  /* first play after 1s */
    }
}

static void SP_misc_model(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    /* Non-interactive model placement */
    ent->solid = 0;  /* SOLID_NOT */
    ent->movetype = 0;  /* MOVETYPE_NONE */
}

/* ==========================================================================
   Item / Weapon Pickups
   ========================================================================== */

/* Map classname to weapon ID for ammo/weapon pickup */
static int item_classname_to_weapon(const char *classname)
{
    static const struct { const char *name; int weap; } weapon_map[] = {
        { "weapon_knife",       WEAP_KNIFE },
        { "weapon_pistol1",     WEAP_PISTOL1 },
        { "weapon_pistol2",     WEAP_PISTOL2 },
        { "weapon_shotgun",     WEAP_SHOTGUN },
        { "weapon_machinegun",  WEAP_MACHINEGUN },
        { "weapon_assault",     WEAP_ASSAULT },
        { "weapon_sniper",      WEAP_SNIPER },
        { "weapon_slugger",     WEAP_SLUGGER },
        { "weapon_rocket",      WEAP_ROCKET },
        { "weapon_flamegun",    WEAP_FLAMEGUN },
        { "weapon_mpg",         WEAP_MPG },
        { "weapon_mpistol",     WEAP_MPISTOL },
        { "weapon_grenade",     WEAP_GRENADE },
        { "weapon_c4",          WEAP_C4 },
        { "ammo_pistol",        WEAP_PISTOL1 },
        { "ammo_shotgun",       WEAP_SHOTGUN },
        { "ammo_machinegun",    WEAP_MACHINEGUN },
        { "ammo_assault",       WEAP_ASSAULT },
        { "ammo_sniper",        WEAP_SNIPER },
        { "ammo_slugger",       WEAP_SLUGGER },
        { "ammo_rockets",       WEAP_ROCKET },
        { "ammo_flame",         WEAP_FLAMEGUN },
        { "ammo_mpg",           WEAP_MPG },
        { "ammo_grenades",      WEAP_GRENADE },
        { NULL, 0 }
    };
    int i;
    for (i = 0; weapon_map[i].name; i++) {
        if (Q_stricmp(classname, weapon_map[i].name) == 0)
            return weapon_map[i].weap;
    }
    return -1;
}

static const int weapon_pickup_ammo[WEAP_COUNT] = {
    0, 0, 12, 15, 8, 50, 30, 5, 20, 4, 50, 25, 30, 3, 1, 3, 0, 0
};
static const int ammo_pickup_amount[WEAP_COUNT] = {
    0, 0, 12, 15, 8, 50, 30, 5, 20, 4, 50, 25, 30, 2, 1, 2, 0, 0
};

static int snd_item_pickup;
static qboolean item_sounds_cached;

static void item_precache_sounds(void)
{
    if (item_sounds_cached) return;
    snd_item_pickup = gi.soundindex("items/pkup.wav");
    item_sounds_cached = qtrue;
}

/* Item respawn think — re-activate after deathmatch respawn delay */
static void item_respawn_think(edict_t *self)
{
    self->solid = SOLID_TRIGGER;
    self->svflags &= ~SVF_NOCLIENT;
    gi.linkentity(self);
}

static void item_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client || !self->classname)
        return;

    /* Health pickup */
    if (strstr(self->classname, "health")) {
        int heal = 25;
        if (strstr(self->classname, "large")) heal = 100;
        else if (strstr(self->classname, "small")) heal = 10;

        if (other->health >= other->max_health)
            return;

        other->health += heal;
        if (other->health > other->max_health)
            other->health = other->max_health;
        other->client->pers_health = other->health;
    }
    /* Armor pickup */
    else if (strstr(self->classname, "armor")) {
        int give = 50;
        if (strstr(self->classname, "body")) give = 200;
        else if (strstr(self->classname, "combat")) give = 100;
        else if (strstr(self->classname, "jacket")) give = 25;

        if (other->client->armor >= other->client->armor_max)
            return;

        other->client->armor += give;
        if (other->client->armor > other->client->armor_max)
            other->client->armor = other->client->armor_max;
    }
    /* Weapon/ammo pickup */
    else {
        int weap = item_classname_to_weapon(self->classname);
        if (weap > 0 && weap < WEAP_COUNT) {
            int give;
            qboolean is_weapon = (strncmp(self->classname, "weapon_", 7) == 0);

            give = is_weapon ? weapon_pickup_ammo[weap] : ammo_pickup_amount[weap];

            if (other->client->ammo[weap] >= other->client->ammo_max[weap] && !is_weapon)
                return;

            other->client->ammo[weap] += give;
            if (other->client->ammo[weap] > other->client->ammo_max[weap])
                other->client->ammo[weap] = other->client->ammo_max[weap];

            if (is_weapon) {
                other->client->pers_weapon = weap;
                other->weapon_index = weap;
            }
        }
    }

    gi.cprintf(other, PRINT_ALL, "Picked up %s\n", self->classname);

    /* Play pickup sound */
    if (snd_item_pickup)
        gi.sound(other, CHAN_ITEM, snd_item_pickup, 1.0f, ATTN_NORM, 0);

    /* Deathmatch: hide and schedule respawn; SP: remove permanently */
    {
        cvar_t *dm = gi.cvar("deathmatch", "0", 0);
        if (dm && dm->value) {
        self->solid = SOLID_NOT;
        self->svflags |= SVF_NOCLIENT;   /* hide from rendering */
        gi.unlinkentity(self);
        self->think = item_respawn_think;
        self->nextthink = level.time + 30.0f;  /* respawn in 30 seconds */
        } else {
            self->inuse = qfalse;
            gi.unlinkentity(self);
        }
    }
}

static void SP_item_pickup(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->movetype = MOVETYPE_NONE;
    ent->touch = item_touch;

    /* Items have a pickup bbox */
    VectorSet(ent->mins, -16, -16, -16);
    VectorSet(ent->maxs, 16, 16, 16);

    gi.linkentity(ent);
}

/* ==========================================================================
   Main Spawn Entry Point
   Called from g_main.c SpawnEntities
   ========================================================================== */

void G_SpawnEntities(const char *mapname, const char *entstring,
                     const char *spawnpoint)
{
    epair_t     pairs[MAX_ENTITY_FIELDS];
    int         num_pairs;
    const char  *data;
    int         entity_count = 0;
    int         spawned_count = 0;

    (void)spawnpoint;

    gi.dprintf("SpawnEntities: %s\n", mapname);

    data = entstring;
    if (!data || !data[0]) {
        gi.dprintf("  No entity string\n");
        return;
    }

    while (1) {
        data = ED_SkipWhitespace(data);
        if (!*data)
            break;

        data = ED_ParseEntity(data, pairs, &num_pairs);
        if (!data)
            break;

        entity_count++;

        /* Find classname */
        {
            const char  *classname;
            int         i;
            edict_t     *ent;
            qboolean    found;

            classname = ED_FindValue(pairs, num_pairs, "classname");
            if (!classname) {
                gi.dprintf("  Entity %d: no classname\n", entity_count);
                continue;
            }

            /* First entity is always worldspawn (use entity 0) */
            if (entity_count == 1) {
                ent = &globals.edicts[0];
            } else {
                ent = G_AllocEdict();
                if (!ent)
                    break;
            }

            /* Set common fields */
            ED_SetCommonFields(ent, pairs, num_pairs);

            /* Find and call spawn function */
            found = qfalse;
            for (i = 0; spawn_funcs[i].classname; i++) {
                if (Q_stricmp(classname, spawn_funcs[i].classname) == 0) {
                    spawn_funcs[i].spawn(ent, pairs, num_pairs);
                    found = qtrue;
                    spawned_count++;
                    break;
                }
            }

            if (!found) {
                /* Unknown entity type — keep it but log */
                Com_DPrintf("  Unknown classname: %s\n", classname);
                spawned_count++;
            }
        }
    }

    gi.dprintf("  %d entities parsed, %d spawned\n", entity_count, spawned_count);
}
