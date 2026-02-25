# Binary Analysis Notes

## Target Executables

Analysis should be performed against the **v1.06a** patch as the primary target — this is the most widely distributed and stable version.

### sof.exe (Main Engine)
- Format: Win32 PE, 32-bit x86
- Expected compiler: MSVC 6.0 (SP5/SP6)
- Primary responsibilities:
  - Window creation and management
  - Input handling (DirectInput)
  - Sound mixing
  - Network stack
  - File system (PAK loading)
  - Client/server framework
  - Loading and calling into gamex86.dll and ref_gl.dll

### gamex86.dll (Game Logic)
- Loaded dynamically by engine via `GetGameAPI()` export
- Contains:
  - Entity logic
  - AI/NPC behavior
  - Weapon systems
  - GHOUL gore zone handling
  - Designer Script interpreter
  - Level scripting hooks

### ref_gl.dll (OpenGL Renderer)
- Loaded dynamically by engine via `GetRefAPI()` export
- Contains:
  - OpenGL 1.x rendering pipeline
  - BSP rendering
  - Model rendering (GHOUL models)
  - Lightmap management
  - Particle effects
  - HUD rendering

## Analysis Tools

Recommended toolchain:
- **Ghidra** — Primary disassembler/decompiler (free, handles MSVC binaries well)
- **IDA Pro** — Alternative disassembler (if available)
- **x64dbg** — Dynamic analysis on compatible systems
- **PE-bear** — PE header analysis
- **Dependency Walker** — Import/export analysis
- **Custom scripts** — Python tooling for batch analysis

## Import Analysis Checklist

- [ ] Catalog all Win32 API imports
- [ ] Catalog all OpenGL function imports
- [ ] Catalog all DirectX (DirectInput, DirectSound) imports
- [ ] Catalog all WinSock imports
- [ ] Map all inter-module calls (engine <-> game, engine <-> renderer)
- [ ] Identify SafeDisc/copy protection stubs (if present)

## Known Quake II Symbols

Since SoF is based on id Tech 2, many internal functions will match or closely resemble the open-source Quake II codebase. Cross-referencing with the [Quake II GPL source](https://github.com/id-Software/Quake-2) will accelerate analysis significantly.

Key structures to look for:
- `cvar_t` — Console variable system
- `entity_state_t` — Entity networking
- `refdef_t` — Renderer frame definition
- `game_import_t` / `game_export_t` — Engine <-> game interface
- `refimport_t` / `refexport_t` — Engine <-> renderer interface
