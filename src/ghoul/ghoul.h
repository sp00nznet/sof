/*
 * ghoul.h - GHOUL Gore/Model System
 *
 * Raven Software's proprietary model system for Soldier of Fortune.
 * Replaces Quake II's MD2/MD3 with a zone-based damage model supporting:
 *   - 26 independent gore zones per humanoid
 *   - Dismemberment and wound channels
 *   - Bolt-on attachments (weapons, helmets, accessories)
 *   - Per-zone hit detection and damage response
 *   - Animation blending with skeletal hierarchy
 *
 * Original code: SoF.exe 0xA0000-0x108000 (~416KB, GHOUL engine)
 * Key function: GHOUL_LoadGHB at 0xA8C50
 *
 * File formats:
 *   .ghb  — GHOUL Model Bundle (magic 0x198237FE)
 *   .gsq  — GHOUL Sequence (animation sequences)
 *   .rmf  — Animation Reference/Mapping
 *   .gpm  — GHOUL Player Model Definition (text)
 *   .gbm  — GHOUL Bolt-on Model Definition (text)
 *   .m32  — MIP32 Texture (header 0x28, 32-bit mipmapped)
 */

#ifndef GHOUL_H
#define GHOUL_H

#include "../common/q_shared.h"
#include "../common/qcommon.h"

/* ==========================================================================
   GHB File Format (GHOUL Model Bundle)
   ========================================================================== */

#define GHB_MAGIC           0x198237FE
#define GHB_VERSION         0x033FB1A3  /* constant checksum/version field */
#define GHB_HEADER_SIZE     0x4A        /* 74 bytes */

typedef struct {
    uint32_t    magic;          /* +0x00: 0x198237FE */
    uint32_t    data_offset;    /* +0x04: offset to data section */
    uint32_t    total_size;     /* +0x08: file/data block size */
    uint32_t    version;        /* +0x0C: 0x033FB1A3 (format version) */
    uint32_t    header_size;    /* +0x10: 0x4A (74) */
    uint32_t    reserved_14;    /* +0x14: always 0 */
    uint32_t    path_offset;    /* +0x18: offset to source path (0x28) */
    /* +0x1C: null-terminated source path string from build machine */
} ghb_file_header_t;

/* Extended header fields discovered through analysis */
typedef struct {
    ghb_file_header_t   hdr;
    char                source_path[256];   /* build machine path */
    char                model_name[64];     /* model identifier */

    /* Fields at known offsets (relative to file start): */
    /* +0x44: bone/mesh count (11 for bolt-ons, 3393 for full MESO) */
    /* +0x48: skin/material count (1 for bolt-ons, 202 for full MESO) */
    /* +0x58: float — scale or bounding radius (100.0 / 200.0) */
} ghb_extended_header_t;

/* ==========================================================================
   M32 Texture Format (MIP32 — SoF's replacement for Q2 .wal)
   ========================================================================== */

#define M32_HEADER_SIZE     0x28    /* 40 bytes */

typedef struct {
    uint32_t    header_size;    /* +0x00: always 0x28 (40) */
    uint32_t    width;          /* +0x04: texture width */
    uint32_t    height;         /* +0x08: texture height */
    uint32_t    flags;          /* +0x0C: 0 in most files */
    float       param[3];       /* +0x10: three identical floats (LOD?) */
    uint32_t    reserved[5];    /* +0x1C: zeros */
    /* +0x28: mipmap data begins */
    /* Data appears to be RGBA8888, pixel data at ~offset 0xA0 */
} m32_header_t;

/* ==========================================================================
   Gore Zone System — 26 zones per humanoid
   ========================================================================== */

typedef enum {
    GORE_ZONE_HEAD = 0,
    GORE_ZONE_NECK,
    GORE_ZONE_CHEST_UPPER,
    GORE_ZONE_CHEST_LOWER,
    GORE_ZONE_STOMACH,
    GORE_ZONE_GROIN,
    GORE_ZONE_ARM_UPPER_R,
    GORE_ZONE_ARM_LOWER_R,
    GORE_ZONE_HAND_R,
    GORE_ZONE_ARM_UPPER_L,
    GORE_ZONE_ARM_LOWER_L,
    GORE_ZONE_HAND_L,
    GORE_ZONE_LEG_UPPER_R,
    GORE_ZONE_LEG_LOWER_R,
    GORE_ZONE_FOOT_R,
    GORE_ZONE_LEG_UPPER_L,
    GORE_ZONE_LEG_LOWER_L,
    GORE_ZONE_FOOT_L,
    GORE_ZONE_BACK_UPPER,
    GORE_ZONE_BACK_LOWER,
    GORE_ZONE_BUTT,
    GORE_ZONE_SHOULDER_R,
    GORE_ZONE_SHOULDER_L,
    GORE_ZONE_HIP_R,
    GORE_ZONE_HIP_L,
    GORE_ZONE_FACE,

    GORE_NUM_ZONES          /* = 26 */
} gore_zone_id_t;

/* State of a single gore zone */
typedef struct {
    gore_zone_id_t  id;
    int             health;         /* Zone-specific health */
    qboolean        severed;        /* Has this limb been dismembered? */
    qboolean        damaged;        /* Has visible damage? */
    int             damage_level;   /* 0=pristine, 1=scratched, 2=wounded, 3=destroyed */
    int             wound_count;    /* Number of wound decals applied */
} ghoul_zone_state_t;

/* ==========================================================================
   Bolt-on Attachment System
   ========================================================================== */

