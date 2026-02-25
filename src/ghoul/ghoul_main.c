/*
 * ghoul_main.c - GHOUL model system initialization and GHB loader
 *
 * Implements the core GHOUL model loading and gore zone management.
 * Parses GHB binary format, GPM/GBM text formats, and M32 textures.
 *
 * Original: GHOUL_LoadGHB at 0xA8C50 in SoF.exe
 * Error strings: "Old file %s", "Not a ghb file %s", "Thunks are obsolete %s"
 */

#include "ghoul.h"

/* ==========================================================================
   Zone Name Table
   ========================================================================== */

static const char *zone_names[GORE_NUM_ZONES] = {
    "Head",
    "Neck",
    "Upper Chest",
    "Lower Chest",
    "Stomach",
    "Groin",
    "Right Upper Arm",
    "Right Lower Arm",
    "Right Hand",
    "Left Upper Arm",
    "Left Lower Arm",
    "Left Hand",
    "Right Upper Leg",
    "Right Lower Leg",
    "Right Foot",
    "Left Upper Leg",
    "Left Lower Leg",
    "Left Foot",
    "Upper Back",
    "Lower Back",
    "Butt",
    "Right Shoulder",
    "Left Shoulder",
    "Right Hip",
    "Left Hip",
    "Face"
};

const char *GHOUL_ZoneName(gore_zone_id_t zone)
{
    if (zone < 0 || zone >= GORE_NUM_ZONES)
        return "Unknown";
    return zone_names[zone];
}

/* ==========================================================================
   System Init / Shutdown
   ========================================================================== */

static cvar_t *gore_detail;
static int ghoul_initialized = 0;

void GHOUL_Init(void)
{
    gore_detail = Cvar_Get("gore_detail", "2", CVAR_ARCHIVE);

    Com_Printf("------- GHOUL Init -------\n");
    Com_Printf("Gore zones: %d\n", GORE_NUM_ZONES);
    Com_Printf("Gore detail: %s\n",
        (int)gore_detail->value == 0 ? "off" :
        (int)gore_detail->value == 1 ? "low" : "full");
    Com_Printf("--------------------------\n");

    ghoul_initialized = 1;
}

void GHOUL_Shutdown(void)
{
    ghoul_initialized = 0;
}

/* ==========================================================================
   Gore Zone Management
   ========================================================================== */

void GHOUL_ResetZones(ghoul_model_t *model)
{
    int i;
    for (i = 0; i < GORE_NUM_ZONES; i++) {
        model->zones[i].id = (gore_zone_id_t)i;
        model->zones[i].health = 100;
        model->zones[i].severed = qfalse;
        model->zones[i].damaged = qfalse;
        model->zones[i].damage_level = 0;
        model->zones[i].wound_count = 0;
    }
}

void GHOUL_DamageZone(ghoul_model_t *model, gore_zone_id_t zone, int damage)
{
    ghoul_zone_state_t *z;

    if (zone < 0 || zone >= GORE_NUM_ZONES)
        return;
    if (gore_detail && gore_detail->value == 0)
        return; /* Gore disabled */

    z = &model->zones[zone];
    if (z->severed)
        return; /* Already gone */

    z->health -= damage;
    z->damaged = qtrue;
    z->wound_count++;

    /* Determine damage level */
    if (z->health > 70)
        z->damage_level = 1;    /* scratched */
    else if (z->health > 30)
        z->damage_level = 2;    /* wounded */
    else
        z->damage_level = 3;    /* destroyed */

    /* Check for dismemberment */
    if (z->health <= 0 && gore_detail && (int)gore_detail->value >= 2) {
        /* Full gore: sever extremities when destroyed */
        switch (zone) {
        case GORE_ZONE_HEAD:
        case GORE_ZONE_HAND_R:
        case GORE_ZONE_HAND_L:
        case GORE_ZONE_FOOT_R:
        case GORE_ZONE_FOOT_L:
        case GORE_ZONE_ARM_LOWER_R:
        case GORE_ZONE_ARM_LOWER_L:
        case GORE_ZONE_LEG_LOWER_R:
        case GORE_ZONE_LEG_LOWER_L:
            GHOUL_SeverZone(model, zone);
            break;
        default:
            break;
        }
    }

    Com_DPrintf("GHOUL: %s zone %s damaged (%d hp, level %d)\n",
        model->name, GHOUL_ZoneName(zone), z->health, z->damage_level);
}

