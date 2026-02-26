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
    { "func_rotating",              SP_func_plat },
    { "func_button",                SP_func_door },
    { "func_wall",                  SP_misc_model },

    /* Triggers */
    { "trigger_once",               SP_trigger_once },
    { "trigger_multiple",           SP_trigger_multiple },
    { "trigger_relay",              SP_trigger_once },
    { "trigger_always",             SP_trigger_once },

    /* Targets */
    { "target_speaker",             SP_target_speaker },

    /* Misc */
    { "misc_model",                 SP_misc_model },

    /* Sentinel */
    { NULL, NULL }
};

/* ==========================================================================
   Entity Allocation
   ========================================================================== */

extern game_export_t globals;

static edict_t *G_AllocEdict(void)
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
    }

    /* TODO: Set configstrings for sky, cdtrack, etc. */
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
        player->solid = SOLID_BBOX;
        player->clipmask = MASK_PLAYERSOLID;
        player->movetype = MOVETYPE_WALK;
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

static void SP_func_door(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->movetype = 6;  /* MOVETYPE_PUSH */
    ent->solid = 3;     /* SOLID_BSP */

    if (!ent->speed)
        ent->speed = 100;

    gi.dprintf("  func_door '%s'\n", ent->targetname ? ent->targetname : "(unnamed)");
}

static void SP_func_plat(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->movetype = 6;  /* MOVETYPE_PUSH */
    ent->solid = 3;     /* SOLID_BSP */

    if (!ent->speed)
        ent->speed = 200;
}

static void SP_trigger_once(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = 2;  /* SOLID_TRIGGER */
    /* TODO: Set touch function */
}

static void SP_trigger_multiple(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = 2;  /* SOLID_TRIGGER */
    if (!ent->wait)
        ent->wait = 0.2f;
}

static void SP_target_speaker(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *noise = ED_FindValue(pairs, num_pairs, "noise");
    if (noise) {
        ent->noise_index = 0;  /* TODO: gi.soundindex(noise) */
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
