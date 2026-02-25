# SoF Proprietary File Format Analysis

Reverse-engineered from binary headers of actual game assets inside pak0.pak.

## GHB — GHOUL Model Bundle

**Magic:** `0x198237FE` (4 bytes)
**Count:** 480 files, 71 bytes – 7.8 MB

The primary GHOUL model/animation container format. Each GHB is a self-contained bundle
for a character type per level (e.g., `MESO_IRQ3B.ghb` = mesomorph enemies for Iraq level 3B).

### Header Structure

```c
struct ghb_header {
    uint32_t magic;         // +0x00: 0x198237FE
    uint32_t data_offset;   // +0x04: offset to data section (varies)
    uint32_t total_size;    // +0x08: file size or data block size
    uint32_t checksum;      // +0x0C: 0x033FB1A3 (constant across files — format version?)
    uint32_t header_size;   // +0x10: 0x4A (74) — size of this header
    uint32_t unknown_14;    // +0x14: 0
    uint32_t path_offset;   // +0x18: 0x28 (40) — offset to source path string
    // +0x1C: null-terminated source path string from build machine
    //   Small files: "p:/base/GHOUL/ENEMY/BOLT/FADE_ROCKET.GHL"
    //   Large files: "C:\SOF\!GPREP!\ENEMY\MESO\MESO_IRQ3B.GFLA"
};
// After header: model name string, followed by animation/mesh data
```

**Key observations:**
- Source paths from Raven's build machine are embedded: `p:/base/GHOUL/` and `C:\SOF\!GPREP!\`
- The `!GPREP!` directory suggests a preprocessing tool for GHOUL data
- Two source extensions: `.GHL` (small bolt-on models) and `.GFLA` (large animated models)
- Field at +0x44 appears to be model/bone count (e.g., 11 for simple bolt-ons, 3393 for full MESO)
- Field at +0x48 appears to be skin/material count (1 for bolt-ons, 202 for full MESO)
- Field at +0x58 is a float: 100.0 for bolt-ons, 200.0 for MESO — possibly scale or bounding radius
- After the header, animation names appear as null-terminated strings (e.g., `CCH_A_FWD_MS_2`, `FADE_ROCKET`)

**Error strings from GHOUL_LoadGHB (at 0x000A8C50):**
- `"Old file %s"` — version check
- `"Not a ghb file %s"` — magic validation
- `"Thunks are obsolete %s"` — deprecated feature check

## M32 — MIP32 Texture

**Magic:** `0x00000028` (40 decimal) — appears to be header size
**Count:** 4,199 files, 120 bytes – 786 KB

SoF-proprietary 32-bit mipmapped texture format, replacing Quake II's `.wal`.

### Header Structure

```c
struct m32_header {
    uint32_t header_size;   // +0x00: 0x28 (40) — header size
    uint32_t width;         // +0x04: texture width (e.g., 128)
    uint32_t height;        // +0x08: texture height (usually 1 — may be flag)
    uint32_t unknown_0C;    // +0x0C: 0
    float    unknown_10[3]; // +0x10: three identical floats (e.g., 9999.0)
    // +0x1C onwards: zeros, then mipmap data
    // Data at +0xA0: repeating 0x3E803E80 pattern (pixel data)
};
```

**Key observations:**
- Header size 40 bytes is constant across all samples
- Field at +0x04 matches expected texture dimensions (64, 128, 256)
- Three identical floats at +0x10/+0x14/+0x18 may be lighting/LOD parameters
- Pixel data appears to start around offset 0xA0 (160 bytes into file)
- Need to determine: RGBA8888? BGRA? Compressed?

## ROFF — Rotation Object File Format

**Magic:** `"ROFF"` (`0x46464F52`)
**Count:** 312 files, 204 bytes – 53 KB

Movement animation data for scripted entity motion (camera paths, vehicles, doors).

### Header Structure

```c
struct roff_header {
    char     magic[4];      // +0x00: "ROFF"
    uint32_t version;       // +0x04: 1
    float    duration;      // +0x08: total duration in frames (e.g., 55.0)
    // +0x0C: 20 bytes of zeros (padding/reserved)
    // +0x20: padding to 0x24
    // +0x24 onwards: array of keyframes
};

struct roff_keyframe {
    float    origin[3];     // position (x, y, z)
    float    angles[3];     // rotation (pitch, yaw, roll)
};
// Keyframe size: 24 bytes (6 floats)
```

**Key observations:**
- Version is always 1
- Duration at +0x08 is a float representing frame count
- Keyframes are 24 bytes each: 3 floats position + 3 floats rotation
- File size = header + (num_keyframes × 24)
- Keyframe count = (file_size - 36) / 24

## EFT — Effect Definition

**Format:** Plain text
**Count:** 211 files

```
EFFECT  weaponmz_pistol1_shell       // effect name
{
    //  Ejected shell casing           // comments supported
    PARTICLE  pistol_shell             // particle name
    {
        flags   useAlpha               // rendering flags
        spawnflags ...
        type    sprite                 // particle type
        ...
    }
}
```

