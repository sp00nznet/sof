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
static void SP_item_pickup(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_changelevel(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_rotating(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_button(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_target_changelevel(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_timer(edict_t *ent, epair_t *pairs, int num_pairs);

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
    { "func_wall",                  SP_misc_model },
    { "func_timer",                 SP_func_timer },
    { "func_train",                 SP_func_plat },
    { "func_conveyor",              SP_misc_model },
    { "func_group",                 SP_misc_model },

    /* Triggers */
    { "trigger_once",               SP_trigger_once },
    { "trigger_multiple",           SP_trigger_multiple },
    { "trigger_relay",              SP_trigger_once },
    { "trigger_always",             SP_trigger_once },
    { "trigger_changelevel",        SP_trigger_changelevel },
    { "trigger_push",               SP_trigger_multiple },
    { "trigger_hurt",               SP_trigger_multiple },
    { "trigger_gravity",            SP_trigger_multiple },

    /* Targets */
    { "target_speaker",             SP_target_speaker },
    { "target_changelevel",         SP_target_changelevel },
    { "target_explosion",           SP_target_speaker },
    { "target_temp_entity",         SP_target_speaker },

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
        player->max_health = player->client->pers_max_health;
        player->solid = SOLID_BBOX;
        player->clipmask = MASK_PLAYERSOLID;
        player->movetype = MOVETYPE_WALK;
        player->takedamage = DAMAGE_AIM;
        player->client->pers_weapon = WEAP_PISTOL1;
        player->weapon_index = WEAP_PISTOL1;
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
}

static void door_go_up(edict_t *self)
{
    if (self->moveinfo.state == MSTATE_UP || self->moveinfo.state == MSTATE_TOP)
        return;

    self->moveinfo.state = MSTATE_UP;
    Move_Calc(self, self->moveinfo.end_origin, door_hit_top);
}

static void door_go_down(edict_t *self)
{
    if (self->moveinfo.state == MSTATE_DOWN || self->moveinfo.state == MSTATE_BOTTOM)
        return;

    self->moveinfo.state = MSTATE_DOWN;
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
}

static void plat_hit_bottom(edict_t *self)
{
    self->moveinfo.state = MSTATE_BOTTOM;
    VectorClear(self->velocity);
    /* Wait then return up */
    self->nextthink = level.time + 1.0f;
    self->think = plat_go_up;
}

static void plat_go_down(edict_t *self)
{
    if (self->moveinfo.state == MSTATE_DOWN || self->moveinfo.state == MSTATE_BOTTOM)
        return;
    self->moveinfo.state = MSTATE_DOWN;
    Move_Calc(self, self->moveinfo.end_origin, plat_hit_bottom);
}

static void plat_go_up(edict_t *self)
{
    if (self->moveinfo.state == MSTATE_UP || self->moveinfo.state == MSTATE_TOP)
        return;
    self->moveinfo.state = MSTATE_UP;
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

static void trigger_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client)
        return;

    /* Debounce for trigger_multiple */
    if (self->dmg_debounce_time > level.time)
        return;

    self->dmg_debounce_time = level.time + self->wait;

    /* Fire targets */
    if (self->target)
        G_UseTargets(other, self->target);

    /* Print message if set */
    if (self->message)
        gi.centerprintf(other, "%s", self->message);

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
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BSP;
    ent->movetype = MOVETYPE_PUSH;

    /* Default rotation around Z axis at speed or default 100 */
    if (!ent->speed)
        ent->speed = 100;

    /* Default: rotate around Z (yaw) */
    if (!ent->avelocity[0] && !ent->avelocity[1] && !ent->avelocity[2])
        ent->avelocity[1] = ent->speed;

    gi.linkentity(ent);
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

static void button_fire(edict_t *self, edict_t *activator)
{
    if (self->moveinfo.state == MSTATE_UP || self->moveinfo.state == MSTATE_TOP)
        return;

    self->moveinfo.state = MSTATE_UP;
    self->activator = activator;
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

static void target_speaker_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;
    if (self->noise_index)
        gi.sound(self, 0, self->noise_index, 1.0f, 1.0f, 0);
}

static void SP_target_speaker(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *noise = ED_FindValue(pairs, num_pairs, "noise");
    if (noise && noise[0]) {
        ent->noise_index = gi.soundindex(noise);
    }
    ent->use = target_speaker_use;
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

static void item_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client)
        return;

    /* Simple pickup: print message, give health/ammo, remove item */
    if (self->classname) {
        if (strstr(self->classname, "health")) {
            int heal = 25;
            if (strstr(self->classname, "large")) heal = 100;
            else if (strstr(self->classname, "small")) heal = 10;

            if (other->health >= other->max_health)
                return;  /* already full */

            other->health += heal;
            if (other->health > other->max_health)
                other->health = other->max_health;
            if (other->client)
                other->client->pers_health = other->health;
        }

        gi.cprintf(other, PRINT_ALL, "Picked up %s\n", self->classname);
    }

    /* Remove the item */
    self->inuse = qfalse;
    gi.unlinkentity(self);
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
