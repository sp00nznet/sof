# Contributing to SoF Recomp

Thanks for wanting to help bring Soldier of Fortune back from the dead.

## What We Need

### Reverse Engineering
- Binary analysis of `sof.exe`, `gamex86.dll`, and `ref_gl.dll`
- Function identification and annotation
- Data structure reconstruction
- Cross-referencing with Quake II GPL source

### Engine Work
- Win32 API modernization
- OpenGL 1.x -> modern OpenGL/Vulkan translation
- DirectInput -> SDL2/modern input
- DirectSound -> modern audio (OpenAL, SDL_mixer, etc.)

### Testing
- Behavioral comparison with original game
- Regression testing across game levels
- Multiplayer compatibility testing

## Rules

1. **No copyrighted code or assets.** Ever. Don't paste decompiled code directly. Document structures and behavior, then write clean-room implementations.
2. **Document your findings.** Add notes to `docs/` when you discover something about the original binary.
3. **Test against the original.** The gold standard is "does it behave identically to the original game?"
4. **Keep it fun.** This is a passion project. We're here because we love this ridiculous game.

## Getting Started

1. Fork the repo
2. Grab a copy of Soldier of Fortune (GOG, original disc, etc.)
3. Pick an area from the issues list
4. Start hacking

## Code Style

- C17 for engine code, C++20 where it genuinely helps
- Follow existing patterns in the codebase
- Comment non-obvious reverse-engineered behavior
- Use the types defined in `src/common/sof_types.h`
