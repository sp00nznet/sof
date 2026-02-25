# Asset Structure Analysis

## PAK File Overview

| PAK File | Size | Files | Contents |
|----------|------|-------|----------|
| `pak0.pak` | 673 MB | 9,728 | Main game data |
| `pak1.pak` | 19.6 MB | 533 | Supplemental sound (all `sound/`) |
| **Total** | **692 MB** | **10,261** | No duplicates between PAKs |

Standard Quake II PAK format: `"PACK"` magic, directory offset, directory size.
Each entry: 56-byte null-terminated filename + 4-byte offset + 4-byte size.

## File Types (27 unique extensions)

| Extension | Count | Size | % | Description |
|-----------|-------|------|---|-------------|
| `.ghb` | 477 | 187 MB | 27.1% | GHOUL animation/model bundles |
| `.bsp` | 52 | 169 MB | 24.4% | Compiled map files |
| `.m32` | 4,199 | 162 MB | 23.4% | MIP32 textures (SoF-proprietary) |
| `.adp` | 747 | 81 MB | 11.8% | ADPCM compressed audio |
| `.wav` | 1,000 | 56 MB | 8.1% | Uncompressed audio |
| `.tga` | 1,123 | 26 MB | 3.7% | Targa textures |
| `.nvb` | 26 | 6.4 MB | 0.9% | Navigation/visibility data |
| `.raw` | 32 | 2.0 MB | 0.3% | Raw image data (skies) |
| `.os` | 727 | 699 KB | 0.1% | Objective scripts (Designer Script) |
| `.sp` | 136 | 606 KB | 0.1% | String packs (localization) |
| `.rof` | 312 | 352 KB | — | ROFF motion files |
| `.eft` | 211 | 301 KB | — | Effect definitions |
| `.rmf` | 408 | 227 KB | — | Animation reference files |
| `.gsq` | 308 | 139 KB | — | GHOUL sequence files |
| `.eal` | 52 | 78 KB | — | Entity/AI layout per map |
| `.ifl` | 252 | 17 KB | — | Image file lists (animated tex) |
| `.gpm` | 50 | 18 KB | — | GHOUL player model defs |
| `.cfg` | 58 | 35 KB | — | Configuration files |
| `.gbm` | 33 | 3 KB | — | GHOUL bolt-on model defs |
| `.dms` | 30 | 4 KB | — | Deathmatch spawn files |

**Key finding:** SoF does NOT use standard Quake II model formats (.md2/.md3). The entire
model pipeline is proprietary GHOUL — `.ghb`, `.gsq`, `.rmf`, `.gpm`, `.gbm`.

Similarly, textures use `.m32` (MIP32) instead of Q2's `.wal` format.

## Directory Structure

### textures/ (3,990 files)
M32 and TGA textures organized by level theme:
`iraq/`, `tokyo/`, `siberia/`, `sudan/`, `castle/`, `newyork/`, `uganda/`, `bosnia/`, `kosovo/`,
`common/`, `sky/`, `generic/`, `special/`

### ghoul/ (2,301 files) — The GHOUL Model System

The heart of SoF's proprietary character system:

**Enemy models:**
| Directory | Files | Description |
|-----------|-------|-------------|
| `ghoul/enemy/meso/` | 345 | Mesomorph humanoid — the main dismemberment-capable system |
| `ghoul/enemy/bolt/` | 215 | Bolt-on parts (heads, arms, legs, torsos) for gore |
| `ghoul/enemy/ecto/` | 61 | Ectomorph (thin) body type |
| `ghoul/enemy/female/` | 61 | Female enemy characters |
| `ghoul/enemy/chopper/` | 33 | Helicopter |
| `ghoul/enemy/hind/` | 9 | Hind helicopter |
| `ghoul/enemy/tank/` | 14 | Tank vehicle |
| `ghoul/enemy/rottweiler/` | 5 | Attack dog |
| `ghoul/enemy/snowcat/` | 8 | Snowcat vehicle |
| `ghoul/enemy/bull/` | 5 | Bull |

MESO bundles are the largest files in the game (4–7.5 MB each), one per level:
`MESO_IRQ1A.ghb`, `MESO_NYC2.ghb`, `MESO_KOS1.ghb`, etc.

