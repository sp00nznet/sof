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

#include <math.h>

/* Particle/light effects from renderer (unified binary) */
extern void R_ParticleEffect(vec3_t org, vec3_t dir, int type, int count);
extern void SCR_AddScreenShake(float intensity, float duration);
extern void R_AddDlight(vec3_t origin, float r, float g, float b,
                         float intensity, float duration);

/* Monster spawn functions (g_ai.c) — use void* for epair_t since they don't parse it */
extern void SP_monster_soldier(edict_t *ent, void *pairs, int num_pairs);
extern void SP_monster_soldier_light(edict_t *ent, void *pairs, int num_pairs);
extern void SP_monster_soldier_ss(edict_t *ent, void *pairs, int num_pairs);
extern void SP_monster_guard(edict_t *ent, void *pairs, int num_pairs);
extern void SP_monster_sniper(edict_t *ent, void *pairs, int num_pairs);
extern void SP_monster_boss(edict_t *ent, void *pairs, int num_pairs);
extern void SP_monster_medic(edict_t *ent, void *pairs, int num_pairs);

/* Forward declarations for precache functions */
static void door_precache_sounds(void);
static void item_precache_sounds(void);

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
static void SP_misc_particles(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_item_pickup(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_changelevel(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_rotating(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_door_rotating(edict_t *ent, epair_t *pairs, int num_pairs);
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
static void SP_path_corner(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_train(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_counter(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_target_earthquake(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_explobox_big(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_target_splash(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_secret(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_ladder(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_glass(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_info_npc(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_conveyor_real(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_water(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_pendulum(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_target_monster_maker(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_target_objective(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_drip(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_steam(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_sparks(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_dust(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_pushable(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_light_breakable(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_throwable(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_ambient_creature(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_readable(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_turret(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_tripmine(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_lava(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_acid(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_debris(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_rope(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_corpse(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trap_pressure_plate(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trap_swinging_blade(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_info_checkpoint(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_item_medkit(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_security_camera(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_smoke(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_item_armor(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_item_ammo_crate(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_wind(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_door_locked(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_water_current(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_spotlight(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_elevator(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_fire(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_electric(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_objective(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_misc_supply_crate(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_audio_zone(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_item_shield(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_info_landmark(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_mirror(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_env_fog(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_valve(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_security_door(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_alarm(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_cover(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_cutscene(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_music(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_cage(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_hazard(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_zipline(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_elevator_call(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_crane(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_generator(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_trap_floor(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_modstation(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_spotlight(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_portcullis(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_floodlight(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_barrier(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_func_crate(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_cover_point(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_trigger_monster_spawn(edict_t *ent, epair_t *pairs, int num_pairs);
static void SP_fallback_point(edict_t *ent, epair_t *pairs, int num_pairs);
static void explosive_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                           int damage, vec3_t point);

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
    { "func_light_breakable",       SP_func_light_breakable },
    { "light_breakable",            SP_func_light_breakable },

    /* Brush entities */
    { "func_door",                  SP_func_door },
    { "func_door_rotating",         SP_func_door_rotating },
    { "func_plat",                  SP_func_plat },
    { "func_rotating",              SP_func_rotating },
    { "func_button",                SP_func_button },
    { "func_wall",                  SP_func_wall },
    { "func_timer",                 SP_func_timer },
    { "func_train",                 SP_func_train },
    { "func_ladder",                SP_func_ladder },
    { "func_glass",                 SP_func_glass },
    { "func_water",                 SP_func_water },
    { "func_pendulum",              SP_func_pendulum },
    { "func_conveyor",              SP_func_conveyor_real },
    { "func_group",                 SP_misc_model },
    { "path_corner",                SP_path_corner },

    /* Triggers */
    { "trigger_once",               SP_trigger_once },
    { "trigger_multiple",           SP_trigger_multiple },
    { "trigger_relay",              SP_target_relay },
    { "trigger_always",             SP_trigger_once },
    { "trigger_changelevel",        SP_trigger_changelevel },
    { "trigger_push",               SP_trigger_push },
    { "trigger_hurt",               SP_trigger_hurt },
    { "trigger_gravity",            SP_trigger_multiple },
    { "trigger_counter",            SP_trigger_counter },
    { "trigger_teleport",           SP_trigger_teleport },
    { "misc_teleport_dest",         SP_misc_teleport_dest },
    { "info_teleport_destination",  SP_misc_teleport_dest },

    /* Targets */
    { "target_speaker",             SP_target_speaker },
    { "target_changelevel",         SP_target_changelevel },
    { "target_relay",               SP_target_relay },
    { "target_earthquake",          SP_target_earthquake },
    { "target_explosion",           SP_target_speaker },
    { "target_temp_entity",         SP_target_speaker },
    { "target_monster_maker",       SP_target_monster_maker },
    { "target_objective",           SP_target_objective },

    /* Breakable/explosive/pushable */
    { "func_breakable",             SP_func_breakable },
    { "func_explosive",             SP_func_explosive },
    { "func_pushable",              SP_func_pushable },
    { "func_object",                SP_func_pushable },
    { "misc_explobox",              SP_func_explosive },
    { "misc_explobox_big",          SP_misc_explobox_big },
    { "func_debris",                SP_func_debris },
    { "func_rope",                  SP_func_rope },
    { "func_zipline",               SP_func_rope },

    /* Splash effects */
    { "target_splash",              SP_target_splash },

    /* Environmental particle emitters */
    { "env_drip",                   SP_env_drip },
    { "env_steam",                  SP_env_steam },
    { "env_sparks",                 SP_env_sparks },
    { "env_dust",                   SP_env_dust },
    { "misc_particles",             SP_env_dust },

    /* Environmental hazard zones */
    { "env_lava",                   SP_env_lava },
    { "env_acid",                   SP_env_acid },
    { "func_lava",                  SP_env_lava },
    { "func_acid",                  SP_env_acid },

    /* Secret */
    { "trigger_secret",             SP_trigger_secret },

    /* Monsters (g_ai.c) */
    { "monster_soldier",            (void (*)(edict_t *, epair_t *, int))SP_monster_soldier },
    { "monster_soldier_light",      (void (*)(edict_t *, epair_t *, int))SP_monster_soldier_light },
    { "monster_soldier_ss",         (void (*)(edict_t *, epair_t *, int))SP_monster_soldier_ss },
    { "monster_guard",              (void (*)(edict_t *, epair_t *, int))SP_monster_guard },
    { "monster_sniper",             (void (*)(edict_t *, epair_t *, int))SP_monster_sniper },
    { "monster_boss",               (void (*)(edict_t *, epair_t *, int))SP_monster_boss },
    { "monster_medic",              (void (*)(edict_t *, epair_t *, int))SP_monster_medic },
    /* Q2-compatible monster names */
    { "monster_soldier_light",      (void (*)(edict_t *, epair_t *, int))SP_monster_soldier_light },
    { "monster_infantry",           (void (*)(edict_t *, epair_t *, int))SP_monster_soldier },
    { "monster_gunner",             (void (*)(edict_t *, epair_t *, int))SP_monster_soldier_ss },
    { "monster_enforcer",           (void (*)(edict_t *, epair_t *, int))SP_monster_soldier },

    /* Misc */
    { "misc_model",                 SP_misc_model },
    { "misc_particles",             SP_misc_particles },
    { "misc_throwable",             SP_misc_throwable },
    { "misc_bottle",                SP_misc_throwable },
    { "misc_barrel_small",          SP_misc_throwable },
    { "info_npc",                   SP_info_npc },
    { "info_npc_talk",              SP_info_npc },
    { "misc_rat",                   SP_misc_ambient_creature },
    { "misc_bat",                   SP_misc_ambient_creature },
    { "misc_bird",                  SP_misc_ambient_creature },
    { "misc_cockroach",             SP_misc_ambient_creature },
    { "misc_readable",              SP_misc_readable },
    { "misc_note",                  SP_misc_readable },
    { "misc_sign",                  SP_misc_readable },
    { "misc_corpse",                SP_misc_corpse },
    { "misc_dead_body",             SP_misc_corpse },
    { "misc_turret",                SP_misc_turret },
    { "misc_mounted_gun",           SP_misc_turret },
    { "misc_tripmine",              SP_misc_tripmine },
    { "misc_trap",                  SP_misc_tripmine },
    { "info_checkpoint",            SP_info_checkpoint },
    { "trap_pressure_plate",        SP_trap_pressure_plate },
    { "trap_swinging_blade",        SP_trap_swinging_blade },
    { "trap_blade",                 SP_trap_swinging_blade },

    /* Pickups */
    { "item_medkit",                SP_item_medkit },
    { "item_health",                SP_item_medkit },
    { "item_health_large",          SP_item_medkit },

    /* Surveillance */
    { "misc_security_camera",       SP_misc_security_camera },
    { "misc_camera",                SP_misc_security_camera },

    /* Smoke/fog volumes */
    { "env_smoke",                  SP_env_smoke },
    { "env_fog_volume",             SP_env_smoke },

    /* Armor & ammo pickups */
    { "item_armor",                 SP_item_armor },
    { "item_armor_body",            SP_item_armor },
    { "item_armor_jacket",          SP_item_armor },
    { "item_ammo_crate",            SP_item_ammo_crate },
    { "item_ammo",                  SP_item_ammo_crate },

    /* Wind/push zones */
    { "env_wind",                   SP_env_wind },
    { "trigger_wind",               SP_env_wind },

    /* Locked doors (require key item use to open) */
    { "func_door_locked",           SP_func_door_locked },

    /* Water current */
    { "env_water_current",          SP_env_water_current },
    { "func_current",               SP_env_water_current },

    /* Spotlight (visual) */
    { "misc_spotlight",             SP_misc_spotlight },
    { "light_spot",                 SP_misc_spotlight },

    /* Elevator (vertical platform with call button) */
    { "func_elevator",              SP_func_elevator },
    { "func_lift",                  SP_func_elevator },

    /* Fire hazard */
    { "env_fire",                   SP_env_fire },
    { "env_flame",                  SP_env_fire },

    /* Electricity hazard */
    { "env_electric",               SP_env_electric },
    { "env_tesla",                  SP_env_electric },

    /* Objectives */
    { "trigger_objective",          SP_trigger_objective },
    { "info_objective",             SP_trigger_objective },

    /* Supply crate */
    { "misc_supply_crate",          SP_misc_supply_crate },
    { "item_supply",                SP_misc_supply_crate },

    /* Audio zones */
    { "trigger_audio_zone",         SP_trigger_audio_zone },
    { "env_audio",                  SP_trigger_audio_zone },

    /* Power-ups */
    { "item_shield",                SP_item_shield },
    { "item_invuln",                SP_item_shield },
    { "item_powerup",               SP_item_shield },

    /* Map transition landmarks */
    { "info_landmark",              SP_info_landmark },

    /* Reflective surfaces */
    { "func_mirror",                SP_func_mirror },

    /* Fog volume */
    { "env_fog",                    SP_env_fog },
    { "env_fog_global",             SP_env_fog },

    /* Valve/wheel interaction */
    { "func_valve",                 SP_func_valve },
    { "func_wheel",                 SP_func_valve },

    /* Security doors (keycard + delay) */
    { "func_security_door",         SP_func_security_door },
    { "func_blast_door",            SP_func_security_door },

    /* Alarm system */
    { "func_alarm",                 SP_func_alarm },
    { "misc_alarm",                 SP_func_alarm },

    /* Destructible cover */
    { "func_cover",                 SP_func_cover },
    { "func_barricade",             SP_func_cover },

    /* Cutscene triggers */
    { "trigger_cutscene",           SP_trigger_cutscene },
    { "trigger_cinematic",          SP_trigger_cutscene },

    /* Music triggers */
    { "trigger_music",              SP_trigger_music },
    { "target_music",               SP_trigger_music },

    /* Trap cage */
    { "func_cage",                  SP_func_cage },
    { "func_trap_cage",             SP_func_cage },

    /* Mountable turret (alias) */
    { "func_turret",                SP_misc_turret },

    /* Environmental hazard zone */
    { "trigger_hazard",             SP_trigger_hazard },
    { "trigger_hurt_zone",          SP_trigger_hazard },

    /* Zipline */
    { "func_zipline",               SP_func_zipline },
    { "misc_zipline",               SP_func_zipline },

    /* Elevator call button */
    { "func_elevator_call",         SP_func_elevator_call },
    { "func_call_button",           SP_func_elevator_call },

    /* Crane */
    { "func_crane",                 SP_func_crane },
    { "misc_crane",                 SP_func_crane },

    /* Conveyor belt (alias) */
    { "trigger_conveyor",           SP_func_conveyor_real },

    /* Power generator */
    { "func_generator",             SP_func_generator },
    { "misc_generator",             SP_func_generator },

    /* Trap floor */
    { "func_trap_floor",            SP_func_trap_floor },
    { "trigger_trap_floor",         SP_func_trap_floor },

    /* Weapon mod station */
    { "func_modstation",            SP_func_modstation },
    { "misc_modstation",            SP_func_modstation },

    /* Spotlight (dynamic light version) */
    { "func_spotlight",             SP_func_spotlight },

    /* Portcullis / gate */
    { "func_portcullis",            SP_func_portcullis },
    { "func_gate",                  SP_func_portcullis },

    /* Floodlight */
    { "func_floodlight",            SP_func_floodlight },
    { "light_flood",                SP_func_floodlight },

    /* Barrier / bollard */
    { "func_barrier",               SP_func_barrier },
    { "func_bollard",               SP_func_barrier },

    /* Destroyable crate */
    { "func_crate",                 SP_func_crate },
    { "misc_crate",                 SP_func_crate },

    /* AI cover node */
    { "cover_point",                SP_cover_point },
    { "info_cover",                 SP_cover_point },

    /* Monster spawner */
    { "trigger_monster_spawn",      SP_trigger_monster_spawn },
    { "target_spawn",               SP_trigger_monster_spawn },

    /* AI fallback point */
    { "fallback_point",             SP_fallback_point },
    { "info_fallback",              SP_fallback_point },

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
    { "item_armor_shard",           SP_item_pickup },
    { "item_ammo_crate",            SP_item_pickup },

    /* Keys */
    { "key_red",                    SP_item_pickup },
    { "key_blue",                   SP_item_pickup },
    { "key_silver",                 SP_item_pickup },
    { "key_gold",                   SP_item_pickup },
    { "item_key_red",               SP_item_pickup },
    { "item_key_blue",              SP_item_pickup },

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

    /* Ambient music track */
    val = ED_FindValue(pairs, num_pairs, "music");
    if (!val) val = ED_FindValue(pairs, num_pairs, "ambient_track");
    if (val) {
        gi.dprintf("Ambient track: %s\n", val);
        gi.configstring(CS_CDTRACK, val);
    }

    /* Gravity override */
    val = ED_FindValue(pairs, num_pairs, "gravity");
    if (val) {
        gi.cvar_set("sv_gravity", val);
    }

    /* Weather — set via "weather" key: 0=none, 1=rain, 2=snow */
    val = ED_FindValue(pairs, num_pairs, "weather");
    if (val) {
        level.weather = atoi(val);
        if (level.weather < 0) level.weather = 0;
        if (level.weather > 2) level.weather = 2;
        level.weather_density = 1.0f;
        gi.dprintf("Weather: %s\n",
                   level.weather == 1 ? "rain" :
                   level.weather == 2 ? "snow" : "none");
    }

    /* Weather density override */
    val = ED_FindValue(pairs, num_pairs, "weather_density");
    if (val) {
        level.weather_density = (float)atof(val);
        if (level.weather_density < 0.1f) level.weather_density = 0.1f;
        if (level.weather_density > 3.0f) level.weather_density = 3.0f;
    }

    /* Precache common sounds */
    item_precache_sounds();
    door_precache_sounds();
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
    /* Set lightstyle if the light has a style with a pattern */
    if (ent->style) {
        const char *pattern = ED_FindValue(pairs, num_pairs, "style_pattern");
        if (!pattern) {
            /* Standard Q2 lightstyle patterns */
            static const char *default_patterns[] = {
                "m",                                        /* 0: normal */
                "mmnmmommommnonmmonqnmmo",                  /* 1: FLICKER */
                "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba", /* 2: SLOW PULSE */
                "mmmmmaaaaammmmmaaaaaabcdefgabcdefg",        /* 3: CANDLE */
                "mamamamamama",                             /* 4: FAST STROBE */
                "jklmnopqrstuvwxyzyxwvutsrqponmlkj",        /* 5: GENTLE PULSE */
                "nmonqnmomnmomomno",                        /* 6: FLICKER2 */
                "mmmaaaabcdefgmmmmaaaammmaamm",              /* 7: CANDLE2 */
                "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa", /* 8: CANDLE3 */
                "aaaaaaaazzzzzzzz",                         /* 9: SLOW STROBE */
                "mmamammmmammamamaaamammma",                 /* 10: FLUORESCENT */
                "abcdefghijklmnopqrrqponmlkjihgfedcba",     /* 11: SLOW PULSE2 */
            };

            if (ent->style > 0 && ent->style < 12) {
                gi.configstring(CS_LIGHTS + ent->style, default_patterns[ent->style]);
            }
        } else {
            gi.configstring(CS_LIGHTS + ent->style, pattern);
        }
    }

    /* Lights are compiled into lightmaps, no runtime entity needed */
    ent->inuse = qfalse;
}

/* Forward declaration */
static void G_UseTargets(edict_t *activator, const char *target);

/*
 * Destructible light — can be shot out, turning off its lightstyle
 */
static void light_breakable_die(edict_t *self, edict_t *inflictor,
                                edict_t *attacker, int damage, vec3_t point)
{
    (void)inflictor; (void)attacker; (void)damage; (void)point;

    /* Turn off the light by setting lightstyle to pitch black */
    if (self->style > 0 && self->style < 64)
        gi.configstring(CS_LIGHTS + self->style, "a");

    /* Glass break particles */
    {
        vec3_t up = {0, 0, -1};
        R_ParticleEffect(self->s.origin, up, 11, 8);   /* debris */
        R_ParticleEffect(self->s.origin, up, 12, 4);   /* sparks */
    }

    /* One-time spark flash */
    R_AddDlight(self->s.origin, 1.0f, 0.8f, 0.2f, 200.0f, 0.3f);

    /* Fire targets */
    if (self->target)
        G_UseTargets(attacker, self->target);

    /* Remove the entity */
    self->takedamage = DAMAGE_NO;
    self->solid = SOLID_NOT;
    self->deadflag = 1;
    gi.linkentity(self);
}

static void SP_func_light_breakable(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *health_str = ED_FindValue(pairs, num_pairs, "health");

    ent->solid = SOLID_BSP;
    ent->movetype = MOVETYPE_PUSH;
    ent->takedamage = DAMAGE_YES;
    ent->health = health_str ? atoi(health_str) : 15;
    ent->max_health = ent->health;
    ent->die = light_breakable_die;

    /* Apply lightstyle if set */
    if (ent->style > 0 && ent->style < 64)
        gi.configstring(CS_LIGHTS + ent->style, "m");

    gi.linkentity(ent);
}

/*
 * Throwable object — can be picked up with USE and thrown with ATTACK
 *
 * When held, the object floats in front of the player.
 * Pressing attack throws it in the look direction, dealing impact damage.
 */
static void throwable_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;

    if (!activator || !activator->client)
        return;

    /* If player is already holding something, drop it first */
    if (activator->client->held_object) {
        edict_t *held = activator->client->held_object;
        held->movetype = MOVETYPE_TOSS;
        held->solid = SOLID_BBOX;
        gi.linkentity(held);
        activator->client->held_object = NULL;
    }

    /* Pick up this object */
    activator->client->held_object = self;
    self->solid = SOLID_NOT;
    self->movetype = MOVETYPE_NONE;
    gi.linkentity(self);
}

static void throwable_touch(edict_t *self, edict_t *other,
                            void *plane, void *surf)
{
    (void)plane; (void)surf;

    /* Deal impact damage on collision after being thrown */
    if (self->dmg > 0 && other && other->takedamage && other->health > 0) {
        float speed = VectorLength(self->velocity);
        if (speed > 100.0f) {
            int dmg = self->dmg;
            other->health -= dmg;
            R_ParticleEffect(self->s.origin, self->velocity, 11, 4);

            if (other->health <= 0 && other->die)
                other->die(other, self, self->owner, dmg, self->s.origin);
        }
        self->dmg = 0;  /* Only damage on first impact */
    }

    /* Slow down on each bounce */
    VectorScale(self->velocity, 0.5f, self->velocity);
}

static void SP_misc_throwable(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *dmg_str = ED_FindValue(pairs, num_pairs, "dmg");
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->takedamage = DAMAGE_NO;
    ent->dmg = dmg_str ? atoi(dmg_str) : 10;
    ent->use = throwable_use;
    ent->touch = throwable_touch;

    /* Small bounding box */
    VectorSet(ent->mins, -4, -4, -4);
    VectorSet(ent->maxs, 4, 4, 4);

    gi.linkentity(ent);
}

/* ==========================================================================
   Ambient Creatures — misc_rat, misc_bat, misc_bird, misc_cockroach
   Small entities that flee from the player for atmosphere.
   ========================================================================== */

static void ambient_creature_think(edict_t *self)
{
    edict_t *player = &globals.edicts[1];
    vec3_t diff;
    float dist;

    if (!self->inuse) return;

    if (player && player->inuse && player->client) {
        VectorSubtract(self->s.origin, player->s.origin, diff);
        diff[2] = 0;
        dist = VectorLength(diff);

        /* Flee when player gets close */
        if (dist < 200.0f && dist > 1.0f) {
            float flee_speed = self->speed > 0 ? self->speed : 120.0f;
            VectorScale(diff, 1.0f / dist, diff);  /* normalize */
            self->velocity[0] = diff[0] * flee_speed;
            self->velocity[1] = diff[1] * flee_speed;

            /* Bats fly upward when disturbed */
            if (strstr(self->classname, "bat") || strstr(self->classname, "bird"))
                self->velocity[2] = 80.0f;
        } else if (dist >= 200.0f) {
            /* Stop fleeing */
            self->velocity[0] *= 0.8f;
            self->velocity[1] *= 0.8f;
            self->velocity[2] *= 0.8f;
        }
    }

    /* Random scurry — small random movement while idle */
    if (gi.irand(0, 20) == 0) {
        self->velocity[0] += gi.flrand(-30.0f, 30.0f);
        self->velocity[1] += gi.flrand(-30.0f, 30.0f);
    }

    self->nextthink = level.time + 0.2f;
    gi.linkentity(self);
}

static void SP_misc_ambient_creature(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *speed_str = ED_FindValue(pairs, num_pairs, "speed");
    int is_flying;
    (void)pairs; (void)num_pairs;

    is_flying = (strstr(ent->classname, "bat") || strstr(ent->classname, "bird")) ? 1 : 0;

    ent->movetype = is_flying ? MOVETYPE_FLY : MOVETYPE_STEP;
    ent->solid = SOLID_NOT;  /* non-solid: just visual */
    ent->takedamage = DAMAGE_YES;
    ent->health = 1;  /* one-shot kill */
    ent->max_health = 1;
    ent->speed = speed_str ? (float)atof(speed_str) : 120.0f;
    ent->think = ambient_creature_think;
    ent->nextthink = level.time + 1.0f + gi.flrand(0, 2.0f);  /* stagger start */

    VectorSet(ent->mins, -4, -4, 0);
    VectorSet(ent->maxs, 4, 4, is_flying ? 4 : 8);

    gi.linkentity(ent);
    gi.dprintf("  %s at (%.0f %.0f %.0f)\n", ent->classname,
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   Readable Notes / Signs
   Displays message text when the player uses (interacts with) them.
   ========================================================================== */

static void readable_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;
    if (!activator || !activator->client)
        return;

    if (self->message && self->message[0]) {
        gi.cprintf(activator, PRINT_ALL, "\n--- %s ---\n%s\n---\n",
                   self->classname, self->message);
        SCR_AddPickupMessage(self->message);
    }
}

static void SP_misc_readable(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *msg = ED_FindValue(pairs, num_pairs, "message");
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->takedamage = DAMAGE_NO;
    ent->use = readable_use;
    ent->message = msg ? (char *)msg : "...";

    VectorSet(ent->mins, -8, -8, -8);
    VectorSet(ent->maxs, 8, 8, 8);

    gi.linkentity(ent);
    gi.dprintf("  misc_readable '%s'\n", ent->message);
}

/* ==========================================================================
   Turret / Mounted Gun
   Player uses (interacts) to mount, fires with attack button.
   High damage, limited rotation, fixed position.
   ========================================================================== */

static void turret_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;
    if (!activator || !activator->client)
        return;

    if (activator->client->held_object == self) {
        /* Dismount turret */
        activator->client->held_object = NULL;
        gi.cprintf(activator, PRINT_ALL, "Dismounted turret\n");
        return;
    }

    /* Mount turret — store reference in held_object (overloaded) */
    activator->client->held_object = self;
    gi.cprintf(activator, PRINT_ALL, "Mounted turret — ATTACK to fire, USE to dismount\n");
    SCR_AddPickupMessage("Mounted Turret");
}

static void turret_think(edict_t *self)
{
    extern game_export_t globals;
    edict_t *player = &globals.edicts[1];

    if (!self->inuse) return;
    self->nextthink = level.time + 0.1f;

    /* Check if player is mounted on this turret */
    if (player && player->inuse && player->client &&
        player->client->held_object == self) {
        /* Keep player position locked to turret */
        vec3_t diff;
        float dist;
        VectorSubtract(player->s.origin, self->s.origin, diff);
        dist = VectorLength(diff);
        if (dist > 64.0f) {
            /* Player moved too far — dismount */
            player->client->held_object = NULL;
        }
    }
}

static void SP_misc_turret(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *dmg_str = ED_FindValue(pairs, num_pairs, "dmg");
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->takedamage = DAMAGE_NO;
    ent->dmg = dmg_str ? atoi(dmg_str) : 40;  /* turret damage per shot */
    ent->use = turret_use;
    ent->think = turret_think;
    ent->nextthink = level.time + 1.0f;

    VectorSet(ent->mins, -12, -12, 0);
    VectorSet(ent->maxs, 12, 12, 24);

    gi.linkentity(ent);
    gi.dprintf("  misc_turret at (%.0f %.0f %.0f) dmg=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->dmg);
}

/* ==========================================================================
   Trip Mine — Proximity explosive that detonates when entities get close
   ========================================================================== */

static void tripmine_think(edict_t *self)
{
    edict_t *touch[16];
    int num, i;
    vec3_t prox_mins, prox_maxs;
    float trigger_range = self->dmg_radius > 0 ? (float)self->dmg_radius * 0.5f : 64.0f;

    if (!self->inuse) return;
    self->nextthink = level.time + 0.2f;  /* check 5 times per second */

    /* Arm delay: don't trigger for first 2 seconds */
    if (self->teleport_time > level.time)
        return;

    VectorSet(prox_mins, self->s.origin[0] - trigger_range,
                         self->s.origin[1] - trigger_range,
                         self->s.origin[2] - trigger_range);
    VectorSet(prox_maxs, self->s.origin[0] + trigger_range,
                         self->s.origin[1] + trigger_range,
                         self->s.origin[2] + trigger_range);

    num = gi.BoxEdicts(prox_mins, prox_maxs, touch, 16, AREA_SOLID);
    for (i = 0; i < num; i++) {
        if (touch[i] && touch[i] != self && touch[i]->inuse &&
            (touch[i]->client || (touch[i]->svflags & SVF_MONSTER))) {
            /* Triggered! Explode */
            {
                vec3_t up = {0, 0, 1};
                R_ParticleEffect(self->s.origin, up, 2, 24);
                R_AddDlight(self->s.origin, 1.0f, 0.5f, 0.1f, 350.0f, 0.4f);
                {
                    int snd = gi.soundindex("weapons/explode.wav");
                    if (snd)
                        gi.positioned_sound(self->s.origin, NULL, CHAN_AUTO,
                                            snd, 1.0f, ATTN_NORM, 0);
                }
            }
            SCR_AddScreenShake(0.5f, 0.3f);

            /* Damage nearby entities */
            {
                int j;
                for (j = 0; j < num; j++) {
                    edict_t *t = touch[j];
                    if (t && t != self && t->inuse && t->takedamage) {
                        t->health -= self->dmg;
                        if (t->health <= 0 && t->die)
                            t->die(t, self, self->owner ? self->owner : self,
                                   self->dmg, self->s.origin);
                    }
                }
            }

            /* Remove self */
            self->inuse = qfalse;
            gi.unlinkentity(self);
            return;
        }
    }
}

static void SP_misc_tripmine(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *dmg_str = ED_FindValue(pairs, num_pairs, "dmg");
    const char *radius_str = ED_FindValue(pairs, num_pairs, "dmg_radius");
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->takedamage = DAMAGE_YES;
    ent->health = 1;  /* can be shot to detonate */
    ent->max_health = 1;
    ent->dmg = dmg_str ? atoi(dmg_str) : 75;
    ent->dmg_radius = radius_str ? atoi(radius_str) : 128;
    ent->think = tripmine_think;
    ent->nextthink = level.time + 0.5f;
    ent->teleport_time = level.time + 2.0f;  /* 2s arm delay */
    ent->die = explosive_die;  /* can be shot */

    VectorSet(ent->mins, -4, -4, -4);
    VectorSet(ent->maxs, 4, 4, 4);

    gi.linkentity(ent);
    gi.dprintf("  misc_tripmine at (%.0f %.0f %.0f) dmg=%d radius=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->dmg, ent->dmg_radius);
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

/* Check if player has the key required to open a door */
static qboolean door_check_key(edict_t *door, edict_t *other)
{
    if (door->count == 0)
        return qtrue;  /* no key required */
    if (!other || !other->client)
        return qfalse;
    if (other->client->keys & door->count)
        return qtrue;

    /* Player doesn't have the key — show message */
    if (door->count & KEY_RED)
        gi.cprintf(other, PRINT_ALL, "You need the Red Key\n");
    else if (door->count & KEY_BLUE)
        gi.cprintf(other, PRINT_ALL, "You need the Blue Key\n");
    else if (door->count & KEY_SILVER)
        gi.cprintf(other, PRINT_ALL, "You need the Silver Key\n");
    else if (door->count & KEY_GOLD)
        gi.cprintf(other, PRINT_ALL, "You need the Gold Key\n");
    return qfalse;
}

/* Door blocked — crush damage when entity caught in closing door */
static void door_blocked(edict_t *self, edict_t *other)
{
    if (!other) return;

    if (other->takedamage && other->health > 0) {
        int crush_dmg = self->dmg ? self->dmg : 5;
        other->health -= crush_dmg;
        if (other->client)
            other->client->pers_health = other->health;
        if (other->health <= 0 && other->die)
            other->die(other, self, self, crush_dmg, other->s.origin);
    }

    /* Reverse the door to avoid trapping */
    if (self->moveinfo.state == MSTATE_DOWN)
        door_go_up(self);
    else if (self->moveinfo.state == MSTATE_UP)
        door_go_down(self);
}

/* Touch callback for doors without targetname (auto-open) */
static void door_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client)
        return;

    if (!door_check_key(self, other))
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

    /* Check for required key */
    {
        const char *key_str = ED_FindValue(pairs, num_pairs, "key");
        if (key_str) {
            if (Q_stricmp(key_str, "red") == 0) ent->count = KEY_RED;
            else if (Q_stricmp(key_str, "blue") == 0) ent->count = KEY_BLUE;
            else if (Q_stricmp(key_str, "silver") == 0) ent->count = KEY_SILVER;
            else if (Q_stricmp(key_str, "gold") == 0) ent->count = KEY_GOLD;
            else ent->count = atoi(key_str);
        }
    }

    /* If no targetname, door opens on touch */
    if (!ent->targetname) {
        ent->touch = door_touch;
    }
    ent->use = door_use;
    ent->blocked = door_blocked;

    gi.linkentity(ent);
    gi.dprintf("  func_door '%s'%s\n", ent->targetname ? ent->targetname : "(auto)",
               ent->count ? " (locked)" : "");
}

/* ==========================================================================
   func_door_rotating — BSP brush that rotates open/closed
   ========================================================================== */

static void rot_door_go_open(edict_t *self);
static void rot_door_go_close(edict_t *self);

static void rot_door_think(edict_t *self)
{
    vec3_t delta;
    float remaining;

    VectorSubtract(self->moveinfo.end_angles, self->s.angles, delta);
    remaining = VectorLength(delta);

    if (remaining < 1.0f) {
        /* Arrived at target angle */
        VectorCopy(self->moveinfo.end_angles, self->s.angles);
        VectorClear(self->avelocity);
        gi.linkentity(self);

        if (snd_door_end)
            gi.sound(self, CHAN_AUTO, snd_door_end, 1.0f, 1, 0);

        if (self->moveinfo.endfunc)
            self->moveinfo.endfunc(self);
        return;
    }

    self->nextthink = level.time + level.frametime;
    self->think = rot_door_think;
}

static void rot_door_hit_open(edict_t *self)
{
    self->moveinfo.state = MSTATE_TOP;

    if (self->moveinfo.wait >= 0) {
        self->nextthink = level.time + self->moveinfo.wait;
        self->think = rot_door_go_close;
    }
}

static void rot_door_hit_closed(edict_t *self)
{
    self->moveinfo.state = MSTATE_BOTTOM;
}

static void rot_door_go_open(edict_t *self)
{
    vec3_t delta;
    float dist, time;

    if (self->moveinfo.state == MSTATE_UP || self->moveinfo.state == MSTATE_TOP)
        return;

    self->moveinfo.state = MSTATE_UP;
    self->moveinfo.endfunc = rot_door_hit_open;

    /* Target angles = end_origin (we store open angles there) */
    VectorCopy(self->moveinfo.end_origin, self->moveinfo.end_angles);

    /* Calculate angular velocity */
    VectorSubtract(self->moveinfo.end_angles, self->s.angles, delta);
    dist = VectorLength(delta);
    if (dist < 0.1f) { rot_door_hit_open(self); return; }

    time = dist / self->moveinfo.speed;
    VectorScale(delta, 1.0f / time, self->avelocity);

    if (snd_door_start)
        gi.sound(self, CHAN_AUTO, snd_door_start, 1.0f, 1, 0);

    self->nextthink = level.time + time;
    self->think = rot_door_think;
}

static void rot_door_go_close(edict_t *self)
{
    vec3_t delta;
    float dist, time;

    if (self->moveinfo.state == MSTATE_DOWN || self->moveinfo.state == MSTATE_BOTTOM)
        return;

    self->moveinfo.state = MSTATE_DOWN;
    self->moveinfo.endfunc = rot_door_hit_closed;

    /* Target = closed position (start_origin stores closed angles) */
    VectorCopy(self->moveinfo.start_origin, self->moveinfo.end_angles);

    VectorSubtract(self->moveinfo.end_angles, self->s.angles, delta);
    dist = VectorLength(delta);
    if (dist < 0.1f) { rot_door_hit_closed(self); return; }

    time = dist / self->moveinfo.speed;
    VectorScale(delta, 1.0f / time, self->avelocity);

    if (snd_door_start)
        gi.sound(self, CHAN_AUTO, snd_door_start, 1.0f, 1, 0);

    self->nextthink = level.time + time;
    self->think = rot_door_think;
}

static void rot_door_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;
    if (self->moveinfo.state == MSTATE_BOTTOM || self->moveinfo.state == MSTATE_DOWN)
        rot_door_go_open(self);
    else
        rot_door_go_close(self);
}

static void rot_door_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;
    if (!other || !other->client) return;
    if (self->moveinfo.state == MSTATE_BOTTOM)
        rot_door_go_open(self);
}

static void SP_func_door_rotating(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *distance_str, *axis_str;
    float distance;
    int axis = 1;  /* default: yaw (Y axis) */

    door_precache_sounds();

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;

    if (!ent->speed)
        ent->speed = 100;

    /* Rotation distance in degrees (default 90) */
    distance_str = ED_FindValue(pairs, num_pairs, "distance");
    distance = distance_str ? (float)atof(distance_str) : 90.0f;

    /* Rotation axis: "x"=pitch, "z"=roll, default=yaw */
    axis_str = ED_FindValue(pairs, num_pairs, "axis");
    if (axis_str) {
        if (axis_str[0] == 'x' || axis_str[0] == 'X' || axis_str[0] == '0')
            axis = 0;
        else if (axis_str[0] == 'z' || axis_str[0] == 'Z' || axis_str[0] == '2')
            axis = 2;
    }

    /* Store closed angles */
    VectorCopy(ent->s.angles, ent->moveinfo.start_origin);

    /* Calculate open angles */
    VectorCopy(ent->s.angles, ent->moveinfo.end_origin);
    ent->moveinfo.end_origin[axis] += distance;

    ent->moveinfo.speed = ent->speed;
    ent->moveinfo.wait = ent->wait ? ent->wait : 3.0f;
    ent->moveinfo.state = MSTATE_BOTTOM;

    if (!ent->targetname)
        ent->touch = rot_door_touch;
    ent->use = rot_door_use;

    gi.linkentity(ent);
    gi.dprintf("  func_door_rotating '%s' dist=%.0f axis=%d\n",
               ent->targetname ? ent->targetname : "(auto)", distance, axis);
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

/* Platform blocked — crush damage when player caught */
static void plat_blocked(edict_t *self, edict_t *other)
{
    if (!other) return;

    /* Deal crush damage */
    if (other->takedamage && other->health > 0) {
        other->health -= 10;
        if (other->client)
            other->client->pers_health = other->health;
        if (other->health <= 0 && other->die)
            other->die(other, self, self, 10, other->s.origin);
    }
}

/* Platform wait at bottom, then return to top */
static void plat_wait_return(edict_t *self)
{
    plat_go_up(self);
}

static void plat_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;
    if (!other || !other->client) return;

    if (self->moveinfo.state == MSTATE_TOP) {
        /* Player touches top of plat — descend */
        plat_go_down(self);
    } else if (self->moveinfo.state == MSTATE_BOTTOM) {
        /* At bottom — schedule return with wait time */
        if (self->wait > 0) {
            self->nextthink = level.time + self->wait;
            self->think = plat_wait_return;
        } else {
            plat_go_up(self);
        }
    }
}

static void SP_func_plat(edict_t *ent, epair_t *pairs, int num_pairs)
{
    float height, lip;
    const char *height_str, *lip_str, *wait_str;

    door_precache_sounds();

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;

    if (!ent->speed)
        ent->speed = 200;

    /* Lip: how much of the platform sticks up at the bottom position */
    lip_str = ED_FindValue(pairs, num_pairs, "lip");
    lip = lip_str ? (float)atof(lip_str) : 8;

    /* Wait: time at bottom before auto-return (0 = instant) */
    wait_str = ED_FindValue(pairs, num_pairs, "wait");
    ent->wait = wait_str ? (float)atof(wait_str) : 3.0f;

    /* Platforms descend by their height minus lip */
    height_str = ED_FindValue(pairs, num_pairs, "height");
    height = height_str ? (float)atof(height_str) : (ent->size[2] - lip);
    if (height < 0) height = 0;

    /* Start at top */
    VectorCopy(ent->s.origin, ent->moveinfo.start_origin);
    VectorCopy(ent->s.origin, ent->moveinfo.end_origin);
    ent->moveinfo.end_origin[2] -= height;

    ent->moveinfo.speed = ent->speed;
    ent->moveinfo.state = MSTATE_TOP;

    ent->touch = plat_touch;
    ent->blocked = plat_blocked;
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

    /* Debounce — use 'wait' key or default 1 second */
    if (self->dmg_debounce_time > level.time)
        return;
    self->dmg_debounce_time = level.time + (self->wait ? self->wait : 1.0f);

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
    const char *dmg_str, *wait_str;

    ent->solid = SOLID_TRIGGER;
    ent->touch = trigger_hurt_touch;

    /* Parse damage amount */
    dmg_str = ED_FindValue(pairs, num_pairs, "dmg");
    ent->dmg = dmg_str ? atoi(dmg_str) : 5;

    /* Parse damage interval (default 1.0s) */
    wait_str = ED_FindValue(pairs, num_pairs, "wait");
    ent->wait = wait_str ? (float)atof(wait_str) : 1.0f;

    gi.linkentity(ent);
}

/* ==========================================================================
   env_lava / env_acid — Hazard zones with visual effects
   ========================================================================== */

static void lava_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    int damage;
    (void)plane; (void)surf;

    if (!other || !other->inuse || other->health <= 0 || !other->takedamage)
        return;
    if (self->dmg_debounce_time > level.time)
        return;
    self->dmg_debounce_time = level.time + 0.5f;

    damage = self->dmg ? self->dmg : 15;
    other->health -= damage;

    /* Orange-red damage flash and fire particles on victim */
    if (other->client) {
        other->client->blend[0] = 1.0f;
        other->client->blend[1] = 0.3f;
        other->client->blend[2] = 0.0f;
        other->client->blend[3] = 0.4f;
        other->client->pers_health = other->health;
    }
    R_ParticleEffect(other->s.origin, other->mins, 4, 6);  /* flame particles */

    if (other->health <= 0 && other->die)
        other->die(other, self, self, damage, other->s.origin);
}

static void SP_env_lava(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *dmg_str = ED_FindValue(pairs, num_pairs, "dmg");
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = lava_touch;
    ent->dmg = dmg_str ? atoi(dmg_str) : 15;

    gi.linkentity(ent);
    gi.dprintf("  env_lava at (%.0f %.0f %.0f) dmg=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->dmg);
}

static void acid_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    int damage;
    (void)plane; (void)surf;

    if (!other || !other->inuse || other->health <= 0 || !other->takedamage)
        return;
    if (self->dmg_debounce_time > level.time)
        return;
    self->dmg_debounce_time = level.time + 0.8f;

    damage = self->dmg ? self->dmg : 10;
    other->health -= damage;

    /* Green damage flash and steam particles on victim */
    if (other->client) {
        other->client->blend[0] = 0.2f;
        other->client->blend[1] = 1.0f;
        other->client->blend[2] = 0.0f;
        other->client->blend[3] = 0.35f;
        other->client->pers_health = other->health;

        /* Apply poison DoT — 5 seconds of lingering damage */
        other->client->poison_end = level.time + 5.0f;
        other->client->poison_next_tick = level.time + 0.8f;
    }
    R_ParticleEffect(other->s.origin, other->mins, 7, 4);  /* steam/bubble particles */

    if (other->health <= 0 && other->die)
        other->die(other, self, self, damage, other->s.origin);
}

static void SP_env_acid(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *dmg_str = ED_FindValue(pairs, num_pairs, "dmg");
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = acid_touch;
    ent->dmg = dmg_str ? atoi(dmg_str) : 10;

    gi.linkentity(ent);
    gi.dprintf("  env_acid at (%.0f %.0f %.0f) dmg=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->dmg);
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

/* Forward declaration for level transition */
extern void G_SaveTransitionState(void);
extern void SCR_BeginIntermission(const char *nextmap);

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

    /* Autosave checkpoint before level transition */
    {
        extern void WriteGame(const char *filename, qboolean autosave);
        extern void WriteLevel(const char *filename);
        WriteGame("autosave.sav", qtrue);
        WriteLevel("autosave.sv2");
        gi.dprintf("Autosave checkpoint created\n");
    }

    /* Save player state for level transition */
    G_SaveTransitionState();

    /* Show intermission stats screen before loading next map */
    if (self->target) {
        gi.dprintf("trigger_changelevel: intermission -> %s\n", self->target);
        SCR_BeginIntermission(self->target);
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

    /* Save player state for level transition */
    G_SaveTransitionState();

    /* Show intermission stats screen before loading next map */
    if (self->target) {
        gi.dprintf("target_changelevel: intermission -> %s\n", self->target);
        SCR_BeginIntermission(self->target);
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

/* ==========================================================================
   func_debris — Breakable that spawns flying physics chunks on destruction
   ========================================================================== */

static void debris_chunk_think(edict_t *self)
{
    /* Remove chunk after its lifetime expires */
    if (level.time > self->teleport_time) {
        self->inuse = qfalse;
        gi.unlinkentity(self);
        return;
    }
    self->nextthink = level.time + 0.5f;
}

static void debris_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                        int damage, vec3_t point)
{
    vec3_t up = {0, 0, 1};
    int chunk_count, i;

    (void)inflictor; (void)damage; (void)point;

    /* Debris particle effects */
    R_ParticleEffect(self->s.origin, up, 11, 24);  /* debris chunks */
    R_ParticleEffect(self->s.origin, up, 13, 12);  /* ground dust */
    R_AddDlight(self->s.origin, 0.8f, 0.6f, 0.3f, 200.0f, 0.3f);

    /* Spawn physical chunk entities that fly outward */
    chunk_count = self->count > 0 ? self->count : 5;
    if (chunk_count > 12) chunk_count = 12;

    for (i = 0; i < chunk_count; i++) {
        edict_t *chunk = G_AllocEdict();
        if (!chunk) break;

        chunk->classname = "debris_chunk";
        chunk->solid = SOLID_NOT;
        chunk->movetype = MOVETYPE_TOSS;
        chunk->gravity = 1.0f;
        VectorCopy(self->s.origin, chunk->s.origin);
        chunk->s.origin[2] += 8.0f;

        /* Random outward velocity */
        chunk->velocity[0] = (float)((rand() % 400) - 200);
        chunk->velocity[1] = (float)((rand() % 400) - 200);
        chunk->velocity[2] = (float)(100 + (rand() % 200));

        VectorSet(chunk->mins, -4, -4, -4);
        VectorSet(chunk->maxs, 4, 4, 4);

        chunk->think = debris_chunk_think;
        chunk->nextthink = level.time + 0.5f;
        chunk->teleport_time = level.time + 3.0f;  /* 3s lifetime */

        gi.linkentity(chunk);
    }

    /* Fire targets on destruction */
    if (self->target)
        G_UseTargets(attacker, self->target);

    /* Screen shake */
    SCR_AddScreenShake(0.3f, 0.2f);

    /* Remove the entity */
    self->takedamage = DAMAGE_NO;
    self->solid = SOLID_NOT;
    self->inuse = qfalse;
    gi.unlinkentity(self);
}

static void SP_func_debris(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *health_str = ED_FindValue(pairs, num_pairs, "health");
    const char *count_str = ED_FindValue(pairs, num_pairs, "count");

    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BSP;
    ent->movetype = MOVETYPE_PUSH;
    ent->takedamage = DAMAGE_YES;
    ent->health = health_str ? atoi(health_str) : 30;
    ent->max_health = ent->health;
    ent->count = count_str ? atoi(count_str) : 5;
    ent->die = debris_die;

    gi.linkentity(ent);
    gi.dprintf("  func_debris at (%.0f %.0f %.0f) hp=%d chunks=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->health, ent->count);
}

/* ==========================================================================
   func_rope / func_zipline — Grab and slide along a line between two points
   Player touches the trigger volume, gets pulled toward the target endpoint.
   ========================================================================== */

static void rope_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    vec3_t dir;
    float dist, slide_speed;

    (void)plane; (void)surf;

    if (!other || !other->client || other->health <= 0)
        return;

    /* Only grab if jumping/airborne or pressing USE */
    if (other->groundentity && !(other->client->ps.pm_flags & 0))
        return;

    /* Compute direction from current position toward the endpoint */
    VectorSubtract(self->move_origin, other->s.origin, dir);
    dist = VectorLength(dir);

    if (dist < 32.0f) {
        /* Reached the end — release */
        return;
    }

    VectorNormalize(dir);
    slide_speed = self->speed > 0 ? self->speed : 300.0f;

    /* Pull player along the rope toward the endpoint */
    other->velocity[0] = dir[0] * slide_speed;
    other->velocity[1] = dir[1] * slide_speed;
    other->velocity[2] = dir[2] * slide_speed * 0.5f;  /* less vertical pull */

    /* Prevent falling while on rope */
    other->groundentity = self;
}

static void SP_func_rope(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *speed_str = ED_FindValue(pairs, num_pairs, "speed");
    edict_t *endpoint;

    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = rope_touch;
    ent->speed = speed_str ? (float)atof(speed_str) : 300.0f;

    /* Find the target endpoint — store its position */
    VectorCopy(ent->s.origin, ent->move_origin);  /* default: end = start */
    if (ent->target) {
        endpoint = G_FindByTargetname(ent->target);
        if (endpoint)
            VectorCopy(endpoint->s.origin, ent->move_origin);
    }

    gi.linkentity(ent);
    gi.dprintf("  func_rope at (%.0f %.0f %.0f) speed=%.0f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->speed);
}

/* ==========================================================================
   misc_corpse — Environmental dead body for storytelling
   Can be pushed/shot, spawns blood on damage, gibs on overkill.
   ========================================================================== */

static void corpse_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                        int damage, vec3_t point)
{
    vec3_t up = {0, 0, 1};
    (void)inflictor; (void)attacker;

    /* Gib the corpse */
    R_ParticleEffect(point ? point : self->s.origin, up, 1, 16);
    R_ParticleEffect(self->s.origin, up, 11, 8);  /* debris chunks */
    R_ParticleEffect(self->s.origin, up, 10, 6);  /* smoke */

    self->takedamage = DAMAGE_NO;
    self->solid = SOLID_NOT;
    self->inuse = qfalse;
    gi.unlinkentity(self);
}

static void corpse_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    vec3_t up = {0, 0, 1};
    (void)other; (void)kick;

    /* Blood spray when shot */
    R_ParticleEffect(self->s.origin, up, 1, 4 + damage / 5);
}

static void SP_misc_corpse(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *frame_str = ED_FindValue(pairs, num_pairs, "frame");
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_TOSS;  /* affected by gravity */
    ent->takedamage = DAMAGE_YES;
    ent->health = 20;
    ent->max_health = 20;
    ent->die = corpse_die;
    ent->pain = corpse_pain;

    /* Death pose frame */
    ent->s.frame = frame_str ? atoi(frame_str) : 183;  /* FRAME_DEATH1_END */

    VectorSet(ent->mins, -16, -16, -8);
    VectorSet(ent->maxs, 16, 16, 8);

    gi.linkentity(ent);
    gi.dprintf("  misc_corpse at (%.0f %.0f %.0f) frame=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->s.frame);
}

/* ==========================================================================
   info_checkpoint — Auto-saves when player walks through
   ========================================================================== */

static void checkpoint_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client || self->count)
        return;

    self->count = 1;  /* only trigger once */

    /* Auto-save */
    {
        extern void WriteGame(const char *filename, qboolean autosave);
        extern void WriteLevel(const char *filename);
        WriteGame("checkpoint.sav", qtrue);
        WriteLevel("checkpoint.sv2");
    }

    SCR_AddPickupMessage("CHECKPOINT");
    gi.dprintf("Checkpoint reached at (%.0f %.0f %.0f)\n",
               other->s.origin[0], other->s.origin[1], other->s.origin[2]);

    /* Fire targets if any */
    if (self->target)
        G_UseTargets(other, self->target);
}

static void SP_info_checkpoint(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = checkpoint_touch;
    ent->count = 0;

    VectorSet(ent->mins, -32, -32, -32);
    VectorSet(ent->maxs, 32, 32, 32);

    gi.linkentity(ent);
    gi.dprintf("  info_checkpoint at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   trap_pressure_plate — Steps on it to fire targets (like trigger_once but
   visible and with a click sound + visual depression)
   ========================================================================== */

static void pressure_plate_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client || self->count)
        return;

    self->count = 1;  /* triggered — only once */

    /* Click sound */
    {
        int snd = gi.soundindex("world/switch.wav");
        if (snd)
            gi.sound(self, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }

    /* Fire targets */
    if (self->target)
        G_UseTargets(other, self->target);

    /* Visual: depress the plate slightly */
    self->s.origin[2] -= 2.0f;
    gi.linkentity(self);

    SCR_AddPickupMessage("*click*");
}

static void SP_trap_pressure_plate(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = pressure_plate_touch;
    ent->count = 0;  /* not yet triggered */

    VectorSet(ent->mins, -16, -16, -2);
    VectorSet(ent->maxs, 16, 16, 2);

    gi.linkentity(ent);
    gi.dprintf("  trap_pressure_plate at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   trap_swinging_blade — Pendulum blade that damages entities in its arc
   ========================================================================== */

static void blade_think(edict_t *self)
{
    edict_t *touch[16];
    int num, i;
    vec3_t blade_mins, blade_maxs;
    float range = self->dmg_radius > 0 ? (float)self->dmg_radius : 48.0f;

    if (!self->inuse) return;
    self->nextthink = level.time + 0.2f;

    /* Swing animation — oscillate yaw angle */
    self->s.angles[2] = sinf(level.time * self->speed) * 45.0f;

    /* Check for entities within blade reach */
    VectorSet(blade_mins, self->s.origin[0] - range,
                           self->s.origin[1] - range,
                           self->s.origin[2] - range);
    VectorSet(blade_maxs, self->s.origin[0] + range,
                           self->s.origin[1] + range,
                           self->s.origin[2] + range);

    num = gi.BoxEdicts(blade_mins, blade_maxs, touch, 16, AREA_SOLID);
    for (i = 0; i < num; i++) {
        if (touch[i] && touch[i] != self && touch[i]->inuse &&
            touch[i]->takedamage && touch[i]->health > 0) {
            /* Damage debounce per victim */
            if (self->dmg_debounce_time > level.time)
                continue;
            self->dmg_debounce_time = level.time + 0.8f;

            touch[i]->health -= self->dmg;
            R_ParticleEffect(touch[i]->s.origin, touch[i]->mins, 1, 8);

            if (touch[i]->client) {
                touch[i]->client->blend[0] = 1.0f;
                touch[i]->client->blend[1] = 0.0f;
                touch[i]->client->blend[2] = 0.0f;
                touch[i]->client->blend[3] = 0.3f;
                touch[i]->client->pers_health = touch[i]->health;
                touch[i]->client->kick_angles[0] -= 3.0f;
            }

            if (touch[i]->health <= 0 && touch[i]->die)
                touch[i]->die(touch[i], self, self, self->dmg, touch[i]->s.origin);
        }
    }
}

static void SP_trap_swinging_blade(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *dmg_str = ED_FindValue(pairs, num_pairs, "dmg");
    const char *speed_str = ED_FindValue(pairs, num_pairs, "speed");
    const char *radius_str = ED_FindValue(pairs, num_pairs, "dmg_radius");
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_NOT;  /* blade itself is non-solid */
    ent->movetype = MOVETYPE_NONE;
    ent->dmg = dmg_str ? atoi(dmg_str) : 25;
    ent->dmg_radius = radius_str ? atoi(radius_str) : 48;
    ent->speed = speed_str ? (float)atof(speed_str) : 3.0f;
    ent->think = blade_think;
    ent->nextthink = level.time + 0.5f;

    gi.linkentity(ent);
    gi.dprintf("  trap_swinging_blade at (%.0f %.0f %.0f) dmg=%d speed=%.1f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->dmg, ent->speed);
}

static void explosive_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                          int damage, vec3_t point)
{
    vec3_t up = {0, 0, 1};

    (void)inflictor; (void)damage; (void)point;

    /* Explosion particles and light */
    R_ParticleEffect(self->s.origin, up, 2, 32);     /* fire burst */
    R_ParticleEffect(self->s.origin, up, 11, 16);    /* debris chunks */
    R_ParticleEffect(self->s.origin, up, 10, 8);     /* smoke cloud */
    R_ParticleEffect(self->s.origin, up, 13, 10);    /* ground dust */
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

    /* Distance-based screen shake for nearby barrel explosions */
    {
        extern game_export_t globals;
        edict_t *player = &globals.edicts[1];
        if (player && player->inuse && player->client) {
            vec3_t diff;
            float dist, scale;
            VectorSubtract(player->s.origin, self->s.origin, diff);
            dist = VectorLength(diff);
            if (dist < 384.0f) {
                scale = 1.0f - (dist / 384.0f);
                SCR_AddScreenShake(0.7f * scale, 0.35f * scale);
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
   misc_explobox_big — Large explosive barrel (freestanding, not BSP brush)
   Health 200, damage 200, radius 400
   ========================================================================== */

static void SP_misc_explobox_big(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *health_str = ED_FindValue(pairs, num_pairs, "health");
    const char *dmg_str = ED_FindValue(pairs, num_pairs, "dmg");

    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->takedamage = DAMAGE_YES;
    ent->health = health_str ? atoi(health_str) : 200;
    ent->max_health = ent->health;
    ent->dmg = dmg_str ? atoi(dmg_str) : 200;
    ent->dmg_radius = 400;
    ent->die = explosive_die;

    VectorSet(ent->mins, -16, -16, 0);
    VectorSet(ent->maxs, 16, 16, 40);

    if (ent->model)
        ent->s.modelindex = gi.modelindex(ent->model);
    else
        ent->s.modelindex = gi.modelindex("models/objects/barrels/tris.md2");

    gi.linkentity(ent);
}

/* ==========================================================================
   func_pushable — Player-pushable crate or barrel
   Slides when player walks into it; affected by gravity.
   Keys: health (0=indestructible), mass (default 100), model
   ========================================================================== */

static void pushable_touch(edict_t *self, edict_t *other,
                            void *plane, csurface_t *surf)
{
    vec3_t push_dir;
    float push_speed;
    float mass_factor;

    (void)plane; (void)surf;

    if (!other || !other->client)
        return;  /* only players push */

    /* Direction from player to pushable */
    VectorSubtract(self->s.origin, other->s.origin, push_dir);
    push_dir[2] = 0;  /* horizontal only */
    VectorNormalize(push_dir);

    /* Mass-based resistance: heavier = slower push */
    mass_factor = 100.0f / (float)(self->mass > 0 ? self->mass : 100);
    if (mass_factor > 1.0f) mass_factor = 1.0f;

    /* Push speed based on player velocity toward the pushable */
    push_speed = (other->velocity[0] * push_dir[0] +
                  other->velocity[1] * push_dir[1]);
    if (push_speed < 50.0f)
        return;  /* not pushing hard enough */

    push_speed *= 0.5f * mass_factor;  /* dampen */
    if (push_speed > 200.0f) push_speed = 200.0f;

    self->velocity[0] = push_dir[0] * push_speed;
    self->velocity[1] = push_dir[1] * push_speed;
}

static void SP_func_pushable(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *health_str = ED_FindValue(pairs, num_pairs, "health");
    const char *mass_str = ED_FindValue(pairs, num_pairs, "mass");

    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_TOSS;
    ent->touch = pushable_touch;
    ent->mass = mass_str ? atoi(mass_str) : 100;

    VectorSet(ent->mins, -16, -16, 0);
    VectorSet(ent->maxs, 16, 16, 32);

    if (health_str && atoi(health_str) > 0) {
        ent->takedamage = DAMAGE_YES;
        ent->health = atoi(health_str);
        ent->max_health = ent->health;
        ent->die = explosive_die;
    }

    if (ent->model)
        ent->s.modelindex = gi.modelindex(ent->model);
    else
        ent->s.modelindex = gi.modelindex("models/objects/crate/tris.md2");

    gi.linkentity(ent);
}

/* ==========================================================================
   target_splash — Spawn splash particle effect when triggered
   Spawnflags control splash type (color), count = particle count
   ========================================================================== */

static void target_splash_use(edict_t *self, edict_t *other, edict_t *activator)
{
    vec3_t up = {0, 0, 1};

    (void)other; (void)activator;

    R_ParticleEffect(self->s.origin, up, self->style, self->count);

    if (self->noise_index)
        gi.positioned_sound(self->s.origin, NULL, CHAN_AUTO,
                            self->noise_index, 1.0f, ATTN_NORM, 0);
}

static void SP_target_splash(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *count_str = ED_FindValue(pairs, num_pairs, "count");
    const char *style_str = ED_FindValue(pairs, num_pairs, "style");
    const char *sound_str = ED_FindValue(pairs, num_pairs, "noise");

    ent->use = target_splash_use;
    ent->count = count_str ? atoi(count_str) : 16;
    ent->style = style_str ? atoi(style_str) : 2;  /* default: explosion particles */

    if (sound_str)
        ent->noise_index = gi.soundindex(sound_str);
}

/* ==========================================================================
   Environmental Particle Emitters — Drips, sparks, steam, dust
   ========================================================================== */

/*
 * env_emitter_think — Periodically emit particles based on entity's style
 * style: 6=water drip, 7=steam, 8=spark, 9=dust
 * count: particles per emission
 * wait: seconds between emissions (stored in wait field)
 */
static void env_emitter_think(edict_t *self)
{
    vec3_t dir;
    VectorCopy(self->s.angles, dir);
    if (VectorLength(dir) < 0.1f)
        VectorSet(dir, 0, 0, -1);  /* default: downward */

    R_ParticleEffect(self->s.origin, dir, self->style, self->count);

    self->nextthink = level.time + self->wait;
}

static void SP_env_drip(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *count_str = ED_FindValue(pairs, num_pairs, "count");
    const char *wait_str = ED_FindValue(pairs, num_pairs, "wait");

    ent->style = 6;  /* water drip particles */
    ent->count = count_str ? atoi(count_str) : 2;
    ent->wait = wait_str ? (float)atof(wait_str) : 0.3f;
    ent->think = env_emitter_think;
    ent->nextthink = level.time + (float)(rand() % 100) * 0.01f;  /* stagger */
}

static void SP_env_steam(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *count_str = ED_FindValue(pairs, num_pairs, "count");
    const char *wait_str = ED_FindValue(pairs, num_pairs, "wait");

    ent->style = 7;  /* steam particles */
    ent->count = count_str ? atoi(count_str) : 4;
    ent->wait = wait_str ? (float)atof(wait_str) : 0.2f;
    ent->think = env_emitter_think;
    ent->nextthink = level.time + (float)(rand() % 100) * 0.01f;
}

static void SP_env_sparks(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *count_str = ED_FindValue(pairs, num_pairs, "count");
    const char *wait_str = ED_FindValue(pairs, num_pairs, "wait");

    ent->style = 8;  /* spark particles */
    ent->count = count_str ? atoi(count_str) : 6;
    ent->wait = wait_str ? (float)atof(wait_str) : 1.5f;
    ent->think = env_emitter_think;
    ent->nextthink = level.time + (float)(rand() % 200) * 0.01f;
}

static void SP_env_dust(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *count_str = ED_FindValue(pairs, num_pairs, "count");
    const char *wait_str = ED_FindValue(pairs, num_pairs, "wait");

    ent->style = 9;  /* dust mote particles */
    ent->count = count_str ? atoi(count_str) : 3;
    ent->wait = wait_str ? (float)atof(wait_str) : 2.0f;
    ent->think = env_emitter_think;
    ent->nextthink = level.time + (float)(rand() % 300) * 0.01f;
}

/* ==========================================================================
   trigger_secret — Counts as a found secret when touched
   ========================================================================== */

extern level_t level;

static void trigger_secret_touch(edict_t *self, edict_t *other,
                                  void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;
    if (!other || !other->client) return;
    if (self->count) return;  /* already triggered */

    self->count = 1;
    level.found_secrets++;

    gi.centerprintf(other, "You found a secret area!");
    gi.cprintf(other, PRINT_ALL, "Secret %d / %d\n",
               level.found_secrets, level.total_secrets);

    /* Fire targets if any */
    if (self->target)
        G_UseTargets(other, self->target);
    if (self->message)
        gi.centerprintf(other, "%s", self->message);
}

static void SP_trigger_secret(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = trigger_secret_touch;
    ent->count = 0;  /* not yet found */

    gi.linkentity(ent);
}

/* ==========================================================================
   func_ladder — Ladder brush for climbing
   BSP brush that sets CONTENTS_LADDER so player can climb
   ========================================================================== */

static void ladder_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)self; (void)plane; (void)surf;
    if (other && other->client)
        other->client->on_ladder = qtrue;
}

static void SP_func_ladder(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_TRIGGER;  /* trigger volume — player walks into it */
    ent->svflags |= SVF_NOCLIENT;  /* invisible */
    ent->touch = ladder_touch;

    gi.linkentity(ent);
    gi.dprintf("  func_ladder\n");
}

/* ==========================================================================
   func_glass — Breakable glass brush with shatter effects
   Health: default 10 (very fragile)
   On break: glass shard particles, break sound, fire target chain
   ========================================================================== */

static int snd_glass_break;
static qboolean glass_sounds_cached;

static void glass_precache_sounds(void)
{
    if (glass_sounds_cached) return;
    snd_glass_break = gi.soundindex("world/glass_break.wav");
    glass_sounds_cached = qtrue;
}

static void glass_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                       int damage, vec3_t point)
{
    vec3_t center, up;

    (void)inflictor; (void)damage;

    /* Calculate center of glass */
    center[0] = (self->absmin[0] + self->absmax[0]) * 0.5f;
    center[1] = (self->absmin[1] + self->absmax[1]) * 0.5f;
    center[2] = (self->absmin[2] + self->absmax[2]) * 0.5f;
    VectorSet(up, 0, 0, 1);

    /* Glass shard particle burst */
    R_ParticleEffect(center, up, 5, 32);
    R_AddDlight(center, 0.8f, 0.8f, 1.0f, 200.0f, 0.2f);

    /* Break sound */
    if (snd_glass_break)
        gi.positioned_sound(center, NULL, CHAN_AUTO,
                            snd_glass_break, 1.0f, ATTN_NORM, 0);

    /* Fire target chain */
    if (self->target)
        G_UseTargets(attacker, self->target);

    /* Remove glass */
    self->solid = SOLID_NOT;
    self->svflags |= SVF_NOCLIENT;
    self->takedamage = DAMAGE_NO;
    self->inuse = qfalse;
    gi.unlinkentity(self);
}

static void SP_func_glass(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *health_str = ED_FindValue(pairs, num_pairs, "health");

    glass_precache_sounds();

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;
    ent->takedamage = DAMAGE_YES;
    ent->health = health_str ? atoi(health_str) : 10;
    ent->max_health = ent->health;
    ent->die = glass_die;

    gi.linkentity(ent);
}

/* ==========================================================================
   func_conveyor — BSP brush that pushes touching entities
   speed = conveyor speed (default 100)
   angle = push direction
   Spawnflags: 1 = start off (toggleable via use)
   ========================================================================== */

static void conveyor_touch(edict_t *self, edict_t *other,
                            void *plane, csurface_t *surf)
{
    float angle, push_speed;
    (void)plane; (void)surf;

    if (!other) return;
    if (self->count == 0) return;  /* currently off */

    angle = self->s.angles[1] * (3.14159265f / 180.0f);
    push_speed = self->speed;

    other->velocity[0] += (float)cos(angle) * push_speed * 0.1f;
    other->velocity[1] += (float)sin(angle) * push_speed * 0.1f;
}

static void conveyor_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;
    self->count = !self->count;  /* toggle on/off */
}

static void SP_func_conveyor_real(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *sf_str = ED_FindValue(pairs, num_pairs, "spawnflags");
    int sf = sf_str ? atoi(sf_str) : 0;

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;
    ent->touch = conveyor_touch;
    ent->use = conveyor_use;

    if (!ent->speed)
        ent->speed = 100;

    /* Spawnflag 1 = start off */
    ent->count = (sf & 1) ? 0 : 1;

    gi.linkentity(ent);
}

/* ==========================================================================
   func_water — Water volume brush
   Entities inside this brush experience water physics.
   Handled by engine content detection, but we set it up for visibility.
   ========================================================================== */

static void water_touch(edict_t *self, edict_t *other,
                         void *plane, csurface_t *surf)
{
    (void)self; (void)plane; (void)surf;

    if (!other || !other->client) return;

    /* Blue water tint when player enters */
    if (other->client->blend[3] < 0.1f) {
        other->client->blend[0] = 0.0f;
        other->client->blend[1] = 0.1f;
        other->client->blend[2] = 0.4f;
        other->client->blend[3] = 0.15f;
    }
}

static void SP_func_water(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;
    ent->touch = water_touch;

    /* Mark as non-solid trigger volume so entities pass through */
    ent->solid = SOLID_TRIGGER;

    gi.linkentity(ent);
}

/* ==========================================================================
   func_pendulum — Swinging brush entity
   speed = swing speed (default 30)
   dmg = damage on contact (0 = none)
   ========================================================================== */

static void pendulum_think(edict_t *self)
{
    float phase = level.time * self->speed * (3.14159265f / 180.0f);
    float swing = (float)sin(phase) * self->count;  /* count = max angle */

    self->avelocity[2] = swing - self->s.angles[2];
    self->s.angles[2] = swing;

    self->nextthink = level.time + level.frametime;
}

static void pendulum_blocked(edict_t *self, edict_t *other)
{
    if (!other || self->dmg <= 0) return;
    if (!other->takedamage || other->health <= 0) return;

    other->health -= self->dmg;
    if (other->client)
        other->client->pers_health = other->health;
    if (other->health <= 0 && other->die)
        other->die(other, self, self, self->dmg, other->s.origin);
}

static void SP_func_pendulum(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *dmg_str = ED_FindValue(pairs, num_pairs, "dmg");
    const char *angle_str = ED_FindValue(pairs, num_pairs, "count");

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;

    if (!ent->speed)
        ent->speed = 30;

    ent->dmg = dmg_str ? atoi(dmg_str) : 0;
    ent->count = angle_str ? atoi(angle_str) : 30;  /* max swing angle */

    ent->think = pendulum_think;
    ent->nextthink = level.time + level.frametime;
    ent->blocked = pendulum_blocked;

    gi.linkentity(ent);
}

/* ==========================================================================
   info_npc — NPC that displays dialogue when player uses them
   message = dialogue text (semicolons separate pages)
   count = tracks current dialogue page
   ========================================================================== */

static void npc_use(edict_t *self, edict_t *other, edict_t *activator)
{
    const char *msg;
    char page[256];
    int page_num, cur_page, i, start, len;

    (void)other;

    if (!self->message || !activator || !activator->client)
        return;

    msg = self->message;
    cur_page = self->count;  /* current page index */

    /* Find the current page (pages separated by semicolons) */
    page_num = 0;
    start = 0;
    for (i = 0; msg[i]; i++) {
        if (msg[i] == ';') {
            if (page_num == cur_page) {
                len = i - start;
                if (len > 255) len = 255;
                memcpy(page, msg + start, len);
                page[len] = '\0';
                break;
            }
            page_num++;
            start = i + 1;
        }
    }

    /* If we didn't find a semicolon, use the rest of the string */
    if (!msg[i]) {
        if (page_num == cur_page) {
            len = i - start;
            if (len > 255) len = 255;
            memcpy(page, msg + start, len);
            page[len] = '\0';
        } else {
            /* Past last page — fire target and reset */
            if (self->target)
                G_UseTargets(activator, self->target);
            self->count = 0;
            return;
        }
    }

    /* Display current page */
    gi.centerprintf(activator, "%s", page);

    /* Advance to next page */
    self->count++;
}

static void SP_info_npc(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->takedamage = DAMAGE_NO;
    ent->use = npc_use;
    ent->count = 0;  /* start at first dialogue page */

    VectorSet(ent->mins, -16, -16, -24);
    VectorSet(ent->maxs, 16, 16, 32);

    if (ent->model)
        ent->s.modelindex = gi.modelindex(ent->model);

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
   target_objective — Sets/updates a mission objective when triggered
   Keys:
     "message"  — objective text
     "count"    — objective slot index (0-3)
   ========================================================================== */

extern void SCR_SetObjective(int index, const char *text);
extern void SCR_ClearObjectives(void);

static void target_objective_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;

    if (self->message && self->message[0])
        SCR_SetObjective(self->count, self->message);
    else
        SCR_SetObjective(self->count, NULL);  /* clear this slot */
}

static void SP_target_objective(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *msg = ED_FindValue(pairs, num_pairs, "message");
    const char *idx = ED_FindValue(pairs, num_pairs, "count");

    if (msg) {
        int len = (int)strlen(msg) + 1;
        ent->message = (char *)gi.TagMalloc(len, Z_TAG_GAME);
        memcpy(ent->message, msg, len);
    }

    ent->count = idx ? atoi(idx) : 0;
    if (ent->count < 0) ent->count = 0;
    if (ent->count > 3) ent->count = 3;

    ent->use = target_objective_use;
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

/* ==========================================================================
   path_corner — Waypoint for func_train and patrol paths
   ========================================================================== */

static void SP_path_corner(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;
    /* path_corner is just a point entity with targetname and target */
    /* No solid, no think — just data */
}

/* ==========================================================================
   func_train — BSP brush that follows path_corner chain
   ========================================================================== */

static void train_next(edict_t *self);

static void train_arrived(edict_t *self)
{
    /* Fire targets at this path_corner */
    if (self->enemy && self->enemy->target)
        G_UseTargets(self, self->enemy->target);

    /* Wait, then move to next corner */
    if (self->enemy && self->enemy->wait > 0) {
        self->nextthink = level.time + self->enemy->wait;
        self->think = train_next;
    } else {
        /* No wait, immediately proceed */
        self->nextthink = level.time + level.frametime;
        self->think = train_next;
    }
}

static void train_move_think(edict_t *self)
{
    vec3_t delta;
    float dist;

    if (!self->enemy) return;

    VectorSubtract(self->enemy->s.origin, self->s.origin, delta);
    dist = VectorLength(delta);

    if (dist < 8.0f) {
        /* Arrived at destination */
        VectorCopy(self->enemy->s.origin, self->s.origin);
        VectorClear(self->velocity);
        gi.linkentity(self);
        train_arrived(self);
        return;
    }

    /* Continue moving toward target */
    {
        float move_dist = self->speed * level.frametime;
        if (move_dist > dist) move_dist = dist;
        VectorScale(delta, move_dist / dist, self->velocity);
        VectorMA(self->s.origin, level.frametime, self->velocity, self->s.origin);
    }

    gi.linkentity(self);
    self->nextthink = level.time + level.frametime;
    self->think = train_move_think;
}

static void train_next(edict_t *self)
{
    edict_t *next;

    if (!self->enemy || !self->enemy->target) {
        /* No next path_corner — stop */
        VectorClear(self->velocity);
        return;
    }

    next = G_FindByTargetname(self->enemy->target);
    if (!next) {
        gi.dprintf("func_train: can't find path_corner '%s'\n", self->enemy->target);
        VectorClear(self->velocity);
        return;
    }

    self->enemy = next;
    train_move_think(self);
}

static void train_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;
    train_next(self);
}

static void SP_func_train(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BSP;
    ent->movetype = MOVETYPE_PUSH;

    if (!ent->speed)
        ent->speed = 100;

    ent->use = train_use;

    /* Find initial path_corner target */
    if (ent->target) {
        edict_t *first = G_FindByTargetname(ent->target);
        if (first) {
            ent->enemy = first;
            VectorCopy(first->s.origin, ent->s.origin);
            /* Auto-start: begin moving immediately */
            ent->think = train_next;
            ent->nextthink = level.time + level.frametime;
        }
    }

    gi.linkentity(ent);
}

/* ==========================================================================
   trigger_counter — Counts touches, fires target on reaching count
   ========================================================================== */

static void trigger_counter_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;

    self->count--;
    if (self->count > 0) {
        gi.cprintf(activator, PRINT_ALL, "%d more to go...\n", self->count);
        return;
    }

    /* Count reached — fire targets */
    self->activator = activator;
    G_FireTargets(self, activator);

    /* Remove self */
    self->use = NULL;
}

static void trigger_counter_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;
    if (!other || !other->client) return;
    trigger_counter_use(self, other, other);
}

static void SP_trigger_counter(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    if (!ent->count)
        ent->count = 2;

    ent->solid = SOLID_TRIGGER;
    ent->touch = trigger_counter_touch;
    ent->use = trigger_counter_use;
    gi.linkentity(ent);
}

/* ==========================================================================
   target_earthquake — Shakes player view when used
   ========================================================================== */

static void earthquake_think(edict_t *self)
{
    extern game_export_t globals;
    edict_t *player = &globals.edicts[1];

    if (level.time > self->teleport_time) {
        /* Earthquake over */
        self->think = NULL;
        self->nextthink = 0;
        return;
    }

    /* Apply random kick angles to player */
    if (player->inuse && player->client) {
        float intensity = self->speed ? self->speed : 2.0f;
        player->client->kick_angles[0] = gi.flrand(-intensity, intensity);
        player->client->kick_angles[1] = gi.flrand(-intensity, intensity);
        player->client->kick_angles[2] = gi.flrand(-intensity * 0.5f, intensity * 0.5f);
    }

    self->nextthink = level.time + level.frametime;
}

static void earthquake_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;

    float duration = self->wait ? self->wait : 3.0f;
    self->teleport_time = level.time + duration;
    self->think = earthquake_think;
    self->nextthink = level.time + level.frametime;
}

static void SP_target_earthquake(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->use = earthquake_use;
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

    /* Spawnflag 1 = looping: use CHAN_LOOP for true continuous playback */
    if (ent->style & 1) {
        if (ent->noise_index)
            gi.sound(ent, CHAN_AUTO | 0x100, ent->noise_index,
                     ent->volume, ent->attenuation, 0);
    }
}

/*
 * misc_particles — Persistent environmental particle emitter
 * Spawns particles at its origin periodically.
 *   "style" = particle type (0=sparks, 1=blood, 2=fire, 3=muzzle, 4=flame,
 *             10=smoke, 11=debris, 13=dust, 14=rain, 15=snow)
 *   "count" = particles per emission (default 4)
 *   "wait"  = seconds between emissions (default 0.5)
 *   Can be toggled on/off via use trigger
 */
static void misc_particles_think(edict_t *self)
{
    vec3_t dir;
    VectorCopy(self->move_origin, dir);
    R_ParticleEffect(self->s.origin, dir, self->style, self->count ? self->count : 4);
    self->nextthink = level.time + (self->wait > 0 ? self->wait : 0.5f);
}

static void misc_particles_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;
    if (self->think == misc_particles_think) {
        /* Turn off */
        self->think = NULL;
        self->nextthink = 0;
    } else {
        /* Turn on */
        self->think = misc_particles_think;
        self->nextthink = level.time + 0.1f;
    }
}

static void SP_misc_particles(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *dir_str = ED_FindValue(pairs, num_pairs, "movedir");

    (void)num_pairs;

    if (!ent->style) ent->style = 0;  /* default: sparks */
    if (!ent->count) ent->count = 4;

    /* Default emit direction: up */
    VectorSet(ent->move_origin, 0, 0, 1);
    if (dir_str) {
        sscanf(dir_str, "%f %f %f", &ent->move_origin[0], &ent->move_origin[1], &ent->move_origin[2]);
    }

    ent->use = misc_particles_use;

    /* Always start emitting (use trigger to toggle on/off) */
    ent->think = misc_particles_think;
    ent->nextthink = level.time + 0.5f + ((float)(rand() % 100)) * 0.01f;

    gi.linkentity(ent);
    gi.dprintf("  misc_particles type=%d count=%d\n", ent->style, ent->count);
}

static void SP_misc_model(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *mdl = ED_FindValue(pairs, num_pairs, "model");

    /* Non-interactive model placement */
    ent->solid = SOLID_NOT;
    ent->movetype = MOVETYPE_NONE;

    /* Set model if specified */
    if (mdl && mdl[0]) {
        gi.setmodel(ent, mdl);
    } else if (ent->model && ent->model[0]) {
        gi.setmodel(ent, ent->model);
    }

    gi.linkentity(ent);
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
        else if (strstr(self->classname, "shard")) give = 5;

        if (other->client->armor >= other->client->armor_max)
            return;

        other->client->armor += give;
        if (other->client->armor > other->client->armor_max)
            other->client->armor = other->client->armor_max;
    }
    /* Field pack pickup */
    else if (strstr(self->classname, "fpak") || strstr(self->classname, "field_pack")) {
        if (other->client->fpak_count >= 3)
            return;  /* max 3 field packs */
        other->client->fpak_count++;
    }
    /* Goggles pickup — refill battery */
    else if (strstr(self->classname, "goggles") || strstr(self->classname, "nightvision")) {
        other->client->goggles_battery = 100.0f;
    }
    /* Key pickup */
    else if (strstr(self->classname, "key_red") || strstr(self->classname, "key_Red")) {
        if (other->client->keys & KEY_RED) return;
        other->client->keys |= KEY_RED;
        SCR_AddPickupMessage("Red Key");
    }
    else if (strstr(self->classname, "key_blue") || strstr(self->classname, "key_Blue")) {
        if (other->client->keys & KEY_BLUE) return;
        other->client->keys |= KEY_BLUE;
        SCR_AddPickupMessage("Blue Key");
    }
    else if (strstr(self->classname, "key_silver") || strstr(self->classname, "key_Silver")) {
        if (other->client->keys & KEY_SILVER) return;
        other->client->keys |= KEY_SILVER;
        SCR_AddPickupMessage("Silver Key");
    }
    else if (strstr(self->classname, "key_gold") || strstr(self->classname, "key_Gold")) {
        if (other->client->keys & KEY_GOLD) return;
        other->client->keys |= KEY_GOLD;
        SCR_AddPickupMessage("Gold Key");
    }
    /* Ammo crate — refills all weapon ammo */
    else if (strstr(self->classname, "ammo_crate")) {
        int i;
        qboolean any_refilled = qfalse;
        for (i = 1; i < WEAP_COUNT; i++) {
            if (other->client->ammo[i] > 0 && other->client->ammo[i] < other->client->ammo_max[i]) {
                other->client->ammo[i] = other->client->ammo_max[i];
                any_refilled = qtrue;
            }
        }
        if (!any_refilled)
            return;
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

/*
 * item_bob_think — Rotate item and bob up/down
 * Classic id Tech 2 pickup animation.
 */
static void item_bob_think(edict_t *self)
{
    /* Rotate 90 degrees per second */
    self->s.angles[1] += 9.0f;     /* ~90 deg/s at 10Hz */
    if (self->s.angles[1] >= 360.0f)
        self->s.angles[1] -= 360.0f;

    /* Bob up and down using a sine wave (stored phase in speed field) */
    self->speed += 0.628f;  /* ~2*pi / 10Hz = one cycle per second */
    if (self->speed > 6.2832f)
        self->speed -= 6.2832f;

    self->s.origin[2] = self->move_origin[2] + (float)sin(self->speed) * 4.0f;

    /* Colored glow based on item type */
    if (self->classname) {
        float pulse = 0.6f + 0.4f * (float)sin(self->speed * 2.0f);
        float intensity = 80.0f * pulse;

        if (strstr(self->classname, "health")) {
            R_AddDlight(self->s.origin, 0.2f, 1.0f, 0.2f, intensity, 0.15f);
        } else if (strstr(self->classname, "armor")) {
            R_AddDlight(self->s.origin, 0.3f, 0.5f, 1.0f, intensity, 0.15f);
        } else if (strstr(self->classname, "weapon")) {
            R_AddDlight(self->s.origin, 1.0f, 0.8f, 0.2f, intensity, 0.15f);
        } else if (strstr(self->classname, "ammo")) {
            R_AddDlight(self->s.origin, 0.8f, 0.6f, 0.2f, intensity * 0.7f, 0.15f);
        } else if (strstr(self->classname, "fpak") || strstr(self->classname, "field_pack")) {
            R_AddDlight(self->s.origin, 1.0f, 1.0f, 0.5f, intensity, 0.15f);
        } else if (strstr(self->classname, "goggles") || strstr(self->classname, "nvg")) {
            R_AddDlight(self->s.origin, 0.0f, 1.0f, 0.0f, intensity, 0.15f);
        }
    }

    self->nextthink = level.time + level.frametime;
    gi.linkentity(self);
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

    /* Save base position for bob animation */
    VectorCopy(ent->s.origin, ent->move_origin);
    ent->speed = ((float)(rand() % 628)) * 0.01f;  /* random phase */

    /* Start bob/rotate animation */
    ent->think = item_bob_think;
    ent->nextthink = level.time + 0.1f + ((float)(rand() % 100)) * 0.001f;

    gi.linkentity(ent);
}

/*
 * G_DropItem — Spawn an item pickup at a world position
 * Used for monster death drops. The item bobs and can be picked up.
 */
edict_t *G_DropItem(vec3_t origin, const char *classname)
{
    edict_t *drop;

    drop = G_AllocEdict();
    if (!drop) return NULL;

    drop->classname = (char *)classname;
    VectorCopy(origin, drop->s.origin);
    drop->s.origin[2] += 16;  /* pop up slightly */

    drop->solid = SOLID_TRIGGER;
    drop->movetype = MOVETYPE_NONE;
    drop->touch = item_touch;

    VectorSet(drop->mins, -16, -16, -16);
    VectorSet(drop->maxs, 16, 16, 16);

    VectorCopy(drop->s.origin, drop->move_origin);
    drop->speed = ((float)(rand() % 628)) * 0.01f;

    drop->think = item_bob_think;
    drop->nextthink = level.time + 0.1f;

    /* In DM mode, disappear after 30s; in SP mode, stay */
    drop->wait = 0;  /* persistent */

    gi.linkentity(drop);
    return drop;
}

/* ==========================================================================
   target_monster_maker — Spawns a monster at its location when triggered
   Keys: monstertype (classname), count (max spawns, 0=infinite)
   ========================================================================== */

static void monster_maker_use(edict_t *self, edict_t *other, edict_t *activator)
{
    edict_t *monster;

    (void)other; (void)activator;

    /* Check spawn limit */
    if (self->count > 0 && self->dmg >= self->count)
        return;  /* dmg used as spawn counter */

    monster = G_AllocEdict();
    if (!monster) return;

    VectorCopy(self->s.origin, monster->s.origin);
    monster->s.origin[2] += 16;  /* lift slightly */
    VectorCopy(self->s.angles, monster->s.angles);

    /* Spawn the appropriate monster type */
    {
        const char *type = self->message ? self->message : "monster_soldier";
        if (Q_stricmp(type, "monster_soldier_ss") == 0)
            SP_monster_soldier_ss(monster, NULL, 0);
        else if (Q_stricmp(type, "monster_soldier_light") == 0)
            SP_monster_soldier_light(monster, NULL, 0);
        else if (Q_stricmp(type, "monster_guard") == 0)
            SP_monster_guard(monster, NULL, 0);
        else if (Q_stricmp(type, "monster_sniper") == 0)
            SP_monster_sniper(monster, NULL, 0);
        else if (Q_stricmp(type, "monster_boss") == 0)
            SP_monster_boss(monster, NULL, 0);
        else
            SP_monster_soldier(monster, NULL, 0);
    }

    /* Spawn particles */
    {
        vec3_t up = {0, 0, 1};
        R_ParticleEffect(monster->s.origin, up, 3, 16);
    }

    level.total_monsters++;
    self->dmg++;  /* increment spawn counter */
}

static void SP_target_monster_maker(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *type_str = ED_FindValue(pairs, num_pairs, "monstertype");
    const char *count_str = ED_FindValue(pairs, num_pairs, "count");

    ent->use = monster_maker_use;
    ent->solid = SOLID_NOT;

    if (type_str)
        ent->message = (char *)type_str;  /* reuse message field for type */

    ent->count = count_str ? atoi(count_str) : 0;
    ent->dmg = 0;  /* spawn counter */
}

/* ==========================================================================
   item_medkit — Health pickup, restores 25HP on touch, respawns after 30s
   ========================================================================== */

static void medkit_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client || other->deadflag)
        return;
    if (other->health >= 100)
        return;  /* already full health */

    other->health += 25;
    if (other->health > 100)
        other->health = 100;
    other->client->pers_health = other->health;

    /* Pickup sound */
    {
        int snd = gi.soundindex("items/health.wav");
        if (snd)
            gi.sound(other, CHAN_ITEM, snd, 1.0f, ATTN_NORM, 0);
    }

    SCR_AddPickupMessage("Medkit (+25 Health)");

    /* Hide and respawn after 30 seconds */
    self->solid = SOLID_NOT;
    self->svflags |= SVF_NOCLIENT;
    self->nextthink = level.time + 30.0f;
    gi.linkentity(self);
}

static void medkit_respawn(edict_t *self)
{
    self->solid = SOLID_TRIGGER;
    self->svflags &= ~SVF_NOCLIENT;
    self->nextthink = 0;
    gi.linkentity(self);
}

static void SP_item_medkit(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = medkit_touch;
    ent->think = medkit_respawn;
    ent->s.modelindex = gi.modelindex("models/items/medkit/tris.md2");

    VectorSet(ent->mins, -16, -16, 0);
    VectorSet(ent->maxs, 16, 16, 16);

    gi.linkentity(ent);
    gi.dprintf("  item_medkit at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   misc_security_camera — Rotating camera that alerts nearby monsters when
   it detects the player. Can be destroyed (50 HP).
   ========================================================================== */

static void camera_think(edict_t *self)
{
    extern game_export_t globals;
    edict_t *player;
    vec3_t diff, fwd;
    float dist, dot, angle;
    int i;

    self->nextthink = level.time + 0.5f;

    /* Rotate slowly */
    self->s.angles[1] += 15.0f;  /* 30 deg/sec at 2 calls/sec */
    if (self->s.angles[1] >= 360.0f)
        self->s.angles[1] -= 360.0f;

    /* Check for visible player */
    player = &globals.edicts[1];
    if (!player->inuse || !player->client || player->deadflag)
        return;

    VectorSubtract(player->s.origin, self->s.origin, diff);
    dist = VectorLength(diff);
    if (dist > 512.0f)
        return;

    /* Check if player is in camera's FOV (90 degree cone) */
    angle = self->s.angles[1] * (3.14159265f / 180.0f);
    fwd[0] = (float)cos(angle);
    fwd[1] = (float)sin(angle);
    fwd[2] = 0;
    VectorNormalize(diff);
    dot = diff[0] * fwd[0] + diff[1] * fwd[1];
    if (dot < 0.707f)  /* ~45 degree half-angle */
        return;

    /* Trace visibility */
    {
        trace_t tr = gi.trace(self->s.origin, NULL, NULL, player->s.origin,
                               self, MASK_OPAQUE);
        if (tr.fraction < 1.0f && tr.ent != player)
            return;
    }

    /* Detected! Alert nearby monsters */
    {
        int snd = gi.soundindex("world/alarm.wav");
        if (snd)
            gi.sound(self, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }

    for (i = 1; i < globals.num_edicts; i++) {
        edict_t *e = &globals.edicts[i];
        vec3_t d2;
        if (!e->inuse || e->health <= 0 || !(e->svflags & SVF_MONSTER) || e->enemy)
            continue;
        VectorSubtract(e->s.origin, self->s.origin, d2);
        if (VectorLength(d2) > 800.0f)
            continue;
        e->enemy = player;
        e->count = 2;  /* AI_STATE_ALERT */
        VectorCopy(player->s.origin, e->move_origin);
    }
}

static void camera_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                        int damage, vec3_t point)
{
    (void)inflictor; (void)attacker; (void)damage; (void)point;

    /* Spark particles */
    {
        vec3_t up = {0, 0, 1};
        R_ParticleEffect(self->s.origin, up, 3, 8);  /* sparks */
    }

    {
        int snd = gi.soundindex("world/spark.wav");
        if (snd)
            gi.sound(self, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }

    self->think = NULL;
    self->nextthink = 0;
    self->solid = SOLID_NOT;
    self->svflags |= SVF_NOCLIENT;
    gi.linkentity(self);
}

static void SP_misc_security_camera(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->health = 50;
    ent->takedamage = DAMAGE_YES;
    ent->die = camera_die;
    ent->think = camera_think;
    ent->nextthink = level.time + 1.0f;

    VectorSet(ent->mins, -8, -8, -8);
    VectorSet(ent->maxs, 8, 8, 8);

    gi.linkentity(ent);
    gi.dprintf("  misc_security_camera at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   env_smoke — Smoke/fog volume that reduces visibility and slows movement
   ========================================================================== */

static void smoke_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client || other->deadflag)
        return;

    /* Slow movement by 40% while inside smoke */
    other->velocity[0] *= 0.6f;
    other->velocity[1] *= 0.6f;

    /* Grey screen tint for obscured vision */
    other->client->blend[0] = 0.5f;
    other->client->blend[1] = 0.5f;
    other->client->blend[2] = 0.5f;
    other->client->blend[3] = 0.3f;

    /* Smoke particles on player */
    {
        vec3_t up = {0, 0, 1};
        R_ParticleEffect(other->s.origin, up, 8, 2);  /* grey/white */
    }
}

static void SP_env_smoke(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = smoke_touch;
    ent->movetype = MOVETYPE_NONE;

    VectorSet(ent->mins, -64, -64, -32);
    VectorSet(ent->maxs, 64, 64, 64);

    gi.linkentity(ent);
    gi.dprintf("  env_smoke at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   item_armor — Armor pickup, restores 50 armor on touch, respawns after 30s
   ========================================================================== */

static void armor_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client || other->deadflag)
        return;
    if (other->client->armor >= other->client->armor_max)
        return;

    other->client->armor += 50;
    if (other->client->armor > other->client->armor_max)
        other->client->armor = other->client->armor_max;

    {
        int snd = gi.soundindex("items/armor.wav");
        if (snd)
            gi.sound(other, CHAN_ITEM, snd, 1.0f, ATTN_NORM, 0);
    }

    SCR_AddPickupMessage("Armor (+50)");

    self->solid = SOLID_NOT;
    self->svflags |= SVF_NOCLIENT;
    self->nextthink = level.time + 30.0f;
    gi.linkentity(self);
}

static void armor_respawn(edict_t *self)
{
    self->solid = SOLID_TRIGGER;
    self->svflags &= ~SVF_NOCLIENT;
    self->nextthink = 0;
    gi.linkentity(self);
}

static void SP_item_armor(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = armor_touch;
    ent->think = armor_respawn;
    ent->s.modelindex = gi.modelindex("models/items/armor/tris.md2");

    VectorSet(ent->mins, -16, -16, 0);
    VectorSet(ent->maxs, 16, 16, 16);

    gi.linkentity(ent);
    gi.dprintf("  item_armor at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   item_ammo_crate — Refills current weapon ammo on touch, respawns after 45s
   ========================================================================== */

static void ammo_crate_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client || other->deadflag)
        return;

    {
        int w = other->client->pers_weapon;
        if (w <= 0 || w >= WEAP_COUNT)
            return;

        if (other->client->ammo[w] >= other->client->ammo_max[w])
            return;

        /* Refill to max */
        other->client->ammo[w] = other->client->ammo_max[w];

        {
            int snd = gi.soundindex("items/ammo.wav");
            if (snd)
                gi.sound(other, CHAN_ITEM, snd, 1.0f, ATTN_NORM, 0);
        }

        SCR_AddPickupMessage("Ammo Crate (ammo refilled)");
    }

    self->solid = SOLID_NOT;
    self->svflags |= SVF_NOCLIENT;
    self->nextthink = level.time + 45.0f;
    gi.linkentity(self);
}

static void ammo_crate_respawn(edict_t *self)
{
    self->solid = SOLID_TRIGGER;
    self->svflags &= ~SVF_NOCLIENT;
    self->nextthink = 0;
    gi.linkentity(self);
}

static void SP_item_ammo_crate(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = ammo_crate_touch;
    ent->think = ammo_crate_respawn;
    ent->s.modelindex = gi.modelindex("models/items/ammo/tris.md2");

    VectorSet(ent->mins, -16, -16, 0);
    VectorSet(ent->maxs, 16, 16, 24);

    gi.linkentity(ent);
    gi.dprintf("  item_ammo_crate at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   env_wind — Directional push zone (like trigger_push but continuous)
   Uses entity angles to determine push direction
   ========================================================================== */

static void wind_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || other->movetype == MOVETYPE_NONE)
        return;

    /* Push in the direction the wind entity faces */
    {
        float force = self->speed ? self->speed : 200.0f;
        float angle = self->s.angles[1] * (3.14159265f / 180.0f);
        float push_x = (float)cos(angle) * force * level.frametime;
        float push_y = (float)sin(angle) * force * level.frametime;

        other->velocity[0] += push_x;
        other->velocity[1] += push_y;

        /* Vertical push component from pitch */
        if (self->s.angles[0] != 0) {
            float pitch = self->s.angles[0] * (3.14159265f / 180.0f);
            other->velocity[2] += (float)sin(-pitch) * force * level.frametime;
        }
    }
}

static void SP_env_wind(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = wind_touch;
    ent->movetype = MOVETYPE_NONE;

    VectorSet(ent->mins, -64, -64, -32);
    VectorSet(ent->maxs, 64, 64, 64);

    gi.linkentity(ent);
    gi.dprintf("  env_wind at (%.0f %.0f %.0f) speed=%.0f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->speed ? ent->speed : 200.0f);
}

/* ==========================================================================
   func_door_locked — Door that requires a specific key to open and shows
   a "locked" message when used without the key
   ========================================================================== */

static void locked_door_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;

    if (!activator || !activator->client)
        return;

    /* Check if player has required key */
    if (self->count && !(activator->client->keys & self->count)) {
        /* Locked! */
        gi.cprintf(activator, PRINT_ALL, "This door is locked.\n");
        {
            int snd = gi.soundindex("world/door_locked.wav");
            if (snd)
                gi.sound(self, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
        }
        return;
    }

    /* Unlock and open — reuse standard door_use */
    self->count = 0;  /* consumed key requirement */
    door_use(self, other, activator);
}

static void SP_func_door_locked(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *key_str;

    /* Initialize as a regular door first */
    SP_func_door(ent, pairs, num_pairs);

    /* Override use function with locked version */
    key_str = ED_FindValue(pairs, num_pairs, "key");
    if (key_str) {
        if (Q_stricmp(key_str, "red") == 0) ent->count = KEY_RED;
        else if (Q_stricmp(key_str, "blue") == 0) ent->count = KEY_BLUE;
        else if (Q_stricmp(key_str, "silver") == 0) ent->count = KEY_SILVER;
        else if (Q_stricmp(key_str, "gold") == 0) ent->count = KEY_GOLD;
        else ent->count = atoi(key_str);
    } else {
        ent->count = KEY_RED;  /* default: requires red key */
    }

    ent->use = (void (*)(edict_t *, edict_t *, edict_t *))locked_door_use;

    gi.dprintf("  func_door_locked (key=%d)\n", ent->count);
}

/* ==========================================================================
   env_water_current — Underwater current that pushes entities along a
   direction. Like env_wind but also applies drag and buoyancy effects.
   ========================================================================== */

static void water_current_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || other->movetype == MOVETYPE_NONE)
        return;

    {
        float force = self->speed ? self->speed : 150.0f;
        float angle = self->s.angles[1] * (3.14159265f / 180.0f);

        other->velocity[0] += (float)cos(angle) * force * level.frametime;
        other->velocity[1] += (float)sin(angle) * force * level.frametime;

        /* Slight upward buoyancy */
        other->velocity[2] += 20.0f * level.frametime;

        /* Drag: slow existing velocity slightly */
        other->velocity[0] *= 0.98f;
        other->velocity[1] *= 0.98f;
    }

    /* Blue water tint */
    if (other->client) {
        other->client->blend[0] = 0.1f;
        other->client->blend[1] = 0.2f;
        other->client->blend[2] = 0.6f;
        other->client->blend[3] = 0.15f;
    }
}

static void SP_env_water_current(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = water_current_touch;
    ent->movetype = MOVETYPE_NONE;

    VectorSet(ent->mins, -64, -64, -32);
    VectorSet(ent->maxs, 64, 64, 32);

    gi.linkentity(ent);
    gi.dprintf("  env_water_current at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   misc_spotlight — Decorative rotating spotlight entity. Emits a visible
   light cone particle effect.
   ========================================================================== */

static void spotlight_think(edict_t *self)
{
    self->nextthink = level.time + 0.5f;

    /* Rotate beam */
    self->s.angles[1] += 10.0f;
    if (self->s.angles[1] >= 360.0f)
        self->s.angles[1] -= 360.0f;

    /* Light cone particles */
    {
        float angle = self->s.angles[1] * (3.14159265f / 180.0f);
        vec3_t beam_end;
        beam_end[0] = self->s.origin[0] + (float)cos(angle) * 256.0f;
        beam_end[1] = self->s.origin[1] + (float)sin(angle) * 256.0f;
        beam_end[2] = self->s.origin[2] - 128.0f;

        R_AddTracer(self->s.origin, beam_end, 1.0f, 1.0f, 0.8f);
    }
}

static void SP_misc_spotlight(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_NOT;
    ent->movetype = MOVETYPE_NONE;
    ent->think = spotlight_think;
    ent->nextthink = level.time + 1.0f;

    gi.linkentity(ent);
    gi.dprintf("  misc_spotlight at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   func_elevator — Vertical platform that cycles between bottom and top
   positions. Activated by use or touch, waits at each end.
   ========================================================================== */

static void elevator_go_top(edict_t *self);
static void elevator_go_bottom(edict_t *self);

static void elevator_wait_top(edict_t *self)
{
    self->think = elevator_go_bottom;
    self->nextthink = level.time + (self->wait ? self->wait : 3.0f);
}

static void elevator_wait_bottom(edict_t *self)
{
    self->moveinfo.state = MSTATE_BOTTOM;
    self->nextthink = 0;  /* wait for activation */
}

static void elevator_go_top(edict_t *self)
{
    self->moveinfo.state = MSTATE_UP;
    Move_Calc(self, self->moveinfo.end_origin, elevator_wait_top);

    {
        int snd = gi.soundindex("plats/pt1_strt.wav");
        if (snd)
            gi.sound(self, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }
}

static void elevator_go_bottom(edict_t *self)
{
    self->moveinfo.state = MSTATE_DOWN;
    Move_Calc(self, self->moveinfo.start_origin, elevator_wait_bottom);

    {
        int snd = gi.soundindex("plats/pt1_strt.wav");
        if (snd)
            gi.sound(self, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }
}

static void elevator_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;

    if (self->moveinfo.state == MSTATE_BOTTOM)
        elevator_go_top(self);
}

static void elevator_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client)
        return;
    if (self->moveinfo.state == MSTATE_BOTTOM)
        elevator_go_top(self);
}

static void SP_func_elevator(edict_t *ent, epair_t *pairs, int num_pairs)
{
    float height;
    const char *height_str;

    (void)num_pairs;

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;

    if (!ent->speed)
        ent->speed = 100;

    height_str = ED_FindValue(pairs, num_pairs, "height");
    height = height_str ? (float)atof(height_str) : 128.0f;

    VectorCopy(ent->s.origin, ent->moveinfo.start_origin);
    VectorCopy(ent->s.origin, ent->moveinfo.end_origin);
    ent->moveinfo.end_origin[2] += height;

    ent->moveinfo.speed = ent->speed;
    ent->moveinfo.state = MSTATE_BOTTOM;

    ent->use = (void (*)(edict_t *, edict_t *, edict_t *))elevator_use;
    ent->touch = elevator_touch;
    ent->blocked = door_blocked;

    gi.linkentity(ent);
    gi.dprintf("  func_elevator at (%.0f %.0f %.0f) height=%.0f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], height);
}

/* ==========================================================================
   env_fire — Persistent fire hazard that damages and ignites on contact
   ========================================================================== */

static void fire_think(edict_t *self)
{
    /* Emit flame particles periodically */
    self->nextthink = level.time + 0.3f;
    {
        vec3_t up = {0, 0, 1};
        R_ParticleEffect(self->s.origin, up, 4, 5);  /* fire color */
    }
}

static void fire_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)self; (void)plane; (void)surf;

    if (!other || !other->client || other->deadflag)
        return;

    /* Apply burn damage and set on fire */
    other->health -= 2;
    other->client->pers_health = other->health;
    other->client->burn_end = level.time + 3.0f;
    other->client->burn_next_tick = level.time + 0.5f;

    /* Orange flash */
    other->client->blend[0] = 1.0f;
    other->client->blend[1] = 0.5f;
    other->client->blend[2] = 0.0f;
    other->client->blend[3] = 0.25f;

    if (other->health <= 0) {
        other->deadflag = 1;
        other->client->ps.pm_type = PM_DEAD;
        SCR_AddKillFeed("Player", "fire", "environment");
        other->client->deaths++;
    }
}

static void SP_env_fire(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->movetype = MOVETYPE_NONE;
    ent->touch = fire_touch;
    ent->think = fire_think;
    ent->nextthink = level.time + 1.0f;

    VectorSet(ent->mins, -32, -32, 0);
    VectorSet(ent->maxs, 32, 32, 64);

    gi.linkentity(ent);
    gi.dprintf("  env_fire at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   env_electric — Electrical hazard zone (sparking wires, tesla coils)
   Periodic damage with electric stun effect
   ========================================================================== */

static void electric_think(edict_t *self)
{
    self->nextthink = level.time + 0.4f;
    /* Spark particles */
    {
        vec3_t up = {0, 0, 1};
        R_ParticleEffect(self->s.origin, up, 3, 3);  /* yellow/spark */
    }
}

static void electric_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)self; (void)plane; (void)surf;

    if (!other || other->deadflag)
        return;

    /* Damage */
    other->health -= 5;
    if (other->client) {
        other->client->pers_health = other->health;

        /* Electric blue flash */
        other->client->blend[0] = 0.3f;
        other->client->blend[1] = 0.5f;
        other->client->blend[2] = 1.0f;
        other->client->blend[3] = 0.3f;

        /* Brief stun: lock view for a moment */
        other->client->concussion_end = level.time + 0.5f;
    }

    /* Spark burst on victim */
    {
        vec3_t up = {0, 0, 1};
        R_ParticleEffect(other->s.origin, up, 3, 6);
    }

    {
        int snd = gi.soundindex("world/spark.wav");
        if (snd)
            gi.sound(other, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }

    if (other->health <= 0) {
        if (other->client) {
            other->deadflag = 1;
            other->client->ps.pm_type = PM_DEAD;
            SCR_AddKillFeed("Player", "electrocution", "environment");
            other->client->deaths++;
        } else if (other->die) {
            other->die(other, self, self, 5, other->s.origin);
        }
    }
}

static void SP_env_electric(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->movetype = MOVETYPE_NONE;
    ent->touch = electric_touch;
    ent->think = electric_think;
    ent->nextthink = level.time + 1.0f;

    VectorSet(ent->mins, -24, -24, 0);
    VectorSet(ent->maxs, 24, 24, 48);

    gi.linkentity(ent);
    gi.dprintf("  env_electric at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   trigger_objective — When player enters, marks an objective as complete
   and updates the objective display with the next objective text
   ========================================================================== */

static void objective_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client || self->count)
        return;

    self->count = 1;  /* completed — only once */

    level.objectives_completed++;

    /* Display completion message */
    {
        char msg[128];
        Com_sprintf(msg, sizeof(msg), "OBJECTIVE COMPLETE (%d/%d)",
                    level.objectives_completed, level.objectives_total);
        SCR_AddPickupMessage(msg);
    }

    {
        int snd = gi.soundindex("misc/objective.wav");
        if (snd)
            gi.sound(other, CHAN_ITEM, snd, 1.0f, ATTN_NONE, 0);
    }

    /* Update objective text if this entity has a message */
    if (self->message) {
        Com_sprintf(level.objective_text, sizeof(level.objective_text),
                    "%s", self->message);
        level.objective_active = qtrue;
    } else if (level.objectives_completed >= level.objectives_total) {
        Com_sprintf(level.objective_text, sizeof(level.objective_text),
                    "All objectives complete!");
        level.objective_active = qfalse;
    }

    /* Fire targets */
    if (self->target)
        G_UseTargets(other, self->target);
}

static void SP_trigger_objective(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = objective_touch;
    ent->count = 0;

    level.objectives_total++;

    /* Check for initial objective text */
    if (ent->message && level.objectives_completed == 0 && !level.objective_active) {
        Com_sprintf(level.objective_text, sizeof(level.objective_text),
                    "%s", ent->message);
        level.objective_active = qtrue;
    }

    VectorSet(ent->mins, -32, -32, -32);
    VectorSet(ent->maxs, 32, 32, 32);

    gi.linkentity(ent);
    gi.dprintf("  trigger_objective at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   misc_supply_crate — Breakable crate that drops health and ammo on destroy
   ========================================================================== */

static void supply_crate_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                              int damage, vec3_t point)
{
    (void)inflictor; (void)damage; (void)point;

    /* Spawn health and ammo pickups on destroy */
    {
        vec3_t up = {0, 0, 1};
        R_ParticleEffect(self->s.origin, up, 8, 10);  /* debris */
    }

    {
        int snd = gi.soundindex("world/crate_break.wav");
        if (snd)
            gi.sound(self, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }

    /* Grant health and ammo directly to attacker if player */
    if (attacker && attacker->client) {
        if (attacker->health < 100) {
            attacker->health += 15;
            if (attacker->health > 100)
                attacker->health = 100;
            attacker->client->pers_health = attacker->health;
        }

        {
            int w = attacker->client->pers_weapon;
            if (w > 0 && w < WEAP_COUNT) {
                int add = attacker->client->ammo_max[w] / 4;
                if (add < 5) add = 5;
                attacker->client->ammo[w] += add;
                if (attacker->client->ammo[w] > attacker->client->ammo_max[w])
                    attacker->client->ammo[w] = attacker->client->ammo_max[w];
            }
        }

        SCR_AddPickupMessage("Supply Crate (+15 HP, +ammo)");
    }

    self->solid = SOLID_NOT;
    self->svflags |= SVF_NOCLIENT;
    self->takedamage = DAMAGE_NO;
    gi.linkentity(self);
}

static void SP_misc_supply_crate(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->health = 30;
    ent->takedamage = DAMAGE_YES;
    ent->die = supply_crate_die;
    ent->s.modelindex = gi.modelindex("models/objects/crate/tris.md2");

    VectorSet(ent->mins, -16, -16, 0);
    VectorSet(ent->maxs, 16, 16, 24);

    gi.linkentity(ent);
    gi.dprintf("  misc_supply_crate at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   trigger_audio_zone — Plays ambient sound when player enters.
   Uses "noise" key for sound file, loops while player is inside.
   ========================================================================== */

static void audio_zone_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client)
        return;

    /* Play ambient sound — only retrigger every 2 seconds */
    if (level.time < self->wait)
        return;

    self->wait = level.time + 2.0f;

    if (self->noise_index) {
        gi.sound(other, CHAN_AUTO, self->noise_index, 0.7f, ATTN_NORM, 0);
    }
}

static void SP_trigger_audio_zone(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *noise;

    noise = ED_FindValue(pairs, num_pairs, "noise");
    if (noise)
        ent->noise_index = gi.soundindex(noise);
    else
        ent->noise_index = gi.soundindex("ambient/wind.wav");

    ent->solid = SOLID_TRIGGER;
    ent->touch = audio_zone_touch;
    ent->wait = 0;

    VectorSet(ent->mins, -128, -128, -64);
    VectorSet(ent->maxs, 128, 128, 64);

    gi.linkentity(ent);
    gi.dprintf("  trigger_audio_zone at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   item_shield — Temporary damage resistance power-up (50% reduction, 15s)
   ========================================================================== */

static void shield_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client || other->deadflag)
        return;

    other->client->shield_end = level.time + 15.0f;
    other->client->shield_mult = 0.5f;

    {
        int snd = gi.soundindex("items/powerup.wav");
        if (snd)
            gi.sound(other, CHAN_ITEM, snd, 1.0f, ATTN_NORM, 0);
    }

    SCR_AddPickupMessage("Shield Active (50% damage reduction, 15s)");

    /* Consume — don't respawn */
    self->solid = SOLID_NOT;
    self->svflags |= SVF_NOCLIENT;
    self->nextthink = 0;
    gi.linkentity(self);
}

static void SP_item_shield(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->solid = SOLID_TRIGGER;
    ent->touch = shield_touch;

    VectorSet(ent->mins, -16, -16, 0);
    VectorSet(ent->maxs, 16, 16, 24);

    gi.linkentity(ent);
    gi.dprintf("  item_shield at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   info_landmark — Map transition reference point used for level-to-level
   position preservation (pairs with trigger_changelevel landmark field)
   ========================================================================== */

static void SP_info_landmark(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;
    /* Just a point entity — no solid, no think, just targetname + origin */
    ent->solid = SOLID_NOT;
    gi.dprintf("  info_landmark '%s' at (%.0f %.0f %.0f)\n",
               ent->targetname ? ent->targetname : "(unnamed)",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   func_mirror — Reflective BSP surface (marks brush for env-map rendering)
   ========================================================================== */

static void SP_func_mirror(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;
    ent->s.renderfx |= RF_TRANSLUCENT;  /* hint to renderer: reflective surface */

    gi.linkentity(ent);

    gi.dprintf("  func_mirror at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   env_fog — Fog volume or global fog settings
   color = RGB (0-255), density = fog thickness (0.0-1.0)
   ========================================================================== */

static void env_fog_touch(edict_t *self, edict_t *other,
                           void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client)
        return;

    /* Apply fog tint to player's view blend */
    other->client->blend[0] = self->move_angles[0];  /* fog R (0-1) */
    other->client->blend[1] = self->move_angles[1];  /* fog G (0-1) */
    other->client->blend[2] = self->move_angles[2];  /* fog B (0-1) */
    other->client->blend[3] = self->speed * 0.3f;  /* density->alpha */
}

static void SP_env_fog(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *color_str = ED_FindValue(pairs, num_pairs, "color");
    const char *density_str = ED_FindValue(pairs, num_pairs, "density");

    ent->solid = SOLID_TRIGGER;
    ent->movetype = MOVETYPE_NONE;
    ent->svflags |= SVF_NOCLIENT;
    ent->touch = env_fog_touch;

    /* Parse fog color (0-255 -> 0-1) */
    if (color_str) {
        int r = 128, g = 128, b = 128;
        sscanf(color_str, "%d %d %d", &r, &g, &b);
        ent->move_angles[0] = r / 255.0f;
        ent->move_angles[1] = g / 255.0f;
        ent->move_angles[2] = b / 255.0f;
    } else {
        ent->move_angles[0] = 0.5f;
        ent->move_angles[1] = 0.5f;
        ent->move_angles[2] = 0.5f;
    }

    /* Fog density */
    ent->speed = density_str ? (float)atof(density_str) : 0.3f;
    if (ent->speed > 1.0f) ent->speed = 1.0f;
    if (ent->speed < 0.01f) ent->speed = 0.01f;

    gi.linkentity(ent);

    gi.dprintf("  env_fog at (%.0f %.0f %.0f) density=%.2f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->speed);
}

/* ==========================================================================
   func_valve — Turnable valve/wheel that fires target on use
   count = number of turns required (default 3)
   Fires target when fully turned.
   ========================================================================== */

static void valve_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;

    self->count--;

    /* Rotate the valve model 90 degrees per turn */
    self->s.angles[2] += 90.0f;
    if (self->s.angles[2] >= 360.0f)
        self->s.angles[2] -= 360.0f;

    /* Mechanical sound */
    {
        int snd = gi.soundindex("world/valve_turn.wav");
        if (snd)
            gi.positioned_sound(self->s.origin, self, CHAN_AUTO,
                                snd, 1.0f, ATTN_NORM, 0);
    }

    if (self->count <= 0) {
        /* Valve fully opened — fire target */
        if (self->target)
            G_UseTargets(activator, self->target);
        gi.dprintf("  Valve opened at (%.0f %.0f %.0f)\n",
                   self->s.origin[0], self->s.origin[1], self->s.origin[2]);
        self->use = NULL;  /* can't use again */
    } else {
        gi.cprintf(activator, PRINT_ALL, "Valve: %d turns remaining\n", self->count);
    }
}

static void SP_func_valve(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *turns_str = ED_FindValue(pairs, num_pairs, "count");

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;
    ent->use = valve_use;
    ent->count = turns_str ? atoi(turns_str) : 3;
    if (ent->count < 1) ent->count = 1;

    gi.linkentity(ent);

    gi.dprintf("  func_valve at (%.0f %.0f %.0f) turns=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->count);
}

/* ==========================================================================
   func_security_door — Heavy door requiring keycard + opening delay
   key = required key bitmask (1,2,4,8), delay = opening time in seconds
   Plays alarm sound, then slowly opens after delay.
   ========================================================================== */

static void security_door_open(edict_t *self)
{
    /* Move door upward to open position */
    float height = self->absmax[2] - self->absmin[2];
    self->s.origin[2] += height;
    self->solid = SOLID_NOT;
    gi.linkentity(self);

    /* Open sound */
    {
        int snd = gi.soundindex("world/door_heavy_open.wav");
        if (snd)
            gi.positioned_sound(self->s.origin, self, CHAN_AUTO,
                                snd, 1.0f, ATTN_NORM, 0);
    }

    /* Fire target chain */
    if (self->target)
        G_UseTargets(self, self->target);
}

static void security_door_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;

    if (!activator || !activator->client)
        return;

    /* Check keycard */
    if (self->count > 0 && !(activator->client->keys & self->count)) {
        gi.cprintf(activator, PRINT_ALL, "Security clearance required.\n");
        {
            int snd = gi.soundindex("world/door_locked.wav");
            if (snd)
                gi.positioned_sound(self->s.origin, self, CHAN_AUTO,
                                    snd, 1.0f, ATTN_NORM, 0);
        }
        return;
    }

    /* Alarm/buzzer then delayed open */
    gi.cprintf(activator, PRINT_ALL, "Security door: Opening...\n");
    {
        int snd = gi.soundindex("world/alarm_beep.wav");
        if (snd)
            gi.positioned_sound(self->s.origin, self, CHAN_AUTO,
                                snd, 1.0f, ATTN_NORM, 0);
    }

    self->think = security_door_open;
    self->nextthink = level.time + self->speed;
    self->use = NULL;  /* can't use while opening */
}

static void SP_func_security_door(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *key_str = ED_FindValue(pairs, num_pairs, "key");
    const char *delay_str = ED_FindValue(pairs, num_pairs, "delay");

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;
    ent->use = security_door_use;
    ent->count = key_str ? atoi(key_str) : 0;
    ent->speed = delay_str ? (float)atof(delay_str) : 3.0f;
    if (ent->speed < 0.5f) ent->speed = 0.5f;

    gi.linkentity(ent);

    gi.dprintf("  func_security_door at (%.0f %.0f %.0f) key=%d delay=%.1f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->count, ent->speed);
}

/* ==========================================================================
   func_alarm — Alarm that alerts all monsters within range and fires target
   Use/trigger to activate. Plays siren sound, wakes up monsters.
   ========================================================================== */

static void alarm_use(edict_t *self, edict_t *other, edict_t *activator)
{
    extern game_export_t globals;
    int i;
    int woke = 0;

    (void)other;

    /* Alarm siren sound */
    {
        int snd = gi.soundindex("world/alarm_siren.wav");
        if (snd)
            gi.positioned_sound(self->s.origin, self, CHAN_AUTO,
                                snd, 1.0f, ATTN_NONE, 0);
    }

    /* Alert all monsters within range */
    for (i = 1; i < globals.num_edicts; i++) {
        edict_t *e = &globals.edicts[i];
        vec3_t diff;
        float dist;

        if (!e->inuse || e->health <= 0)
            continue;
        if (!(e->svflags & SVF_MONSTER))
            continue;
        if (e->enemy)  /* already in combat */
            continue;

        VectorSubtract(e->s.origin, self->s.origin, diff);
        dist = VectorLength(diff);
        if (dist > self->speed)
            continue;

        /* Wake up and alert to activator */
        if (activator && activator->client) {
            e->enemy = activator;
            e->count = 2;  /* AI_STATE_ALERT */
            VectorCopy(activator->s.origin, e->move_origin);
            e->nextthink = level.time + 0.5f;
            woke++;
        }
    }

    gi.dprintf("  Alarm triggered: woke %d monsters\n", woke);

    /* Fire target */
    if (self->target)
        G_UseTargets(activator, self->target);

    /* One-shot: disable after use */
    self->use = NULL;
}

static void SP_func_alarm(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *range_str = ED_FindValue(pairs, num_pairs, "range");

    ent->movetype = MOVETYPE_NONE;
    ent->solid = SOLID_NOT;
    ent->svflags |= SVF_NOCLIENT;
    ent->use = alarm_use;
    ent->speed = range_str ? (float)atof(range_str) : 2000.0f;

    gi.linkentity(ent);

    gi.dprintf("  func_alarm at (%.0f %.0f %.0f) range=%.0f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->speed);
}

/* ==========================================================================
   func_cover — Destructible cover (barricade, sandbags, etc.)
   Takes damage and breaks, spawning debris particles.
   Health determines durability, fires target on destruction.
   ========================================================================== */

static void cover_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                       int damage, vec3_t point)
{
    vec3_t center, up;

    (void)inflictor; (void)damage; (void)point;

    center[0] = (self->absmin[0] + self->absmax[0]) * 0.5f;
    center[1] = (self->absmin[1] + self->absmax[1]) * 0.5f;
    center[2] = (self->absmin[2] + self->absmax[2]) * 0.5f;
    VectorSet(up, 0, 0, 1);

    /* Debris burst */
    R_ParticleEffect(center, up, 0, 24);  /* dust/concrete chunks */
    R_ParticleEffect(center, up, 11, 8);  /* wood splinters */

    /* Break sound */
    {
        int snd = gi.soundindex("world/debris_break.wav");
        if (snd)
            gi.positioned_sound(center, NULL, CHAN_AUTO,
                                snd, 1.0f, ATTN_NORM, 0);
    }

    if (self->target)
        G_UseTargets(attacker, self->target);

    self->solid = SOLID_NOT;
    self->svflags |= SVF_NOCLIENT;
    self->takedamage = DAMAGE_NO;
    self->inuse = qfalse;
    gi.unlinkentity(self);
}

static void SP_func_cover(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *health_str = ED_FindValue(pairs, num_pairs, "health");

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;
    ent->takedamage = DAMAGE_YES;
    ent->health = health_str ? atoi(health_str) : 50;
    ent->max_health = ent->health;
    ent->die = cover_die;

    gi.linkentity(ent);

    gi.dprintf("  func_cover at (%.0f %.0f %.0f) hp=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->health);
}

/* ==========================================================================
   trigger_cutscene — Locks player controls and displays message
   message = text to display, delay = duration in seconds
   Fires target when cutscene ends.
   ========================================================================== */

static void cutscene_end(edict_t *self)
{
    extern game_export_t globals;
    edict_t *player = &globals.edicts[1];

    if (player && player->inuse && player->client) {
        player->client->ps.pm_type = PM_NORMAL;
        gi.cprintf(player, PRINT_ALL, "[Cutscene ended]\n");
    }

    if (self->target)
        G_UseTargets(self, self->target);

    self->think = NULL;
}

static void cutscene_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;

    if (!activator || !activator->client)
        return;

    /* Lock player movement */
    activator->client->ps.pm_type = PM_FREEZE;

    /* Display cutscene message */
    if (self->message) {
        gi.centerprintf(activator, "%s", self->message);
    }

    /* Schedule end */
    self->think = cutscene_end;
    self->nextthink = level.time + self->speed;

    self->use = NULL;  /* one-shot */
}

static void SP_trigger_cutscene(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *delay_str = ED_FindValue(pairs, num_pairs, "delay");
    const char *msg_str = ED_FindValue(pairs, num_pairs, "message");

    ent->movetype = MOVETYPE_NONE;
    ent->solid = SOLID_NOT;
    ent->svflags |= SVF_NOCLIENT;
    ent->use = cutscene_use;
    ent->speed = delay_str ? (float)atof(delay_str) : 5.0f;
    if (msg_str)
        ent->message = msg_str;

    gi.linkentity(ent);

    gi.dprintf("  trigger_cutscene at (%.0f %.0f %.0f) duration=%.1f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->speed);
}

/* ==========================================================================
   trigger_music — Changes background music track when triggered
   noise = sound file to play as music, count = CD track number
   ========================================================================== */

static void music_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;

    if (self->noise_index) {
        /* Play as a positioned sound at full volume, no attenuation */
        gi.positioned_sound(self->s.origin, self, CHAN_AUTO,
                            self->noise_index, 1.0f, ATTN_NONE, 0);
    }

    /* Also set configstring for CD track */
    if (self->count > 0) {
        char trackstr[16];
        snprintf(trackstr, sizeof(trackstr), "%d", self->count);
        gi.configstring(32, trackstr);  /* CS_CDTRACK */
    }

    gi.dprintf("  Music trigger fired: track %d\n", self->count);
}

static void SP_trigger_music(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *noise_str = ED_FindValue(pairs, num_pairs, "noise");
    const char *track_str = ED_FindValue(pairs, num_pairs, "count");

    ent->movetype = MOVETYPE_NONE;
    ent->solid = SOLID_NOT;
    ent->svflags |= SVF_NOCLIENT;
    ent->use = music_use;
    ent->count = track_str ? atoi(track_str) : 0;
    ent->noise_index = noise_str ? gi.soundindex(noise_str) : 0;

    gi.linkentity(ent);

    gi.dprintf("  trigger_music at (%.0f %.0f %.0f) track=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->count);
}

/* ==========================================================================
   func_cage — Trap cage that drops when triggered, trapping the player
   Starts raised, drops on use. Player must destroy it (takedamage) to escape.
   ========================================================================== */

static void cage_drop(edict_t *self)
{
    /* Drop cage downward */
    self->s.origin[2] -= 2.0f;

    /* Check if cage reached ground */
    {
        vec3_t below;
        trace_t tr;
        VectorCopy(self->s.origin, below);
        below[2] -= 4.0f;
        tr = gi.trace(self->s.origin, self->mins, self->maxs, below, self, MASK_SOLID);
        if (tr.fraction < 1.0f || self->count <= 0) {
            /* Cage landed */
            self->solid = SOLID_BSP;
            self->takedamage = DAMAGE_YES;
            gi.linkentity(self);
            {
                int snd = gi.soundindex("world/cage_land.wav");
                if (snd)
                    gi.positioned_sound(self->s.origin, self, CHAN_AUTO,
                                        snd, 1.0f, ATTN_NORM, 0);
            }
            self->think = NULL;
            return;
        }
    }

    self->count--;
    gi.linkentity(self);
    self->nextthink = level.time + 0.05f;
}

static void cage_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;

    /* Start dropping */
    self->think = cage_drop;
    self->nextthink = level.time + 0.05f;
    self->count = 60;  /* max drop frames */
    self->solid = SOLID_NOT;  /* no collision while dropping */
    self->use = NULL;

    {
        int snd = gi.soundindex("world/cage_drop.wav");
        if (snd)
            gi.positioned_sound(self->s.origin, self, CHAN_AUTO,
                                snd, 1.0f, ATTN_NORM, 0);
    }
}

static void cage_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                      int damage, vec3_t point)
{
    vec3_t center, up;
    (void)inflictor; (void)damage; (void)point;

    center[0] = (self->absmin[0] + self->absmax[0]) * 0.5f;
    center[1] = (self->absmin[1] + self->absmax[1]) * 0.5f;
    center[2] = (self->absmin[2] + self->absmax[2]) * 0.5f;
    VectorSet(up, 0, 0, 1);

    R_ParticleEffect(center, up, 12, 16);  /* metal sparks */
    {
        int snd = gi.soundindex("world/cage_break.wav");
        if (snd)
            gi.positioned_sound(center, NULL, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }

    if (self->target)
        G_UseTargets(attacker, self->target);

    self->solid = SOLID_NOT;
    self->svflags |= SVF_NOCLIENT;
    self->takedamage = DAMAGE_NO;
    self->inuse = qfalse;
    gi.unlinkentity(self);
}

static void SP_func_cage(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *health_str = ED_FindValue(pairs, num_pairs, "health");

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_NOT;  /* starts non-solid until dropped */
    ent->use = cage_use;
    ent->die = cage_die;
    ent->health = health_str ? atoi(health_str) : 100;
    ent->max_health = ent->health;
    ent->takedamage = DAMAGE_NO;  /* not damageable until landed */

    VectorSet(ent->mins, -32, -32, 0);
    VectorSet(ent->maxs, 32, 32, 64);

    gi.linkentity(ent);

    gi.dprintf("  func_cage at (%.0f %.0f %.0f) hp=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->health);
}

/* ==========================================================================
   trigger_hazard — Environmental damage zone
   Deals periodic damage to entities inside. Type determines damage flavor:
   "acid" = poison DoT, "electric" = burst damage, "fire" = burn DoT
   Keys: "dmg" = damage per tick, "wait" = seconds between ticks
   ========================================================================== */

static void hazard_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    extern void R_ParticleEffect(vec3_t org, vec3_t dir, int type, int count);
    vec3_t up = {0, 0, 1};

    (void)plane; (void)surf;

    if (!other || !other->client || other->health <= 0)
        return;
    if (other->client->invuln_time > level.time)
        return;
    if (level.time < self->dmg_debounce_time)
        return;

    /* Apply damage */
    other->health -= self->dmg;
    other->client->pers_health = other->health;

    /* Damage flash */
    other->client->blend[0] = 0.5f;
    other->client->blend[1] = 0.8f;
    other->client->blend[2] = 0.0f;
    other->client->blend[3] = 0.25f;

    R_ParticleEffect(other->s.origin, up, 4, 8);

    if (self->message && self->message[0])
        gi.cprintf(other, PRINT_ALL, "%s\n", self->message);

    /* Set tick cooldown */
    self->dmg_debounce_time = level.time + self->wait;

    if (other->health <= 0 && other->die)
        other->die(other, self, self, self->dmg, other->s.origin);
}

static void SP_trigger_hazard(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;

    ent->classname = "trigger_hazard";
    ent->solid = SOLID_TRIGGER;
    ent->movetype = MOVETYPE_NONE;
    ent->touch = hazard_touch;
    ent->svflags |= SVF_NOCLIENT;

    v = ED_FindValue(pairs, num_pairs, "dmg");
    ent->dmg = v ? atoi(v) : 10;

    v = ED_FindValue(pairs, num_pairs, "wait");
    ent->wait = v ? (float)atof(v) : 1.0f;

    v = ED_FindValue(pairs, num_pairs, "message");
    if (v && v[0])
        ent->message = (char *)v;

    VectorSet(ent->mins, -32, -32, -8);
    VectorSet(ent->maxs, 32, 32, 32);

    gi.linkentity(ent);

    gi.dprintf("  trigger_hazard at (%.0f %.0f %.0f) dmg=%d wait=%.1f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->dmg, ent->wait);
}

/* ==========================================================================
   func_zipline — Rideable zipline between two points
   Player uses to mount, slides along vector from origin to target position.
   Keys: "speed" = travel speed, "target" = endpoint entity
   ========================================================================== */

static void zipline_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;

    if (!activator || !activator->client)
        return;

    /* Already riding? */
    if (activator->client->zipline_ent) {
        gi.cprintf(activator, PRINT_ALL, "Already on a zipline!\n");
        return;
    }

    /* Check distance to start */
    {
        vec3_t diff;
        VectorSubtract(self->s.origin, activator->s.origin, diff);
        if (VectorLength(diff) > 96.0f) {
            gi.cprintf(activator, PRINT_ALL, "Too far from zipline.\n");
            return;
        }
    }

    activator->client->zipline_ent = self;
    activator->client->zipline_progress = 0;
    activator->movetype = MOVETYPE_NONE;

    gi.cprintf(activator, PRINT_ALL, "Grabbed zipline!\n");
    {
        int snd = gi.soundindex("world/zipline.wav");
        if (snd)
            gi.sound(activator, CHAN_ITEM, snd, 0.8f, ATTN_NORM, 0);
    }
}

static void zipline_think(edict_t *self)
{
    extern game_export_t globals;
    edict_t *player = &globals.edicts[1];

    self->nextthink = level.time + 0.05f;

    if (!player || !player->inuse || !player->client)
        return;

    if (player->client->zipline_ent != self)
        return;

    /* Advance progress */
    {
        float speed = (self->speed > 0) ? self->speed : 300.0f;
        vec3_t travel;
        float total_dist;

        VectorSubtract(self->move_origin, self->s.origin, travel);
        total_dist = VectorLength(travel);
        if (total_dist < 1.0f) total_dist = 1.0f;

        player->client->zipline_progress += (speed * 0.05f) / total_dist;

        if (player->client->zipline_progress >= 1.0f) {
            /* Arrived at end */
            player->client->zipline_progress = 0;
            player->client->zipline_ent = NULL;
            VectorCopy(self->move_origin, player->s.origin);
            player->movetype = MOVETYPE_WALK;
            gi.linkentity(player);
            gi.cprintf(player, PRINT_ALL, "Zipline complete.\n");
            return;
        }

        /* Interpolate position */
        {
            float t = player->client->zipline_progress;
            player->s.origin[0] = self->s.origin[0] + travel[0] * t;
            player->s.origin[1] = self->s.origin[1] + travel[1] * t;
            player->s.origin[2] = self->s.origin[2] + travel[2] * t;
            player->velocity[0] = player->velocity[1] = 0;
            player->velocity[2] = 0;
            gi.linkentity(player);
        }
    }
}

static void SP_func_zipline(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;

    ent->classname = "func_zipline";
    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->use = zipline_use;
    ent->think = zipline_think;
    ent->nextthink = level.time + 1.0f;

    v = ED_FindValue(pairs, num_pairs, "speed");
    ent->speed = v ? (float)atof(v) : 300.0f;

    /* Endpoint stored in move_origin (set from "target_origin" or use a fixed offset) */
    v = ED_FindValue(pairs, num_pairs, "endpoint");
    if (v) {
        sscanf(v, "%f %f %f",
               &ent->move_origin[0], &ent->move_origin[1], &ent->move_origin[2]);
    } else {
        /* Default: horizontal line 512 units in facing direction */
        VectorCopy(ent->s.origin, ent->move_origin);
        ent->move_origin[0] += 512.0f;
    }

    VectorSet(ent->mins, -8, -8, -8);
    VectorSet(ent->maxs, 8, 8, 8);

    gi.linkentity(ent);

    gi.dprintf("  func_zipline at (%.0f %.0f %.0f) -> (%.0f %.0f %.0f) speed=%.0f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->move_origin[0], ent->move_origin[1], ent->move_origin[2],
               ent->speed);
}

/* ==========================================================================
   func_elevator_call — Elevator call button
   When used, finds the targeted func_plat/func_door and fires its use function.
   Plays a button sound and has a cooldown to prevent spam.
   Keys: "target" = name of elevator entity, "wait" = cooldown between uses
   ========================================================================== */

static void elevator_call_use(edict_t *self, edict_t *other, edict_t *activator)
{
    extern game_export_t globals;
    int i;

    (void)other;

    if (!activator || !activator->client)
        return;

    /* Cooldown check */
    if (level.time < self->dmg_debounce_time) {
        gi.cprintf(activator, PRINT_ALL, "Elevator is on its way...\n");
        return;
    }

    /* Find and activate the targeted elevator */
    if (self->target && self->target[0]) {
        for (i = 1; i < globals.num_edicts; i++) {
            edict_t *e = &globals.edicts[i];
            if (e->inuse && e->targetname && e->targetname[0] &&
                Q_stricmp(e->targetname, self->target) == 0) {
                if (e->use)
                    e->use(e, self, activator);
                break;
            }
        }
    }

    self->dmg_debounce_time = level.time + self->wait;

    /* Button press sound + visual */
    {
        int snd = gi.soundindex("world/button.wav");
        if (snd)
            gi.sound(self, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }

    gi.cprintf(activator, PRINT_ALL, "Called elevator.\n");
}

static void SP_func_elevator_call(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;

    ent->classname = "func_elevator_call";
    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->use = elevator_call_use;

    v = ED_FindValue(pairs, num_pairs, "target");
    if (v && v[0])
        ent->target = (char *)v;

    v = ED_FindValue(pairs, num_pairs, "wait");
    ent->wait = v ? (float)atof(v) : 3.0f;  /* 3s default cooldown */

    VectorSet(ent->mins, -8, -8, 0);
    VectorSet(ent->maxs, 8, 8, 16);

    gi.linkentity(ent);

    gi.dprintf("  func_elevator_call at (%.0f %.0f %.0f) target='%s'\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->target ? ent->target : "none");
}

/* ==========================================================================
   func_crane — Overhead crane that moves objects between two positions
   When triggered, the crane moves from its origin to endpoint and back.
   Any entity touching the crane's platform moves with it.
   Keys: "speed" = movement speed, "endpoint" = destination position
   ========================================================================== */

static void crane_think(edict_t *self)
{
    vec3_t dir, target_pos;
    float dist, move_dist;

    self->nextthink = level.time + 0.05f;

    /* Determine current target: moving to endpoint (count=1) or back (count=0) */
    if (self->count)
        VectorCopy(self->move_origin, target_pos);
    else
        VectorCopy(self->s.old_origin, target_pos);

    VectorSubtract(target_pos, self->s.origin, dir);
    dist = VectorLength(dir);

    if (dist < 2.0f) {
        /* Arrived — stop and wait to be triggered again */
        VectorCopy(target_pos, self->s.origin);
        self->velocity[0] = self->velocity[1] = self->velocity[2] = 0;
        self->count = !self->count;  /* toggle direction */
        gi.linkentity(self);
        self->nextthink = 0;  /* stop thinking until triggered */
        return;
    }

    move_dist = self->speed * 0.05f;
    if (move_dist > dist) move_dist = dist;

    VectorNormalize(dir);
    VectorScale(dir, self->speed, self->velocity);
    gi.linkentity(self);
}

static void crane_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;

    /* Start moving */
    self->think = crane_think;
    self->nextthink = level.time + 0.05f;

    {
        int snd = gi.soundindex("world/crane.wav");
        if (snd)
            gi.sound(self, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }
}

static void SP_func_crane(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;

    ent->classname = "func_crane";
    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_PUSH;
    ent->use = crane_use;
    ent->count = 1;  /* first activation moves to endpoint */

    /* Save origin for return trip */
    VectorCopy(ent->s.origin, ent->s.old_origin);

    v = ED_FindValue(pairs, num_pairs, "speed");
    ent->speed = v ? (float)atof(v) : 100.0f;

    v = ED_FindValue(pairs, num_pairs, "endpoint");
    if (v) {
        sscanf(v, "%f %f %f",
               &ent->move_origin[0], &ent->move_origin[1], &ent->move_origin[2]);
    } else {
        VectorCopy(ent->s.origin, ent->move_origin);
        ent->move_origin[2] += 128.0f;  /* default: up 128 units */
    }

    v = ED_FindValue(pairs, num_pairs, "model");
    if (v && v[0])
        ent->s.modelindex = gi.modelindex(v);

    VectorSet(ent->mins, -32, -32, 0);
    VectorSet(ent->maxs, 32, 32, 8);

    gi.linkentity(ent);

    gi.dprintf("  func_crane at (%.0f %.0f %.0f) -> (%.0f %.0f %.0f) speed=%.0f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->move_origin[0], ent->move_origin[1], ent->move_origin[2],
               ent->speed);
}

/* ==========================================================================
   func_generator — Power generator that fires targets when destroyed
   When killed, all entities matching the "target" field are triggered.
   Visually sparks while taking damage, explodes on death.
   Keys: "health" = generator HP, "target" = entities to disable on destruction
   ========================================================================== */

static void generator_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                           int damage, vec3_t point)
{
    extern void R_ParticleEffect(vec3_t org, vec3_t dir, int type, int count);
    extern void R_AddDlight(vec3_t origin, float r, float g, float b,
                             float intensity, float duration);
    vec3_t up = {0, 0, 1};

    (void)inflictor; (void)damage; (void)point;

    R_ParticleEffect(self->s.origin, up, 2, 24);
    R_ParticleEffect(self->s.origin, up, 10, 16);
    R_AddDlight(self->s.origin, 1.0f, 0.5f, 0.0f, 500.0f, 0.8f);

    {
        int snd = gi.soundindex("world/explode.wav");
        if (snd)
            gi.positioned_sound(self->s.origin, NULL, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }

    /* Fire targets — connected entities get triggered */
    if (self->target)
        G_UseTargets(attacker, self->target);

    self->solid = SOLID_NOT;
    self->takedamage = 0;
    self->s.modelindex = 0;
    gi.linkentity(self);
}

static void generator_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    extern void R_ParticleEffect(vec3_t org, vec3_t dir, int type, int count);
    vec3_t up = {0, 0, 1};
    (void)other; (void)kick; (void)damage;

    /* Sparks when hit */
    R_ParticleEffect(self->s.origin, up, 12, 6);
    {
        int snd = gi.soundindex("world/spark.wav");
        if (snd)
            gi.positioned_sound(self->s.origin, NULL, CHAN_AUTO, snd, 0.7f, ATTN_NORM, 0);
    }
}

static void SP_func_generator(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;

    ent->classname = "func_generator";
    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->takedamage = DAMAGE_YES;
    ent->die = generator_die;
    ent->pain = generator_pain;

    v = ED_FindValue(pairs, num_pairs, "health");
    ent->health = v ? atoi(v) : 150;
    ent->max_health = ent->health;

    v = ED_FindValue(pairs, num_pairs, "target");
    if (v && v[0])
        ent->target = (char *)v;

    v = ED_FindValue(pairs, num_pairs, "model");
    if (v && v[0])
        ent->s.modelindex = gi.modelindex(v);

    VectorSet(ent->mins, -16, -16, 0);
    VectorSet(ent->maxs, 16, 16, 32);

    gi.linkentity(ent);

    gi.dprintf("  func_generator at (%.0f %.0f %.0f) hp=%d target='%s'\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->health, ent->target ? ent->target : "none");
}

/* ==========================================================================
   func_trap_floor — Trapdoor that opens when stepped on
   Becomes SOLID_NOT when a player touches it, drops them below.
   Resets after "wait" seconds.
   ========================================================================== */

static void trap_floor_reset(edict_t *self)
{
    self->solid = SOLID_BSP;
    self->s.modelindex = self->count;  /* restore model from saved index */
    gi.linkentity(self);
    self->nextthink = 0;
}

static void trap_floor_touch(edict_t *self, edict_t *other, void *plane, csurface_t *surf)
{
    (void)plane; (void)surf;

    if (!other || !other->client)
        return;
    if (self->solid != SOLID_BSP)
        return;  /* already triggered */

    /* Open the trapdoor */
    self->solid = SOLID_NOT;
    self->s.modelindex = 0;  /* hide model */
    gi.linkentity(self);

    {
        int snd = gi.soundindex("world/trapdoor.wav");
        if (snd)
            gi.positioned_sound(self->s.origin, NULL, CHAN_AUTO, snd, 1.0f, ATTN_NORM, 0);
    }

    /* Reset after wait time */
    self->think = trap_floor_reset;
    self->nextthink = level.time + self->wait;
}

static void SP_func_trap_floor(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;

    ent->classname = "func_trap_floor";
    ent->solid = SOLID_BSP;
    ent->movetype = MOVETYPE_PUSH;
    ent->touch = trap_floor_touch;

    v = ED_FindValue(pairs, num_pairs, "wait");
    ent->wait = v ? (float)atof(v) : 5.0f;

    v = ED_FindValue(pairs, num_pairs, "model");
    if (v && v[0])
        ent->s.modelindex = gi.modelindex(v);

    ent->count = ent->s.modelindex;  /* save model index for reset */

    gi.linkentity(ent);

    gi.dprintf("  func_trap_floor at (%.0f %.0f %.0f) wait=%.1f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], ent->wait);
}

/* ==========================================================================
   func_modstation — Weapon modification station
   Player uses to cycle through and add attachments to current weapon.
   Each use adds the next available attachment.
   ========================================================================== */

static void modstation_use(edict_t *self, edict_t *other, edict_t *activator)
{
    int w, att;
    (void)other; (void)self;

    if (!activator || !activator->client)
        return;

    w = activator->client->pers_weapon;
    if (w <= 0 || w >= WEAP_COUNT) {
        gi.cprintf(activator, PRINT_ALL, "No weapon equipped.\n");
        return;
    }

    att = activator->client->attachments[w];

    /* Add next available attachment */
    if (!(att & ATTACH_SILENCER)) {
        activator->client->attachments[w] |= ATTACH_SILENCER;
        gi.cprintf(activator, PRINT_ALL, "Silencer attached!\n");
        SCR_AddPickupMessage("SILENCER ATTACHED");
    } else if (!(att & ATTACH_SCOPE)) {
        activator->client->attachments[w] |= ATTACH_SCOPE;
        gi.cprintf(activator, PRINT_ALL, "Scope attached!\n");
        SCR_AddPickupMessage("SCOPE ATTACHED");
    } else if (!(att & ATTACH_EXTMAG)) {
        activator->client->attachments[w] |= ATTACH_EXTMAG;
        gi.cprintf(activator, PRINT_ALL, "Extended mag attached!\n");
        SCR_AddPickupMessage("EXTENDED MAG ATTACHED");
    } else if (!(att & ATTACH_LASER)) {
        activator->client->attachments[w] |= ATTACH_LASER;
        gi.cprintf(activator, PRINT_ALL, "Laser sight attached!\n");
        SCR_AddPickupMessage("LASER ATTACHED");
    } else {
        gi.cprintf(activator, PRINT_ALL, "All attachments already equipped.\n");
        return;
    }

    /* Also restore weapon condition */
    activator->client->weapon_condition[w] = 1.0f;

    {
        int snd = gi.soundindex("weapons/mod_attach.wav");
        if (snd)
            gi.sound(activator, CHAN_ITEM, snd, 1.0f, ATTN_NORM, 0);
    }
}

static void SP_func_modstation(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;

    ent->classname = "func_modstation";
    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->use = modstation_use;

    VectorSet(ent->mins, -16, -16, 0);
    VectorSet(ent->maxs, 16, 16, 32);

    gi.linkentity(ent);

    gi.dprintf("  func_modstation at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   func_spotlight — Rotating spotlight that emits a dynamic light
   The spotlight rotates continuously, creating a sweeping light effect.
   Keys: "light" = intensity, "speed" = rotation speed, "_color" = R G B
   ========================================================================== */

static void func_spotlight_think(edict_t *self)
{
    extern void R_AddDlight(vec3_t origin, float r, float g, float b,
                             float intensity, float duration);
    float angle;
    vec3_t light_pos;

    self->nextthink = level.time + 0.05f;

    /* Rotate angle over time */
    angle = level.time * self->speed;
    light_pos[0] = self->s.origin[0] + cosf(angle) * 128.0f;
    light_pos[1] = self->s.origin[1] + sinf(angle) * 128.0f;
    light_pos[2] = self->s.origin[2] - 64.0f;  /* project downward */

    R_AddDlight(light_pos,
                self->move_angles[0], self->move_angles[1], self->move_angles[2],
                self->dmg > 0 ? (float)self->dmg : 300.0f, 0.06f);
}

static void SP_func_spotlight(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;

    ent->classname = "func_spotlight";
    ent->solid = SOLID_NOT;
    ent->movetype = MOVETYPE_NONE;
    ent->think = func_spotlight_think;
    ent->nextthink = level.time + 1.0f;

    v = ED_FindValue(pairs, num_pairs, "speed");
    ent->speed = v ? (float)atof(v) : 1.0f;

    v = ED_FindValue(pairs, num_pairs, "light");
    ent->dmg = v ? atoi(v) : 300;

    /* Color defaults to white */
    v = ED_FindValue(pairs, num_pairs, "_color");
    if (v) {
        sscanf(v, "%f %f %f",
               &ent->move_angles[0], &ent->move_angles[1], &ent->move_angles[2]);
    } else {
        VectorSet(ent->move_angles, 1.0f, 1.0f, 0.9f);
    }

    gi.linkentity(ent);

    gi.dprintf("  func_spotlight at (%.0f %.0f %.0f) speed=%.1f intensity=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->speed, ent->dmg);
}

/* ==========================================================================
   func_floodlight — Togglable area light that illuminates surroundings.
   Keys: "light" = intensity (default 400), "_color" = R G B, "style" = 0/1 on/off
   Activated by use (toggle).
   ========================================================================== */

static void floodlight_think(edict_t *self)
{
    extern void R_AddDlight(vec3_t origin, float r, float g, float b,
                             float intensity, float duration);
    if (self->count) {  /* on */
        R_AddDlight(self->s.origin,
                    self->move_angles[0], self->move_angles[1], self->move_angles[2],
                    self->dmg > 0 ? (float)self->dmg : 400.0f, 0.15f);
    }
    self->nextthink = level.time + 0.1f;
}

static void floodlight_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;
    self->count = !self->count;  /* toggle */
    if (self->count) {
        int snd = gi.soundindex("world/switch_on.wav");
        if (snd) gi.sound(self, CHAN_BODY, snd, 0.7f, ATTN_STATIC, 0);
    }
}

static void SP_func_floodlight(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;

    ent->classname = "func_floodlight";
    ent->solid = SOLID_NOT;
    ent->movetype = MOVETYPE_NONE;
    ent->use = floodlight_use;
    ent->think = floodlight_think;
    ent->nextthink = level.time + 1.0f;

    v = ED_FindValue(pairs, num_pairs, "light");
    ent->dmg = v ? atoi(v) : 400;

    v = ED_FindValue(pairs, num_pairs, "_color");
    if (v)
        sscanf(v, "%f %f %f",
               &ent->move_angles[0], &ent->move_angles[1], &ent->move_angles[2]);
    else
        VectorSet(ent->move_angles, 1.0f, 1.0f, 0.95f);

    v = ED_FindValue(pairs, num_pairs, "style");
    ent->count = v ? atoi(v) : 1;  /* starts on by default */

    gi.linkentity(ent);
    gi.dprintf("  func_floodlight at (%.0f %.0f %.0f) intensity=%d %s\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->dmg, ent->count ? "ON" : "OFF");
}

/* ==========================================================================
   func_barrier — Retractable barrier/bollard that lowers into the ground
   on use and raises back after a delay.
   Keys: "lip" = lower distance (default 64), "speed" = movement speed,
         "wait" = time before raising back (-1 = stay lowered)
   ========================================================================== */

static void barrier_raise(edict_t *self)
{
    if (self->s.origin[2] < self->moveinfo.start_origin[2]) {
        self->velocity[2] = self->speed;
        self->think = barrier_raise;
        self->nextthink = level.time + 0.1f;
    } else {
        VectorCopy(self->moveinfo.start_origin, self->s.origin);
        self->velocity[2] = 0;
        self->moveinfo.state = 0;  /* raised */
        self->solid = SOLID_BSP;
        gi.linkentity(self);
    }
}

static void barrier_wait_raise(edict_t *self)
{
    barrier_raise(self);
}

static void barrier_lower(edict_t *self)
{
    if (self->s.origin[2] > self->moveinfo.end_origin[2]) {
        self->velocity[2] = -self->speed;
        self->think = barrier_lower;
        self->nextthink = level.time + 0.1f;
    } else {
        VectorCopy(self->moveinfo.end_origin, self->s.origin);
        self->velocity[2] = 0;
        self->moveinfo.state = 1;  /* lowered */
        self->solid = SOLID_NOT;  /* passable when lowered */
        gi.linkentity(self);
        if (self->wait >= 0) {
            self->think = barrier_wait_raise;
            self->nextthink = level.time + (self->wait > 0 ? self->wait : 5.0f);
        }
    }
}

static void barrier_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;
    if (self->moveinfo.state == 0) {
        int snd = gi.soundindex("world/switch_on.wav");
        if (snd) gi.sound(self, CHAN_BODY, snd, 0.7f, ATTN_STATIC, 0);
        barrier_lower(self);
    } else if (self->moveinfo.state == 1) {
        barrier_raise(self);
    }
}

static void SP_func_barrier(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;
    float lip;

    ent->classname = "func_barrier";
    ent->solid = SOLID_BSP;
    ent->movetype = MOVETYPE_PUSH;
    ent->use = barrier_use;

    v = ED_FindValue(pairs, num_pairs, "speed");
    ent->speed = v ? (float)atof(v) : 60.0f;

    v = ED_FindValue(pairs, num_pairs, "lip");
    lip = v ? (float)atof(v) : 64.0f;

    v = ED_FindValue(pairs, num_pairs, "wait");
    ent->wait = v ? (float)atof(v) : 5.0f;

    VectorCopy(ent->s.origin, ent->moveinfo.start_origin);
    VectorCopy(ent->s.origin, ent->moveinfo.end_origin);
    ent->moveinfo.end_origin[2] -= lip;
    ent->moveinfo.state = 0;  /* starts raised (blocking) */

    gi.linkentity(ent);
    gi.dprintf("  func_barrier at (%.0f %.0f %.0f) lip=%.0f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], lip);
}

/* ==========================================================================
   func_crate — Destroyable crate that drops an item on destruction.
   Keys: "health" = hit points (default 40), "item" = classname of dropped item
   ========================================================================== */

static void crate_die(edict_t *self, edict_t *inflictor, edict_t *attacker,
                       int damage, vec3_t point)
{
    vec3_t up = {0, 0, 1};
    (void)inflictor; (void)attacker; (void)damage; (void)point;

    /* Break particles */
    R_ParticleEffect(self->s.origin, up, 11, 16);  /* debris chunks */
    R_ParticleEffect(self->s.origin, up, 13, 8);   /* dust */

    /* Break sound */
    {
        int snd = gi.soundindex("world/crate_break.wav");
        if (snd) gi.sound(self, CHAN_BODY, snd, 1.0f, ATTN_NORM, 0);
    }

    /* Drop item if specified */
    if (self->target) {
        edict_t *drop = G_AllocEdict();
        if (drop) {
            drop->classname = self->target;
            VectorCopy(self->s.origin, drop->s.origin);
            drop->s.origin[2] += 16;
            drop->solid = SOLID_TRIGGER;
            drop->movetype = MOVETYPE_TOSS;
            VectorSet(drop->mins, -16, -16, 0);
            VectorSet(drop->maxs, 16, 16, 16);
            drop->velocity[2] = 100.0f;
            gi.linkentity(drop);
        }
    }

    /* Remove crate */
    self->solid = SOLID_NOT;
    self->svflags |= SVF_NOCLIENT;
    self->takedamage = DAMAGE_NO;
    gi.linkentity(self);
}

static void SP_func_crate(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;

    ent->classname = "func_crate";
    ent->solid = SOLID_BBOX;
    ent->movetype = MOVETYPE_NONE;
    ent->takedamage = DAMAGE_YES;
    ent->die = crate_die;

    v = ED_FindValue(pairs, num_pairs, "health");
    ent->health = v ? atoi(v) : 40;
    ent->max_health = ent->health;

    /* "item" key stored in target for drop on death */
    v = ED_FindValue(pairs, num_pairs, "item");
    if (v) ent->target = (char *)v;

    VectorSet(ent->mins, -16, -16, 0);
    VectorSet(ent->maxs, 16, 16, 32);

    gi.linkentity(ent);
    gi.dprintf("  func_crate at (%.0f %.0f %.0f) hp=%d item=%s\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->health, ent->target ? ent->target : "none");
}

/* ==========================================================================
   cover_point — AI cover node. Placed in maps to indicate good cover
   positions. AI_SeekCover will prefer these over random directions.
   ========================================================================== */

static void SP_cover_point(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;
    ent->classname = "cover_point";
    ent->solid = SOLID_NOT;
    ent->movetype = MOVETYPE_NONE;
    ent->svflags |= SVF_NOCLIENT;  /* invisible marker */
    gi.linkentity(ent);
    gi.dprintf("  cover_point at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   trigger_monster_spawn — Spawns a monster when activated by use.
   Keys: "classname_spawn" or "monster" = monster classname to spawn,
         "count" = number to spawn (default 1)
   ========================================================================== */

static void monster_spawn_use(edict_t *self, edict_t *other, edict_t *activator)
{
    extern void G_SpawnMonster(edict_t *ent, const char *classname);
    int i;
    (void)other; (void)activator;

    for (i = 0; i < self->count; i++) {
        edict_t *mon = G_AllocEdict();
        if (!mon) break;
        VectorCopy(self->s.origin, mon->s.origin);
        mon->s.origin[0] += gi.flrand(-32, 32);
        mon->s.origin[1] += gi.flrand(-32, 32);
        mon->classname = self->message ? self->message : "monster_soldier";
        mon->solid = SOLID_BBOX;
        mon->movetype = MOVETYPE_STEP;
        mon->svflags |= SVF_MONSTER;
        mon->takedamage = DAMAGE_AIM;
        mon->health = 100;
        mon->max_health = 100;
        VectorSet(mon->mins, -16, -16, -24);
        VectorSet(mon->maxs, 16, 16, 32);
        gi.linkentity(mon);
    }
}

static void SP_trigger_monster_spawn(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;

    ent->classname = "trigger_monster_spawn";
    ent->solid = SOLID_NOT;
    ent->movetype = MOVETYPE_NONE;
    ent->use = monster_spawn_use;

    v = ED_FindValue(pairs, num_pairs, "monster");
    if (v) ent->message = (char *)v;

    v = ED_FindValue(pairs, num_pairs, "count");
    ent->count = v ? atoi(v) : 1;
    if (ent->count < 1) ent->count = 1;
    if (ent->count > 8) ent->count = 8;

    gi.linkentity(ent);
    gi.dprintf("  trigger_monster_spawn at (%.0f %.0f %.0f) type=%s count=%d\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               ent->message ? ent->message : "default", ent->count);
}

/* ==========================================================================
   fallback_point — AI retreat marker. When AI retreats, it seeks the
   nearest fallback_point entity to move toward.
   ========================================================================== */

static void SP_fallback_point(edict_t *ent, epair_t *pairs, int num_pairs)
{
    (void)pairs; (void)num_pairs;
    ent->classname = "fallback_point";
    ent->solid = SOLID_NOT;
    ent->movetype = MOVETYPE_NONE;
    ent->svflags |= SVF_NOCLIENT;
    gi.linkentity(ent);
    gi.dprintf("  fallback_point at (%.0f %.0f %.0f)\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
}

/* ==========================================================================
   func_portcullis — Vertical gate that raises on use, lowers after delay.
   Keys: "lip" = raise height (default 128), "speed" = movement speed,
         "wait" = time before lowering (-1 = stay open)
   ========================================================================== */

static void portcullis_lower(edict_t *self);

static void portcullis_lower_done(edict_t *self)
{
    self->moveinfo.state = 0;  /* closed */
    self->velocity[2] = 0;
    VectorCopy(self->moveinfo.start_origin, self->s.origin);
    gi.linkentity(self);
}

static void portcullis_lower(edict_t *self)
{
    self->moveinfo.state = 3;  /* lowering */
    self->velocity[2] = -self->speed;
    /* Check if reached bottom */
    if (self->s.origin[2] <= self->moveinfo.start_origin[2]) {
        portcullis_lower_done(self);
        return;
    }
    self->think = portcullis_lower;
    self->nextthink = level.time + 0.1f;
}

static void portcullis_wait(edict_t *self)
{
    portcullis_lower(self);
}

static void portcullis_raise_done(edict_t *self)
{
    self->moveinfo.state = 1;  /* open */
    self->velocity[2] = 0;
    VectorCopy(self->moveinfo.end_origin, self->s.origin);
    gi.linkentity(self);
    /* Wait then lower, unless wait == -1 (stay open) */
    if (self->wait >= 0) {
        self->think = portcullis_wait;
        self->nextthink = level.time + (self->wait > 0 ? self->wait : 3.0f);
    }
}

static void portcullis_raise(edict_t *self)
{
    self->moveinfo.state = 2;  /* raising */
    self->velocity[2] = self->speed;
    /* Check if reached top */
    if (self->s.origin[2] >= self->moveinfo.end_origin[2]) {
        portcullis_raise_done(self);
        return;
    }
    self->think = portcullis_raise;
    self->nextthink = level.time + 0.1f;
}

static void portcullis_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other; (void)activator;
    if (self->moveinfo.state == 0) {
        /* Closed — raise */
        int snd = gi.soundindex("world/gate_open.wav");
        if (snd)
            gi.sound(self, CHAN_BODY, snd, 1.0f, ATTN_STATIC, 0);
        portcullis_raise(self);
    } else if (self->moveinfo.state == 1) {
        /* Open — lower immediately */
        portcullis_lower(self);
    }
}

static void SP_func_portcullis(edict_t *ent, epair_t *pairs, int num_pairs)
{
    const char *v;
    float lip;

    ent->classname = "func_portcullis";
    ent->solid = SOLID_BSP;
    ent->movetype = MOVETYPE_PUSH;
    ent->use = portcullis_use;

    v = ED_FindValue(pairs, num_pairs, "speed");
    ent->speed = v ? (float)atof(v) : 80.0f;

    v = ED_FindValue(pairs, num_pairs, "lip");
    lip = v ? (float)atof(v) : 128.0f;

    v = ED_FindValue(pairs, num_pairs, "wait");
    ent->wait = v ? (float)atof(v) : 3.0f;

    VectorCopy(ent->s.origin, ent->moveinfo.start_origin);
    VectorCopy(ent->s.origin, ent->moveinfo.end_origin);
    ent->moveinfo.end_origin[2] += lip;

    ent->moveinfo.state = 0;  /* starts closed */

    gi.linkentity(ent);
    gi.dprintf("  func_portcullis at (%.0f %.0f %.0f) lip=%.0f speed=%.0f\n",
               ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
               lip, ent->speed);
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