void GHOUL_SeverZone(ghoul_model_t *model, gore_zone_id_t zone)
{
    if (zone < 0 || zone >= GORE_NUM_ZONES)
        return;

    model->zones[zone].severed = qtrue;
    model->zones[zone].health = 0;
    model->zones[zone].damage_level = 3;

    Com_DPrintf("GHOUL: %s — %s SEVERED!\n",
        model->name, GHOUL_ZoneName(zone));

    /* Severing a limb also severs everything below it */
    switch (zone) {
    case GORE_ZONE_ARM_UPPER_R:
        GHOUL_SeverZone(model, GORE_ZONE_ARM_LOWER_R);
        break;
    case GORE_ZONE_ARM_LOWER_R:
        GHOUL_SeverZone(model, GORE_ZONE_HAND_R);
        break;
    case GORE_ZONE_ARM_UPPER_L:
        GHOUL_SeverZone(model, GORE_ZONE_ARM_LOWER_L);
        break;
    case GORE_ZONE_ARM_LOWER_L:
        GHOUL_SeverZone(model, GORE_ZONE_HAND_L);
        break;
    case GORE_ZONE_LEG_UPPER_R:
        GHOUL_SeverZone(model, GORE_ZONE_LEG_LOWER_R);
        break;
    case GORE_ZONE_LEG_LOWER_R:
        GHOUL_SeverZone(model, GORE_ZONE_FOOT_R);
        break;
    case GORE_ZONE_LEG_UPPER_L:
        GHOUL_SeverZone(model, GORE_ZONE_LEG_LOWER_L);
        break;
    case GORE_ZONE_LEG_LOWER_L:
        GHOUL_SeverZone(model, GORE_ZONE_FOOT_L);
        break;
    case GORE_ZONE_NECK:
        GHOUL_SeverZone(model, GORE_ZONE_HEAD);
        break;
    default:
        break;
    }
}

/* ==========================================================================
   GHB File Loader
   ========================================================================== */

ghoul_model_t *GHOUL_LoadGHB(const char *filename)
{
    byte            *raw;
    int             len;
    ghoul_model_t   *model;
    ghb_file_header_t *hdr;

    len = FS_LoadFile(filename, (void **)&raw);
    if (!raw || len < (int)sizeof(ghb_file_header_t)) {
        Com_Printf("GHOUL_LoadGHB: can't load %s\n", filename);
        return NULL;
    }

    hdr = (ghb_file_header_t *)raw;

    /* Validate magic */
    if (LittleLong(hdr->magic) != GHB_MAGIC) {
        Com_Printf("Not a ghb file %s\n", filename);
        FS_FreeFile(raw);
        return NULL;
    }

    /* Validate version */
    if (LittleLong(hdr->version) != GHB_VERSION) {
        Com_Printf("Old file %s\n", filename);
        FS_FreeFile(raw);
        return NULL;
    }

    /* Allocate model */
    model = (ghoul_model_t *)Z_TagMalloc(sizeof(ghoul_model_t), Z_TAG_GAME);
    memset(model, 0, sizeof(ghoul_model_t));

    Q_strncpyz(model->name, filename, sizeof(model->name));

    /* Read source path from header (for debug/identification) */
    {
        int path_off = LittleLong(hdr->path_offset);
        if (path_off > 0 && path_off < len) {
            char *src = (char *)(raw + path_off);
            /* Safely copy null-terminated path */
            int maxlen = len - path_off;
            if (maxlen > (int)sizeof(model->base_dir) - 1)
                maxlen = sizeof(model->base_dir) - 1;
            int i;
            for (i = 0; i < maxlen && src[i] && src[i] >= 32; i++)
                model->base_dir[i] = src[i];
            model->base_dir[i] = 0;
        }
    }

    /* Read extended header fields if file is large enough */
    if (len >= 0x5C) {
        uint32_t *u32 = (uint32_t *)raw;
        model->num_bones = LittleLong(u32[0x44 / 4]);
        model->num_skins = LittleLong(u32[0x48 / 4]);

        float *flt = (float *)raw;
        model->bounding_radius = LittleFloat(flt[0x58 / 4]);
    }

    /* Keep the raw GHB data for mesh/animation access */
    model->ghb_data = raw;  /* Note: caller must not call FS_FreeFile */
    model->ghb_size = len;
    model->loaded = qtrue;

    /* Initialize gore zones */
    GHOUL_ResetZones(model);

    Com_Printf("GHOUL: Loaded %s (%d bones, %d skins, radius %.0f)\n",
        filename, model->num_bones, model->num_skins, model->bounding_radius);

    return model;
}

