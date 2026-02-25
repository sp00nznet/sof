# Technical Notes: Soldier of Fortune

## Engine Overview

Soldier of Fortune (2000) is built on a **heavily modified id Tech 2 (Quake II) engine** by Raven Software. While the core architecture follows the Quake II pattern (client/server separation, BSP rendering, PAK file assets), Raven made substantial modifications.

## Key Engine Modifications

### GHOUL Damage Model System

The crown jewel. GHOUL replaced the entire Quake II model system with:

- **26 gore zones** per humanoid model
- Zone-based hit detection with unique damage responses
- Dismemberment, wound channels, and localized gore effects
- Multiple skin pages per model
- Animation data compression
- Model attachment system (weapons, gear)
- Advanced networking for syncing gore state

GHOUL was so technically ambitious that Raven noted it "turned out to be quite an undertaking" — which is developer-speak for "this nearly killed us."

### ArghRad! Lighting

Enhanced version of id's QRAD lighting tool:
- Phong-type shading model in lightmap calculations
- Global sunlight casting capability
- Improved light bounce and radiosity

### Designer Script (DS)

Custom scripting language developed collaboratively between programmers and designers. Used for game logic, NPC behavior, and level scripting.

### ROFF (Rotation Object File Format)

Movement animation system:
- ~500 movement files used in the shipping game
- Drives entity movement, helicopter flight paths, environmental effects
- Custom format specific to SoF

### Audio

- Custom dynamic music system
- Ambient sound system with property-based configuration
- No hard-coded file references — flexible sound property system

## File Formats

| Format | Extension | Purpose |
|--------|-----------|---------|
| PAK | `.pak` | Asset archive (Quake II heritage) |
| BSP | `.bsp` | Level geometry and lightmaps |
| GHOUL Model | `.glm` | Character/object models |
| ROFF | `.rof` | Movement animation data |
| DS Script | `.ds` | Designer script files |
| Texture | `.wal` / `.tga` | World and model textures |

## Executable Structure

The original game ships as:
- `sof.exe` — Main executable (Win32 PE, x86 32-bit)
- `gamex86.dll` — Game logic module (loaded by engine)
- `ref_gl.dll` — OpenGL renderer (legacy OpenGL 1.x / ICD)

### Known PE Details (to be confirmed via analysis)
- Compiler: MSVC (likely Visual C++ 6.0, era-appropriate)
- Linked against: `opengl32.dll`, `wsock32.dll`, `winmm.dll`, standard CRT
- Uses Win32 API extensively (window management, input, file I/O)

## Modern Compatibility Issues

### Why It Doesn't Run on Windows 10/11

1. **16-bit installer components** — Original installer may use 16-bit code
2. **Legacy OpenGL ICD assumptions** — Expects OpenGL behaviors removed in modern drivers
3. **DirectDraw/DirectInput legacy** — Uses deprecated DirectX interfaces
4. **Memory layout assumptions** — 32-bit address space expectations, ASLR conflicts
5. **Timing issues** — `QueryPerformanceCounter` / `timeGetTime` precision changes
6. **CD-ROM checks** — Original had disc-based copy protection
7. **SafeDisc DRM** — Some versions used SafeDisc, which Microsoft removed support for
8. **Thread scheduling** — Modern Windows thread scheduler behaves differently

## Recompilation Strategy

### Phase 1: Analysis
- Full disassembly of `sof.exe` and `gamex86.dll`
- Identify all imported functions and system dependencies
- Map out function boundaries and call graph
- Identify data sections, vtables, and global state

### Phase 2: Lifting
- Convert x86 instructions to intermediate representation
- Resolve indirect calls and jump tables
- Reconstruct control flow graphs
- Identify and annotate known library functions

### Phase 3: Recompilation
- Generate C/C++ source from IR where possible
- Compile to x86-64 native code
- Replace Win32 API calls with modern equivalents or SDL2 abstraction
- Replace legacy OpenGL with modern rendering backend

### Phase 4: Patching & Testing
- Implement compatibility shims for behavioral differences
- Widescreen and high-resolution support
- Input system modernization
- Extensive testing against original game behavior

## References

- [Postmortem: Raven Software's Soldier of Fortune](https://www.gamedeveloper.com/programming/postmortem-raven-software-s-i-soldier-of-fortune-i-) — Game Developer (2000)
- [Quake II Engine Documentation](https://github.com/id-Software/Quake-2) — id Software GPL source release
- [PCGamingWiki: Soldier of Fortune](https://www.pcgamingwiki.com/wiki/Soldier_of_Fortune) — Community compatibility notes
