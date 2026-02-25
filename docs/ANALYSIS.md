# Binary Analysis Notes

## Target Executables — v1.00 (Retail Disc)

All binaries compiled **2000-03-10** with **MSVC 6.0** (linker 6.0), 32-bit x86 PE32.

### SoF.exe (Main Engine)

| Property | Value |
|----------|-------|
| Size | 2,052,096 bytes |
| Image Base | `0x20000000` |
| Entry Point | `0x0015324F` |
| Code Size | 1,564,672 bytes (.text) |
| Data Size | ~3.7MB virtual (.data, mostly BSS) |
| Subsystem | Windows GUI |
| Sections | `.text` `.rdata` `.data` `.rsrc` |

**Exports (24 functions):**
The engine exports functions for use by game DLLs:
```
CL_GetEntitySoundOrigin    Cmd_AddCommand    Cmd_Argc          Cmd_Argv
Cmd_RemoveCommand          Com_DPrintf       Com_Error         Com_Printf
Com_sprintf                Cvar_Get          Cvar_Set          Cvar_SetValue
FS_FCloseFile              FS_FOpenFile      FS_FreeFile       FS_LoadFile
Sys_Error                  VectorLength      VectorNormalize   Z_Free
Z_Malloc                   Z_Touch           flrand            irand
```
These are classic Quake II engine API functions — confirms the id Tech 2 heritage.

**Imports:**
| DLL | Functions | Purpose |
|-----|-----------|---------|
| KERNEL32.dll | 117 | OS fundamentals (file I/O, memory, process) |
| USER32.dll | 32 | Window management, input, clipboard |
| WSOCK32.dll | 21 (by ordinal) | Networking (Winsock 1.1) |
| FFC10.dll | 42 (C++ mangled) | Force Feedback — Immersion/FeelIT mouse support |
| WINMM.dll | 7 | Timing (`timeGetTime`), joystick, CD audio (`mciSendCommand`) |
| ADVAPI32.dll | 9 | Registry access |
| ole32.dll | 3 | COM (`CoCreateInstance`) |
| VERSION.dll | 2 | File version queries |

**Key observations:**
- No direct DirectInput import — input likely goes through FFC10.dll (FeelIT/force feedback) or raw Win32
- No direct OpenGL imports — renderer is fully isolated in `ref_gl.dll`
- No DirectSound imports — sound DLLs are separate (`EAXSnd.dll`, `A3Dsnd.dll`, `Defsnd.dll`)
- Uses Winsock 1.1 (`WSOCK32.dll`), not Winsock 2
- CD audio via `mciSendCommandA` — will need redbook audio emulation
- Registry used for settings/installation paths
- Image base `0x20000000` is non-standard (not the default `0x00400000`)

### gamex86.dll (Game Logic)

| Property | Value |
|----------|-------|
| Size | 1,421,312 bytes |
| Image Base | `0x50000000` |
| Entry Point | `0x000F19AA` |
| Code Size | 1,036,288 bytes (.text) |
| Sections | `.text` `.rdata` `.data` `.reloc` |
| Has Relocations | Yes (0x1AD1C bytes) |

**Exports:**
```
GetGameAPI  → 0x000953F0
```
Single export — standard Quake II game module interface. Engine calls this to get function pointers for the game logic.

**Imports:** Only `KERNEL32.dll` (57 functions) — pure game logic, all engine interaction through the `game_import_t` function table.

### ref_gl.dll (OpenGL Renderer)

| Property | Value |
|----------|-------|
| Size | 225,280 bytes |
| Image Base | `0x30000000` |
| Entry Point | `0x00020FA2` |
| Code Size | 172,032 bytes (.text) |
| Sections | `.text` `.rdata` `.data` `.reloc` |
| Has Relocations | Yes (0x3CA8 bytes) |

**Exports:**
```
GetRefAPI  → 0x0000DFD0
```
Single export — standard Quake II renderer interface.

**Imports:**
| DLL | Functions | Purpose |
|-----|-----------|---------|
| KERNEL32.dll | 54 | OS fundamentals |
| USER32.dll | 14 | Window creation, display settings |
| GDI32.dll | 4 | Pixel format (WGL setup) |
| WINMM.dll | 1 | `timeGetTime` |
| AVIFIL32.dll | 10 | AVI recording (screenshot/video capture) |

**Key observations:**
- No direct `opengl32.dll` import! OpenGL functions are loaded at runtime via `GetProcAddress`/`wglGetProcAddress`
- AVI file support for recording — neat feature
- Window management is done in the renderer (creates its own window)
- `ChangeDisplaySettingsA` — fullscreen mode switching

### player.dll

| Property | Value |
|----------|-------|
| Size | 98,304 bytes |
| Image Base | `0x40000000` |
| Code Size | 69,632 bytes |
| Has Relocations | Yes |

**Exports:**
```
GetPlayerClientAPI  → 0x00001020
GetPlayerServerAPI  → 0x000010D0
```
Client/server player module — handles player-specific logic separate from general game entities.

### Sound DLLs

| DLL | Size | Purpose |
|-----|------|---------|
| EAXSnd.dll | 77,824 | Creative EAX environmental audio |
| A3Dsnd.dll | 102,400 | Aureal A3D positional audio |
| Defsnd.dll | 77,824 | Default/fallback sound driver |
| EaxMan.dll | 61,440 | EAX manager |

