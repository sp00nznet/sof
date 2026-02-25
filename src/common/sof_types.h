/*
 * sof_types.h - Common type definitions for Soldier of Fortune recompilation
 *
 * These types mirror the original game's data structures, reconstructed
 * through binary analysis of the original executables.
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
    /* Additional zones TBD via analysis */

    GORE_ZONE_COUNT
} ghoul_gore_zone_t;

/* Placeholder - actual structure to be determined via binary analysis */
typedef struct {
    int             zone_id;
    float           health;
    qboolean        severed;
    qboolean        damaged;
    int             damage_level;   /* visual damage state */
} ghoul_zone_state_t;

/* ==========================================================================
   Engine <-> Game Interface (Quake II pattern)
   ========================================================================== */

/* Forward declarations - full structures TBD via analysis */
typedef struct edict_s      edict_t;
typedef struct gclient_s    gclient_t;
typedef struct cvar_s       cvar_t;

/* Console variable */
struct cvar_s {
    char        *name;
    char        *string;
    char        *latched_string;
    int         flags;
    qboolean    modified;
    float       value;
    struct cvar_s *next;
};

#endif /* SOF_TYPES_H */