#define GHOUL_MAX_BOLTONS   16

typedef struct {
    char            model_path[MAX_QPATH];  /* e.g., "boltons/menuassault.gbm" */
    char            bolt_name[32];          /* e.g., "wbolt_hand_r" */
    char            to_bolt[32];            /* e.g., "to_wbolt_hand_r" */
    float           scale;                  /* typically 1.0 */
    qboolean        active;
} ghoul_bolton_t;

/* ==========================================================================
   Skin / Material Assignment
   ========================================================================== */

#define GHOUL_MAX_SKINS     32

typedef struct {
    char    part;           /* Part letter: 'a', 'b', 'f', 'h', etc. */
    char    skin_name[32];  /* e.g., "a_xdekker" */
    int     variant;        /* Skin variant index */
} ghoul_skin_entry_t;

/* ==========================================================================
   GHOUL Model Instance
   Complete runtime state for one GHOUL-rendered entity
   ========================================================================== */

typedef struct ghoul_model_s {
    /* Identity */
    char            name[MAX_QPATH];
    char            base_dir[MAX_QPATH];    /* e.g., "enemy/meso" */

    /* GHB data (loaded from PAK) */
    byte            *ghb_data;
    int             ghb_size;
    qboolean        loaded;

    /* Mesh/Bone counts from header */
    int             num_bones;
    int             num_meshes;
    int             num_skins;
    float           bounding_radius;

    /* Gore zones */
    ghoul_zone_state_t  zones[GORE_NUM_ZONES];

    /* Bolt-on attachments */
    ghoul_bolton_t  boltons[GHOUL_MAX_BOLTONS];
    int             num_boltons;

    /* Skin assignments */
    ghoul_skin_entry_t  skins[GHOUL_MAX_SKINS];
    int                 num_skins_assigned;

    /* Cosmetic toggles (from GPM) */
    struct {
        char    name[32];       /* e.g., "_lbang", "_mohawk" */
        int     enabled;
    } cosmetics[16];
    int             num_cosmetics;

    /* Animation state */
    char            anim_set[MAX_QPATH];    /* e.g., "meso_player" */
    char            menu_anim[MAX_QPATH];   /* e.g., "meso_menu1" */
    int             current_frame;
    int             old_frame;
    float           backlerp;

    /* Team / Class */
    char            player_class[32];       /* e.g., "playerboss" */
    char            team_name[64];          /* e.g., "The Order" */
    char            death_sounds[MAX_QPATH]; /* e.g., "enemy/dth/rad" */
    char            description[128];
} ghoul_model_t;

/* ==========================================================================
   GHOUL Player Model (GPM) — text format definition
   ========================================================================== */

typedef struct {
    char            base_dir[MAX_QPATH];    /* line 1: "enemy/meso" */
    char            menu_anim[MAX_QPATH];   /* line 2: "meso_menu1" */
    char            play_anim[MAX_QPATH];   /* line 3: "meso_player" */
    char            player_class[32];       /* line 4: "playerboss" */
    char            team_name[64];          /* line 5: "The Order" */
    char            death_sounds[MAX_QPATH]; /* line 6: "enemy/dth/rad" */

    /* Braced blocks */
    ghoul_skin_entry_t  skins[GHOUL_MAX_SKINS];
    int                 num_skins;

    ghoul_bolton_t      boltons[GHOUL_MAX_BOLTONS];
    int                 num_boltons;

    struct {
        char    name[32];
        int     value;
    } cosmetics[16];
    int                 num_cosmetics;

    char            description[128];
} ghoul_player_model_t;

/* ==========================================================================
   GHOUL API
   ========================================================================== */

/* System lifecycle */
void    GHOUL_Init(void);
void    GHOUL_Shutdown(void);

/* GHB file loading */
ghoul_model_t   *GHOUL_LoadGHB(const char *filename);
void            GHOUL_FreeModel(ghoul_model_t *model);

/* GPM text format parser */
qboolean    GHOUL_LoadPlayerModel(const char *filename, ghoul_player_model_t *gpm);

/* GBM text format parser */
qboolean    GHOUL_LoadBoltOnModel(const char *filename, ghoul_bolton_t *bolton);

/* Gore zone management */
void    GHOUL_ResetZones(ghoul_model_t *model);
void    GHOUL_DamageZone(ghoul_model_t *model, gore_zone_id_t zone, int damage);
void    GHOUL_SeverZone(ghoul_model_t *model, gore_zone_id_t zone);
const char *GHOUL_ZoneName(gore_zone_id_t zone);

/* M32 texture loading */
byte    *M32_LoadTexture(const char *filename, int *width, int *height);
void    M32_FreeTexture(byte *data);

/* Bolt-on system */
void    GHOUL_AttachBolton(ghoul_model_t *model, const ghoul_bolton_t *bolton);
void    GHOUL_DetachBolton(ghoul_model_t *model, int index);

/* Known bolt points (from binary analysis of GHOUL_SetupBolts at 0x96C82) */
/*
 * FORCED_CAMERA      — Camera attachment point
 * B_DOOR_LEFT        — Left door/shield bolt
 * B_DOOR_RIGHT       — Right door/shield bolt
 * B_BOLT1            — Generic attachment point
 * wbolt_hand_r       — Right hand weapon bolt
 * wbolt_hand_l       — Left hand weapon bolt
 * to_wbolt_hand_r    — Target bolt for right hand weapon
 * to_wbolt_hand_l    — Target bolt for left hand weapon
 */

#endif /* GHOUL_H */