void GHOUL_FreeModel(ghoul_model_t *model)
{
    if (!model)
        return;

    if (model->ghb_data) {
        FS_FreeFile(model->ghb_data);
        model->ghb_data = NULL;
    }

    Z_Free(model);
}

/* ==========================================================================
   GPM Text Format Parser (GHOUL Player Model Definition)
   ========================================================================== */

static char *GPM_ReadLine(char **data)
{
    static char line[512];
    char *p = *data;
    int i = 0;

    if (!p || !*p)
        return NULL;

    /* Skip leading whitespace */
    while (*p && (*p == ' ' || *p == '\t'))
        p++;

    /* Read until newline */
    while (*p && *p != '\n' && *p != '\r' && i < (int)sizeof(line) - 1)
        line[i++] = *p++;

    line[i] = 0;

    /* Skip newline */
    if (*p == '\r') p++;
    if (*p == '\n') p++;

    *data = p;

    /* Strip trailing whitespace */
    while (i > 0 && (line[i-1] == ' ' || line[i-1] == '\t'))
        line[--i] = 0;

    return line;
}

qboolean GHOUL_LoadPlayerModel(const char *filename, ghoul_player_model_t *gpm)
{
    char    *raw, *data;
    int     len;
    char    *line;

    memset(gpm, 0, sizeof(ghoul_player_model_t));

    len = FS_LoadFile(filename, (void **)&raw);
    if (!raw)
        return qfalse;

    data = raw;

    /* Line 1: base model directory */
    line = GPM_ReadLine(&data);
    if (line) Q_strncpyz(gpm->base_dir, line, sizeof(gpm->base_dir));

    /* Line 2: menu animation */
    line = GPM_ReadLine(&data);
    if (line) Q_strncpyz(gpm->menu_anim, line, sizeof(gpm->menu_anim));

    /* Line 3: gameplay animation set */
    line = GPM_ReadLine(&data);
    if (line) Q_strncpyz(gpm->play_anim, line, sizeof(gpm->play_anim));

    /* Line 4: player class */
    line = GPM_ReadLine(&data);
    if (line) Q_strncpyz(gpm->player_class, line, sizeof(gpm->player_class));

    /* Line 5: team name (quoted) */
    line = GPM_ReadLine(&data);
    if (line) {
        char *start = strchr(line, '"');
        if (start) {
            start++;
            char *end = strchr(start, '"');
            if (end) *end = 0;
            Q_strncpyz(gpm->team_name, start, sizeof(gpm->team_name));
        } else {
            Q_strncpyz(gpm->team_name, line, sizeof(gpm->team_name));
        }
    }

    /* Line 6: death sound path */
    line = GPM_ReadLine(&data);
    if (line) Q_strncpyz(gpm->death_sounds, line, sizeof(gpm->death_sounds));

    /* Parse braced blocks:
     * Block 1: {} (empty or custom data)
     * Block 2: { skin assignments }
     * Block 3: { weapon bolt-ons }
     * Block 4: { cosmetic toggles }
     */
    int block = 0;
    while ((line = GPM_ReadLine(&data)) != NULL) {
        if (line[0] == '{') {
            block++;
            continue;
        }
        if (line[0] == '}') {
            continue;
        }
        if (line[0] == 0)
            continue;

        switch (block) {
        case 2: /* Skin assignments: "a a_xdekker 0" */
            if (gpm->num_skins < GHOUL_MAX_SKINS) {
                ghoul_skin_entry_t *s = &gpm->skins[gpm->num_skins];
                char part_str[8] = {0};
                char skin_str[32] = {0};
                int variant = 0;
                if (sscanf(line, "%7s %31s %d", part_str, skin_str, &variant) >= 2) {
                    s->part = part_str[0];
                    Q_strncpyz(s->skin_name, skin_str, sizeof(s->skin_name));
                    s->variant = variant;
                    gpm->num_skins++;
                }
            }
            break;

        case 3: /* Weapon bolt-ons: "boltons/menuassault.gbm wbolt_hand_r to_wbolt_hand_r 1.0" */
            if (gpm->num_boltons < GHOUL_MAX_BOLTONS) {
                ghoul_bolton_t *b = &gpm->boltons[gpm->num_boltons];
                char path[64] = {0}, bolt[32] = {0}, to[32] = {0};
                float scale = 1.0f;
                if (sscanf(line, "%63s %31s %31s %f", path, bolt, to, &scale) >= 3) {
                    Q_strncpyz(b->model_path, path, sizeof(b->model_path));
                    Q_strncpyz(b->bolt_name, bolt, sizeof(b->bolt_name));
                    Q_strncpyz(b->to_bolt, to, sizeof(b->to_bolt));
                    b->scale = scale;
                    b->active = qtrue;
                    gpm->num_boltons++;
                }
            }
            break;

        case 4: /* Cosmetic toggles: "_lbang 0" */
            if (gpm->num_cosmetics < 16) {
                char name[32] = {0};
                int value = 0;
                if (sscanf(line, "%31s %d", name, &value) >= 1) {
                    Q_strncpyz(gpm->cosmetics[gpm->num_cosmetics].name, name, 32);
                    gpm->cosmetics[gpm->num_cosmetics].value = value;
                    gpm->num_cosmetics++;
                }
            }
            break;
        }
    }

    /* Last line: description (quoted, may be empty) */
    /* Already handled in the block parsing */

    FS_FreeFile(raw);

    Com_DPrintf("GHOUL_LoadPlayerModel: %s — %s/%s class=%s team=\"%s\" "
                "skins=%d boltons=%d cosmetics=%d\n",
        filename, gpm->base_dir, gpm->play_anim, gpm->player_class,
        gpm->team_name, gpm->num_skins, gpm->num_boltons, gpm->num_cosmetics);

    return qtrue;
}