**Weapons (first-person view):**
`ghoul/weapon/inview/` — 14 weapon directories:
assaultrifle, autoshotgun, flamegun, knife, machinegun, mpg (microwave pulse gun),
mpistol, pistol1, pistol2, rocket, shotgun, sniperrifle, hurthand, throwhand

**Player models:** `ghoul/pmodels/` — 52 models + 33 bolt-ons + 50 portraits + 6 team icons
**World objects:** `ghoul/objects/` — Per-region objects (generic, iraq, newyork, siberia, etc.)
**Items:** `ghoul/items/` — Ammo, armor, health, medkits, CTF flags, projectiles
**Menu 3D elements:** `ghoul/menus/` — 93 files

### sound/ (1,778 files)
Split between `.wav` (1,000 files, 56 MB) and `.adp` (747 files, 81 MB):
- `sound/Cin/` — Cinematic dialogue per level
- `sound/Enemy/Dth/` — Death sounds by ethnicity (Arab, Fem, Jpn, Rad, Russ, Skin) × 28 variants
- `sound/ambient/` — Environmental sounds (doors, machinery, weather)
- `sound/weapons/` — Weapon effects (fire, reload, ricochet)
- `sound/Enemy/Hawk1/`, `Sam1/`, `NPC1/` — Named character dialogue

### ds/ (1,039 files) — Designer Script System
- **727 `.os` files** — Objective scripts (level events, cinematics, NPC behavior)
- **312 `.rof` files** — ROFF motion data (camera paths, vehicle motion, destruction)
- Organized per-map: `ds/irq1a/`, `ds/nyc1/`, `ds/kos2/`, etc.

### maps/ (162 files)
**30 single-player maps** across the campaign:

| Code | Location | Maps |
|------|----------|------|
| `irq` | Iraq | 6 (irq1a, irq1b, irq2a, irq2b, irq3a, irq3b) |
| `nyc` | New York City | 3 |
| `kos` | Kosovo | 3 |
| `sib` | Siberia | 3 |
| `sud` | Sudan | 3 |
| `jpn` | Japan/Tokyo | 3 |
| `ger` | Germany/Castle | 4 |
| `arm` | Armenia | 3 |
| `tsr` | Tokyo Street | 2 |
| `trn` | Train | 1 |
| `tut` | Tutorial | 1 |

**22 multiplayer maps** in `maps/dm/` (deathmatch + CTF)

Each map has companions: `.eal` (entity/AI layout), `.nvb` (navigation), `.dms` (DM spawns)

### effects/ (211 files)
`.eft` effect definitions:
- `effects/weapons/playermz/` — 36 player muzzle flash effects
- `effects/weapons/othermz/` — 36 enemy muzzle flash effects
- `effects/weapons/world/` — 35 weapon impact effects
- `effects/environ/` — 63 environmental effects
- `effects/gore/` — 15 gore/dismemberment effects

### drivers/ (32 files)
GPU-specific config profiles — a time capsule of late-90s hardware:
Voodoo1/2/3, Riva 128, TNT1/2, Rage Pro/128/MAXX, Savage 3/4/2000, i740,
Banshee, GeForce, G200/G400, plus CPU tier and memory tier configs.

## SoF-Proprietary File Formats (need reverse engineering)

| Format | Ext | Priority | Notes |
|--------|-----|----------|-------|
| GHOUL Model Bundle | `.ghb` | High | Character models with dismemberment zones |
| MIP32 Texture | `.m32` | High | 32-bit mipmapped textures, replaces Q2 .wal |
| GHOUL Sequence | `.gsq` | Medium | Animation sequences |
| GHOUL Animation Ref | `.rmf` | Medium | Animation reference/mapping |
| GHOUL Player Model | `.gpm` | Medium | Player model definitions |
| GHOUL Bolt-on Model | `.gbm` | Medium | Bolt-on attachment definitions |
| Objective Script | `.os` | Medium | Designer Script game logic |
| ROFF Motion | `.rof` | Medium | Entity motion paths |
| Effect Definition | `.eft` | Low | Particle/visual effects |
| String Pack | `.sp` | Low | Localized text |
| Navigation Data | `.nvb` | Low | AI pathfinding |
| Entity/AI Layout | `.eal` | Low | Per-map entity placement |
| ADPCM Audio | `.adp` | Low | Compressed audio (standard ADPCM) |