### Other DLLs

| DLL | Size | Purpose |
|-----|------|---------|
| FFC10.dll | 126,976 | Immersion FeelIT force feedback (mouse) |
| a3dapi.dll | 290,816 | Aureal A3D API |

## Architecture Summary

```
┌──────────────────────────────────────────────────┐
│                   SoF.exe                        │
│              (Main Engine Core)                   │
│  Image Base: 0x20000000                          │
│                                                  │
│  ┌─────────────────┐  ┌──────────────────────┐   │
│  │ Client          │  │ Server               │   │
│  │ - Input         │  │ - Entity management  │   │
│  │ - HUD           │  │ - Networking         │   │
│  │ - Prediction    │  │ - Game state         │   │
│  └────────┬────────┘  └──────────┬───────────┘   │
│           │                      │               │
│  ┌────────┴──────────────────────┴───────────┐   │
│  │ Common: Cmd, Cvar, FS, Com, Z_Malloc      │   │
│  └───────────────────────────────────────────┘   │
└──────┬──────────────────────┬────────────────────┘
       │ GetGameAPI()         │ GetRefAPI()
       ▼                      ▼
┌──────────────┐      ┌──────────────────┐
│ gamex86.dll  │      │  ref_gl.dll      │
│ Base:0x50000 │      │  Base:0x30000    │
│              │      │                  │
│ Game Logic   │      │ OpenGL Renderer  │
│ AI / NPCs    │      │ BSP / Models     │
│ Weapons      │      │ Lightmaps        │
│ GHOUL gore   │      │ Particles        │
│ DS Scripts   │      │ AVI capture      │
│              │      │                  │
│ ┌──────────┐ │      │ OpenGL loaded    │
│ │player.dll│ │      │ dynamically via  │
│ │Base:0x40 │ │      │ GetProcAddress   │
│ └──────────┘ │      └──────────────────┘
└──────────────┘
       │
       │ GetPlayerClientAPI()
       │ GetPlayerServerAPI()
```

## Recompilation Priority

1. **SoF.exe** — Must be done first. The engine is the foundation.
2. **ref_gl.dll** — Renderer needs to be rewritten for modern OpenGL/Vulkan. Smallest code module.
3. **gamex86.dll** — Game logic. Largest code module but well-isolated through the API boundary.
4. **player.dll** — Player logic, small and isolated.
5. **Sound DLLs** — Can be replaced with SDL2/OpenAL wrappers.
6. **FFC10.dll** — Force feedback. Low priority, nice-to-have.

## No SafeDisc Detected

The v1.00 retail disc executable does **not** appear to have SafeDisc protection. No `.stxt` sections, no suspicious section names, and no SafeDisc imports detected. This simplifies the recompilation significantly.

## Cross-Reference: Quake II GPL Source

Since SoF is based on id Tech 2, the [Quake II GPL source](https://github.com/id-Software/Quake-2) is our Rosetta Stone. The exported functions from `SoF.exe` map directly:

| SoF Export | Quake II Equivalent | Source File |
|------------|-------------------|-------------|
| `Cmd_AddCommand` | `Cmd_AddCommand` | `qcommon/cmd.c` |
| `Cmd_Argc` / `Cmd_Argv` | `Cmd_Argc` / `Cmd_Argv` | `qcommon/cmd.c` |
| `Com_Printf` / `Com_DPrintf` | `Com_Printf` / `Com_DPrintf` | `qcommon/common.c` |
| `Com_Error` | `Com_Error` | `qcommon/common.c` |
| `Cvar_Get` / `Cvar_Set` | `Cvar_Get` / `Cvar_Set` | `qcommon/cvar.c` |
| `FS_FOpenFile` / `FS_LoadFile` | `FS_FOpenFile` / `FS_LoadFile` | `qcommon/files.c` |
| `Z_Malloc` / `Z_Free` | `Z_Malloc` / `Z_Free` | `qcommon/common.c` |
| `GetGameAPI` (gamex86) | `GetGameAPI` | `game/g_main.c` |
| `GetRefAPI` (ref_gl) | `GetRefAPI` | `ref_gl/r_main.c` |

## Import Analysis Checklist

- [x] Catalog all Win32 API imports (KERNEL32, USER32, ADVAPI32)
- [ ] Catalog all OpenGL function calls (loaded dynamically — need disassembly)
- [ ] Catalog all DirectX usage (via FFC10.dll force feedback)
- [x] Catalog all WinSock imports (21 ordinal imports in WSOCK32.dll)
- [x] Map all inter-module calls (engine exports → game/renderer/player APIs)
- [x] Identify SafeDisc/copy protection (NOT present in v1.00)

## Next Steps

1. Disassemble `SoF.exe` .text section and cross-reference with Quake II source
2. Map out the `game_import_t` / `game_export_t` structures by analyzing `GetGameAPI`
3. Map out the `refimport_t` / `refexport_t` structures by analyzing `GetRefAPI`
4. Identify all dynamically loaded OpenGL functions in `ref_gl.dll`
5. Catalog the GHOUL-specific additions beyond standard Quake II
6. Analyze the sound DLL interface (how does the engine select/load sound drivers?)