Text-based effect definitions with hierarchical EFFECT → PARTICLE structure.
Each particle has properties for type, flags, spawn behavior, physics, etc.

## OS — Objective Script (Designer Script)

**Format:** Plain text
**Count:** 727 files

```
rem *** IRQ1A_intro ***
rem *** Intro Cinematic for IRQ1A ***

camera_enable

set lobj1 "irq1a_intro_cam1"
set lobj2 "irq1a_intro_look1"
...
camera_move %lobj1% 0.1
camera_lookat %lobj2% 1.0
...
wait 3.0
camera_disable
```

Custom scripting language with commands for:
- Camera control (`camera_enable`, `camera_move`, `camera_lookat`)
- Variable management (`set`, `%var%` expansion)
- Flow control (`wait`, `rem` comments)
- Entity manipulation, cinematic triggers

## GPM — GHOUL Player Model Definition

**Format:** Plain text
**Count:** 50 files, 305–475 bytes

```
enemy/meso                           // base model directory
meso_menu1                           // menu pose animation
meso_player                          // gameplay animation set
playerboss                           // player class
"The Order"                          // default team name
enemy/dth/rad                        // death sound set
{}                                   // (empty block)
{                                    // skin assignments
a a_xdekker 0                        // part letter, skin name, variant
b b_xdekker 0
f f_dekker 0
h h_dekker 0
}
{                                    // weapon bolt-on
boltons/menuassault.gbm wbolt_hand_r to_wbolt_hand_r 1.0
}
{                                    // cosmetic toggles
_lbang 0
_rbang 0
_ponytail 0
_mohawk 0
}
""                                   // description
```

Defines a player model with: base model path, animation sets, team assignment,
death sounds, skin mappings, weapon attachments, and cosmetic part toggles.

## GBM — GHOUL Bolt-on Model Definition

**Format:** Plain text
**Count:** 33 files, 54–126 bytes

```
enemy/bolt                           // bolt-on model directory
w_pistol2                            // model name
""                                   // (unused)
w_pistol2                            // render model
""                                   // (unused) × 3
""
""

{}                                   // skin overrides (optional)
```

Defines attachable model parts (weapons, helmets, accessories).

## SP — String Pack (Localization)

**Format:** Plain text
**Count:** 136 files

```
VERSION 1
ID 224
REFERENCE MENU_SUD1
DESCRIPTION "SUD1 Menu Strings"
COUNT 9
INDEX 0
{
   REFERENCE MISTITLE
   TEXT_ENGLISH "MISSION: DRAGONFIRE"
   TEXT_GERMAN "MISSION: DRACHENFEUER"
   TEXT_FRENCH "MISSION : DRAGONFIRE"
}
```

Localization system supporting English, German, and French.
Each pack has a version, ID, reference name, and indexed string entries.

## EAL — Entity/AI Layout

**Magic:** `"RIFF"` — Standard RIFF container
**Count:** 52 files, ~1.5 KB each

```
RIFF <size> "eal "
  "majv" <4> <2>           // major version = 2
  "minv" <4> <0>           // minor version = 0
  "exe " <260> "C:\projects\sof\sh_debug\SoF.exe"   // build exe path
  ...
```

RIFF-based format containing entity and AI placement data per map.
Embeds the build executable path: `C:\projects\sof\sh_debug\SoF.exe` — confirms the
source tree at `C:\projects\sof\` and a debug build configuration (`sh_debug`).

## NVB — Navigation/Visibility Data

**Binary format, no recognizable magic**
**Count:** 26 files, 37 KB – 825 KB

Appears to be AI pathfinding navigation mesh data. Header contains float values
that look like bounding box coordinates. Structure TBD.

## ADP — ADPCM Audio

**No magic header — raw data**
**Count:** 747 files, 1.3 KB – 6.5 MB

First 4 bytes appear to be the sample rate as uint32: `0x00005622` = 22050 Hz.
Followed immediately by compressed ADPCM audio data.
Likely IMA ADPCM or MS ADPCM at 22050 Hz mono.

## GSQ — GHOUL Sequence

**Binary format**
**Count:** 308 files

Animation sequence definitions for the GHOUL model system.
Links animation names to frame ranges within GHB bundles.

## RMF — Animation Reference

**Binary format with possible text**
**Count:** 408 files

Animation reference/mapping files. May define which animations
are available for each model and how they map to game states.

## Source Path Summary

Multiple formats leak Raven's build environment paths:

| Source | Path |
|--------|------|
| GHB headers | `p:/base/GHOUL/ENEMY/...` |
| GHB headers | `C:\SOF\!GPREP!\ENEMY\...` |
| EAL files | `C:\projects\sof\sh_debug\SoF.exe` |
| WON SDK (SoF.exe) | `D:\projects\sof\code2\wonsdk\API\...` |

This reveals at least three development machines/paths:
1. `p:/base/` — Network drive for game assets
2. `C:\SOF\!GPREP!\` — Local GHOUL preprocessing workspace
3. `C:\projects\sof\` / `D:\projects\sof\code2\` — Source code trees