/* ==========================================================================
   M32 Texture Loader
   ========================================================================== */

byte *M32_LoadTexture(const char *filename, int *width, int *height)
{
    byte    *raw;
    int     len;
    m32_header_t *hdr;
    byte    *pixels;
    int     pixel_size;

    *width = 0;
    *height = 0;

    len = FS_LoadFile(filename, (void **)&raw);
    if (!raw || len < M32_HEADER_SIZE) {
        if (raw) FS_FreeFile(raw);
        return NULL;
    }

    hdr = (m32_header_t *)raw;

    int w = LittleLong(hdr->width);
    int h = LittleLong(hdr->height);

    if (w <= 0 || w > 4096 || h <= 0 || h > 4096) {
        Com_Printf("M32_LoadTexture: invalid dimensions %dx%d in %s\n", w, h, filename);
        FS_FreeFile(raw);
        return NULL;
    }

    /* Pixel data starts after header (at offset 0xA0 based on analysis,
     * but may vary — for now assume right after the 40-byte header + padding) */
    int data_offset = M32_HEADER_SIZE;

    /* Try offset 0xA0 first (observed in real files) */
    if (len >= 0xA0 + w * h * 4) {
        data_offset = 0xA0;
    } else if (len >= M32_HEADER_SIZE + w * h * 4) {
        data_offset = M32_HEADER_SIZE;
    } else {
        Com_DPrintf("M32_LoadTexture: %s too small for %dx%d RGBA\n", filename, w, h);
        FS_FreeFile(raw);
        return NULL;
    }

    pixel_size = w * h * 4;    /* RGBA8888 */
    pixels = (byte *)Z_Malloc(pixel_size);
    memcpy(pixels, raw + data_offset, pixel_size);

    *width = w;
    *height = h;

    FS_FreeFile(raw);

    return pixels;
}

void M32_FreeTexture(byte *data)
{
    if (data)
        Z_Free(data);
}

/* ==========================================================================
   Bolt-on Management
   ========================================================================== */

void GHOUL_AttachBolton(ghoul_model_t *model, const ghoul_bolton_t *bolton)
{
    if (model->num_boltons >= GHOUL_MAX_BOLTONS) {
        Com_Printf("GHOUL_AttachBolton: too many bolt-ons on %s\n", model->name);
        return;
    }

    model->boltons[model->num_boltons] = *bolton;
    model->boltons[model->num_boltons].active = qtrue;
    model->num_boltons++;

    Com_DPrintf("GHOUL: Attached %s to %s at %s\n",
        bolton->model_path, model->name, bolton->bolt_name);
}

void GHOUL_DetachBolton(ghoul_model_t *model, int index)
{
    if (index < 0 || index >= model->num_boltons)
        return;

    Com_DPrintf("GHOUL: Detached %s from %s\n",
        model->boltons[index].model_path, model->name);

    /* Shift remaining bolt-ons down */
    for (int i = index; i < model->num_boltons - 1; i++)
        model->boltons[i] = model->boltons[i + 1];

    model->num_boltons--;
}
