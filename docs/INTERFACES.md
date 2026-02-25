# Module Interface Analysis

Cross-referenced against the [Quake II GPL source](https://github.com/id-Software/Quake-2).

## Architecture Overview

SoF follows the Quake II pluggable module pattern but with significantly expanded interfaces:

```
Engine (SoF.exe)
  │
  ├──► GetGameAPI(game_import_t*) → game_export_t*     [gamex86.dll]
  │      Q2: 44 imports, 20 exports
  │      SoF: 99 imports, 22+ exports
  │
  ├──► GetRefAPI(refexport_t* out, refimport_t*) → refexport_t*   [ref_gl.dll]
  │      Q2: 16 imports, 21 exports
  │      SoF: 27 imports, 54 exports  (DIFFERENT calling convention!)
  │
  └──► GetPlayerClientAPI / GetPlayerServerAPI          [player.dll]
         SoF-specific, no Q2 equivalent
```

---

## game_import_t — Engine → Game Interface

**Global variable:** `gi` at `0x50140820` (.data section of gamex86.dll)
**Size:** 99 DWORDs = 396 bytes (0x18C)
**Q2 comparison:** 44 fields → 99 fields (+55 SoF/GHOUL additions)

The engine passes this struct of function pointers to the game DLL. The game DLL copies it
into a static `gi` global via `REP MOVSD` in `GetGameAPI`.

### Q2-Compatible Fields (offsets 0x000–0x0AC)

| Offset | Index | Q2 Name | Refs | Notes |
|--------|-------|---------|------|-------|
| 0x000 | 0 | `bprintf` | 3 | Broadcast print |
| 0x004 | 1 | `dprintf` | 259 | Debug/developer print (most-called) |
| 0x008 | 2 | `cprintf` | 24 | Client-specific print |
| 0x00C | 3 | `centerprintf` | 1 | Center-screen message |
| 0x010 | 4 | `sound` | 2 | Basic sound emit |
| 0x014 | 5 | `positioned_sound` | 3 | Positional audio |
| 0x024 | 9 | `soundindex` | 1 | Register sound |
| 0x028 | 10 | `imageindex` | 129 | Register image (heavy UI usage) |
| 0x02C | 11 | `setmodel` | 10 | Set entity model |
| 0x030 | 12 | `trace` | 95 | Collision trace |
| 0x034 | 13 | `pointcontents` | 26 | Check point contents |
| 0x038 | 14 | `inPVS` | 1 | PVS visibility check |
| 0x044 | 17 | `AreasConnected` | 4 | Area portal check |
| 0x054 | 21 | `Pmove` | 35 | Player movement |
| 0x084 | 33 | `TagMalloc` | 7 | Managed memory alloc |
| 0x088 | 34 | `TagFree` | 1 | Managed memory free |
| 0x08C | 35 | `FreeTags` | 41 | Free tagged memory |
| 0x094 | 37 | `cvar_set` | 8 | Set cvar by name |
| 0x09C | 39 | `argc` | 175 | Command argument count |
| 0x0A0 | 40 | `argv` | 20 | Command argument string |
| 0x0A4 | 41 | `args` | 7 | All args concatenated |
| 0x0AC | 43 | `DebugGraph` | 1 | Debug visualization |

### SoF-Specific Fields (offsets 0x0B0–0x188)

| Offset | Index | Refs | Tentative Name | Notes |
|--------|-------|------|----------------|-------|
| 0x0B0 | 44 | 54 | `AddCommandString` / `Cmd_TokenizeString` | |
| 0x05C | 23 | 173 | `sound_extended` | 7 args — SoF extended sound with extra params |
| 0x0E4 | 57 | 90 | `cvar_register` | Primary cvar registration, used for all 85 cvars |
| 0x0E8 | 58 | 20 | `cvar_set_string` | 2 args: name, string value |
| 0x0EC | 59 | 26 | `cvar_set_float` | 2 args: name, float value |
| 0x11C | 71 | 2 | `cvar_get` | Used in GetGameAPI: `cvar("ghl_mip","0",1,0)` |
| 0x150 | 84 | 149 | `ghoul_func_1` | GHOUL model function, 3-4 args, very high usage |
| 0x154 | 85 | 138 | `ghoul_func_2` | GHOUL model function, first arg often "AIAI" tag |
| 0x168 | 90 | 66 | `entity_flags` | 2 args: entity + flags |
| 0x184 | 97 | 215 | `flrand` | Float random(min, max) |
| 0x188 | 98 | 255 | `irand` | Integer random(min, max), most-called SoF function |

---

## game_export_t — Game → Engine Interface

**Static address:** `0x50140728` (.data section of gamex86.dll)
**API version:** 3 (same as Q2)

### Fields

| Offset | Q2 Name | SoF Function Address | Notes |
|--------|---------|---------------------|-------|
| +0x00 | `apiversion` | 3 (literal) | Integer, not a pointer |
| +0x04 | `Init` | `0x50094590` | Prints `"==== InitGame ===="`, registers 85 cvars |
| +0x08 | `Shutdown` | `0x50095010` | Prints `"==== ShutdownGame ===="` |
| +0x0C | `SpawnEntities` | `0x500A59A0` | Map loading |
| +0x10 | `WriteGame` | `0x500A2E90` | Save game state |
| +0x14 | `ReadGame` | `0x500A2F80` | Load game state |
| +0x18 | `WriteLevel` | `0x500A3260` | Save level state |
| +0x1C | `ReadLevel` | `0x500A3350` | Load level state |
| +0x20 | `ClientConnect` | `0x500DE060` | Player connection |
| +0x24 | `ClientDisconnect` | `0x500DD360` | Player disconnect |
| +0x28 | `ClientBegin` | `0x500DDA40` | Player spawn |
| +0x2C | `ClientUserinfoChanged` | `0x500DE390` | Userinfo update |
| +0x30 | `ClientCommand` | `0x50072610` | Client console command |
| +0x34 | `ClientThink` | `0x500DE680` | Per-frame client input |
| +0x38 | `RunFrame` | `0x500960E0` | Server frame tick |
| +0x3C | `ServerCommand` | `0x50096160` | `sv <command>` handling |
| +0x40 | *(SoF)* `GetGameTime` | `0x50095050` | Time calculations / stats |
| +0x44 | *(SoF)* `RegisterWeapons` | `0x50095280` | Registers weapon strings |
| +0x48 | *(SoF)* `GetGameVersion` | `0x500953D0` | Returns version string |
| +0x4C | *(SoF)* `GetCheatsEnabled` | `0x50095040` | Reads cheat flag at `0x501416E5` |
| +0x50 | *(SoF)* `SetCheatsEnabled` | `0x50095030` | Writes cheat flag |
| +0x54 | *(SoF)* `RunAI` | `0x50095D20` | AI/game tick logic |
| +0x5C | `edict_size` | 1104 (0x450) | Q2 was ~800 bytes |

### Registered Weapons (from `RegisterWeapons` at 0x50095280)

```
knife, sniper, assault, mpistol, slugger, flamegun, fpak,
goggles, machinegun, shotgun, pistol1, pistol2, rocket,
mpg, grenade, medkit, c4
```

### Key CVars (85 total, registered in Init)

**Physics/Movement:**
`sv_rollspeed`, `sv_rollangle`, `sv_maxvelocity`, `sv_gravity` (+ x/y/z)

**AI:**
`ai_freeze`, `ai_goretest`, `ai_pathtest`, `ai_dumb`, `ai_maxcorpses`

**GHOUL Engine:**
`ghl_specular`, `ghl_light_method`, `ghl_precache_verts`, `ghl_precache_texture`, `ghl_mip`

**Game:**
`dedicated`, `cheats`, `gamename` (="base"), `gamedate` (="Mar 10 2000")
`maxclients`, `maxspectators`, `deathmatch`, `maxentities` (=1024)
`dmflags`, `fraglimit`, `timelimit`, `skill`, `freezeworld`

**CTF:**
`ctf_loops`, `ctf_team_red` (="MeatWagon"), `ctf_team_blue` (="The Order")

---

## refimport_t — Engine → Renderer Interface

**Static address:** `0x3008CBE8` (.data/BSS section of ref_gl.dll)
**Size:** 27 DWORDs = 108 bytes (0x6C)
**Q2 comparison:** 16 fields → 27 fields (+11 SoF additions)

### Fields

| Index | Q2 Name | Xrefs | Notes |
|-------|---------|-------|-------|
| 0 | `Sys_Error` | — | Error reporting |
| 1 | `Cmd_AddCommand` | — | Register console command |
| 2 | `Cmd_RemoveCommand` | — | Unregister command |
| 3 | `Cmd_Argc` | — | Command arg count |
| 4 | `Cmd_Argv` | — | Command arg string |
| 5 | `Cmd_ExecuteText` | — | Execute command string |
| 6 | `Con_Printf` | — | Console print |
| 7 | `FS_LoadFile` | — | Load file from PAK/dir |
| 8 | `FS_FreeFile` | 90 | Free loaded file (most-called) |
| 9 | `FS_Gamedir` | — | Get game directory path |
| 10 | `Cvar_Get` | — | Get/create cvar |
| 11 | `Cvar_Set` | — | Set cvar string |
| 12 | `Cvar_SetValue` | 107 | Set cvar float (most-called) |
| 13 | `Vid_GetModeInfo` | — | Get resolution for mode |
| 14 | `Vid_MenuInit` | — | Video menu init |
| 15 | `Vid_NewWindow` | — | Window size changed |
| 16-19 | *(Q2 end)* | — | — |
| 20 | `Z_Malloc` | — | SoF: Memory alloc |
| 21 | `Z_Free` | — | SoF: Memory free |
| 22 | `Z_Realloc` | — | SoF: Memory realloc |
| 23 | `Z_MemInfo` | — | SoF: Memory stats |
| 24 | `Z_TagMalloc` | — | SoF: Tagged alloc |
| 25 | `Z_TagFree` | — | SoF: Tagged free |
| 26 | `Sys_GetTime` | — | SoF: Timing function |

---

## refexport_t — Renderer → Engine Interface

**Size:** 54 DWORDs = 216 bytes (0xD8)
**API version:** 3
**Q2 comparison:** 21 fields → 54 fields (+33 SoF additions)

**IMPORTANT — Calling convention differs from Q2:**
```c
// Quake II:
refexport_t GetRefAPI(refimport_t rimp);           // returns struct by value

// Soldier of Fortune:
refexport_t* GetRefAPI(refexport_t* out, refimport_t* rimp);  // hidden pointer return
```

### Fields

| Offset | Index | Q2 Name | SoF Function | Notes |
|--------|-------|---------|-------------|-------|
| +0x000 | 0 | `api_version` | 3 (literal) | |
| +0x004 | 1 | `Init` | `0x3000F810` | Refs `gl_vendor`, `gl_renderer` |
| +0x008 | 2 | `Shutdown` | `0x30010F40` | Refs `screenshot`, `imagelist` |
| +0x00C | 3 | *(SoF)* `ChangeDisplaySettings` | `0x30010FC0` | Mode switching |
| +0x010 | 4 | `BeginRegistration` | `0x3000AC80` | Refs `maps/%s.bsp` |
| +0x014 | 5 | `RegisterModel` | `0x3000BA40` | |
| +0x018–0x02C | 6-11 | `RegisterSkin`..`DrawGetPicSize` | various | Standard asset loading |
| +0x030 | 12 | `DrawPic` | `0x30019760` | |
| +0x034 | 13 | `DrawStretchPic` | `0x30016E80` | **STUB — no-op (single RET)** |
| +0x038 | 14 | `DrawChar` | `0x30016D30` | |
| +0x03C | 15 | `DrawTileClear` | `0x3000BAD0` | |
| +0x040–0x058 | 16-22 | `DrawFill`..`AppActivate` | various | Standard Q2 rendering |
| +0x05C | 23 | *(SoF)* `SetMode` | — | Window/display mode |
| +0x060 | 24 | *(SoF)* `SetMode2` | — | |
| +0x064 | 25 | *(reserved)* | NULL | Unused slot |
| +0x068 | 26 | *(SoF)* `SetGamma` | `0x30001A90` | Gamma ramp control |
| +0x06C | 27 | *(SoF)* `GetRefConfig` | — | Renderer configuration |
| +0x070 | 28 | *(SoF)* `SetViewport` | — | GL viewport |
| +0x074 | 29 | *(SoF)* `SetScissor` | — | GL scissor test |
| +0x078 | 30 | *(SoF)* `SetOrtho` | — | Orthographic projection |
| +0x07C | 31 | *(reserved)* | NULL | Unused slot |
| +0x080 | 32 | *(SoF)* `DrawStretchPic2` | — | Extended 2D draw |
| +0x084 | 33 | *(SoF)* `DrawSetColor` | — | Set draw color |
| +0x088–0x098 | 34-38 | *(SoF)* `DrawFillRect`..`DrawBox` | — | Extended 2D primitives |
| +0x09C | 39 | *(SoF)* `DrawModel` | — | 3D model rendering (GHOUL) |
| +0x0A0 | 40 | *(SoF)* `GetLightLevel` | — | Light query |
| +0x0A4 | 41 | *(SoF)* `GetModelBounds` | — | AABB query |
| +0x0A8 | 42 | *(SoF)* `InitFonts` | `0x30002FE0` | Refs `small` |
| +0x0AC | 43 | *(SoF)* `GLimp_Init` | `0x30011030` | GL context creation |
| +0x0B0–0x0B8 | 44-46 | *(SoF)* `GLimp_SetState`.. | — | GL state management |
| +0x0BC | 47 | *(SoF)* `AddParticleEffect` | `0x30007530` | Particle system |
| +0x0C0 | 48 | *(SoF)* `SetGLFunction` | `0x3001C250` | GL function dispatch |
| +0x0C4–0x0CC | 49-51 | *(SoF)* `DrawShadow`..`DrawDecal2` | — | Shadow/decal rendering |
| +0x0D0 | 52 | *(SoF)* `ReadPixels` | `0x30012010` | `glReadPixels(GL_RGB)` |
| +0x0D4 | 53 | *(SoF)* `ClearScene` | `0x3001D0B0` | Clears 0xBCC bytes |

---

## OpenGL Requirements

The renderer loads **all** OpenGL functions dynamically via `GetProcAddress` / `wglGetProcAddress`.

### Required OpenGL Version: 1.1

336 core GL 1.1 functions loaded into a function pointer table at startup.

### Required Extensions

| Extension | Functions | Purpose |
|-----------|-----------|---------|
| **GL_ARB_multitexture** | `glActiveTextureARB`, `glClientActiveTextureARB`, `glMultiTexCoord2fARB`, `glMultiTexCoord2fvARB` | Multi-texture blending |
| **GL_SGIS_multitexture** | `glMTexCoord2fSGIS`, `glMTexCoord2fvSGIS`, `glSelectTextureSGIS` | Legacy fallback for above |
| **GL_EXT_compiled_vertex_array** | `glLockArraysEXT`, `glUnlockArraysEXT` | Vertex array optimization |
| **GL_S3_s3tc** | *(format enums only)* | S3TC texture compression |

### S3TC Texture Formats

```
GL_RGB_S3TC, GL_RGBA_S3TC, GL_RGB4_S3TC, GL_RGBA4_S3TC
```
Note: These are the original S3 extension formats, predating `GL_EXT_texture_compression_s3tc` (DXT).

### WGL Functions (22)

Standard WGL context management plus `wglSwapIntervalEXT` for VSync control.
Also uses `wglUseFontBitmapsA` / `wglUseFontOutlinesA` for text rendering.

---

## player.dll Interface

SoF-specific module with no Q2 equivalent.

### Exports
```
GetPlayerClientAPI  → 0x40001020
GetPlayerServerAPI  → 0x400010D0
```

### Imports
Only `KERNEL32.dll` — all game interaction through function pointer tables (same pattern as gamex86.dll).

---

## Key Differences from Quake II Summary

| Aspect | Quake II | Soldier of Fortune |
|--------|----------|-------------------|
| game_import_t fields | 44 | 99 (+125%) |
| game_export_t fields | 20 | 22+ |
| refimport_t fields | 16 | 27 (+69%) |
| refexport_t fields | 21 | 54 (+157%) |
| GetRefAPI convention | Returns struct by value | Hidden pointer param |
| edict_size | ~800 bytes | 1104 bytes |
| Sound function args | 5 | 7 (extended params) |
| Random functions | None in import | `flrand(min,max)`, `irand(min,max)` |
| GHOUL functions | N/A | 2+ high-usage functions in gi |
| Player module | N/A | Separate player.dll |
| Weapon registration | N/A | 17 weapon types |
| CVars at init | ~20 | 85 |
