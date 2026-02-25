# Sound System Architecture

## Plugin Interface

SoF uses a pluggable sound DLL system. The engine loads one of three mutually exclusive backends
at runtime via `LoadLibrary` / `GetProcAddress`. All three share image base `0x60000000` and were
compiled 2000-03-11 within 30 seconds of each other.

| DLL | Size | Backend |
|-----|------|---------|
| `Defsnd.dll` | 77,824 | Default — DirectSound via runtime `LoadLibrary("dsound.dll")` |
| `EAXSnd.dll` | 77,824 | Creative EAX environmental audio (COM-based) |
| `A3Dsnd.dll` | 102,400 | Aureal A3D positional audio (COM-based) |

### Support DLLs

| DLL | Size | Purpose |
|-----|------|---------|
| `EaxMan.dll` | 61,440 | EAX Manager — COM server for mapping level geometry to EAX presets |
| `a3dapi.dll` | 290,816 | Aureal A3D 2.0 API redistributable |

## Common Export Interface (20 functions)

All three sound DLLs export exactly the same 20 functions:

| # | Export | Purpose |
|---|--------|---------|
| 1 | `S_Init` | Initialize the sound subsystem |
| 2 | `S_Shutdown` | Tear down the sound subsystem |
| 3 | `S_Activate` | Activate/deactivate (window focus) |
| 4 | `S_Update` | Per-frame update (mix, spatialize, submit) |
| 5 | `S_BeginRegistration` | Start sound registration (level load) |
| 6 | `S_EndRegistration` | End registration, free unreferenced sounds |
| 7 | `S_RegisterSound` | Register/precache a sound by name |
| 8 | `S_RegisterAmbientSet` | Register ambient sound set |
| 9 | `S_RegisterMusicSet` | Register music set |
| 10 | `S_FindName` | Look up a sound by name |
| 11 | `S_FreeSound` | Free a specific sound resource |
| 12 | `S_Touch` | Mark sound as in-use (prevent eviction) |
| 13 | `S_StartSound` | Play spatialized sound at world position/entity |
| 14 | `S_StartLocalSound` | Play non-spatialized UI/local sound |
| 15 | `S_StopAllSounds` | Stop all playing sounds |
| 16 | `S_SetSoundStruct` | Pass engine's sound data structure pointer |
| 17 | `S_SetSoundProcType` | Set sound processing type/mode |
| 18 | `S_SetGeneralAmbientSet` | Set current ambient sound set |
| 19 | `S_SetMusicIntensity` | Set music intensity (dynamic music system) |
| 20 | `S_SetMusicDesignerInfo` | Set music designer metadata |

### Backend-Specific Extensions

**EAXSnd.dll** adds 2 extra exports:
- `SNDEAX_SetEnvironment` — Set EAX reverb environment type
- `SNDEAX_SetEnvironmentLevel` — Set EAX environment effect level

**A3Dsnd.dll** adds 1 extra export:
- `S_A3D_ExportRenderGeom` — Export render geometry for A3D occlusion/reflection

## Engine Imports (from SoF.exe)

Sound DLLs import engine functions directly from `SoF.exe`'s export table by ordinal.

**Common to all three (11 functions):**

| Ordinal | Function | Category |
|---------|----------|----------|
| 0 | `CL_GetEntitySoundOrigin` | Sound spatialization |
| 1 | `Cmd_AddCommand` | Console commands |
| 3 | `Cmd_Argv` | Console commands |
| 4 | `Cmd_RemoveCommand` | Console commands |
| 6 | `Com_Error` | Error handling |
| 7 | `Com_Printf` | Console output |
| 8 | `Com_sprintf` | String formatting |
| 9 | `Cvar_Get` | Console variables |
| 14 | `FS_FreeFile` | File system |
| 15 | `FS_LoadFile` | File system |
| 17 | `VectorLength` | Math |

**Defsnd.dll and A3Dsnd.dll additionally import:**
`Cmd_Argc`, `Com_DPrintf`, `FS_FCloseFile`, `FS_FOpenFile`, `VectorNormalize`,
`Z_Malloc`, `Z_Free`, `Z_Touch`, `irand`

EAXSnd.dll was built with more minimal engine linkage.

## External Dependencies

| DLL | Defsnd | EAXSnd | A3Dsnd |
|-----|--------|--------|--------|
| KERNEL32.dll | Yes | Yes | Yes |
| ole32.dll | — | Yes (COM) | Yes (COM) |
| USER32.dll | — | Yes | — |
| ADVAPI32.dll | — | — | Yes (registry) |

## SoF-Specific Additions vs Quake II

Q2's sound system was built into the engine. SoF modularized it into DLLs and added:
- **Ambient sound set system** (`S_RegisterAmbientSet`, `S_SetGeneralAmbientSet`)
- **Dynamic music system** (`S_RegisterMusicSet`, `S_SetMusicIntensity`, `S_SetMusicDesignerInfo`)
- **Sound processing modes** (`S_SetSoundProcType`)
- **Engine data struct passing** (`S_SetSoundStruct`)

## Recompilation Strategy

For the modern replacement, we can implement the 20-function interface using:
- **OpenAL Soft** for 3D spatialized audio (replaces DirectSound + EAX + A3D)
- **SDL2_mixer** as a simpler alternative
- OpenAL Soft includes EAX reverb emulation via EFX extensions

The sound DLL interface is clean and well-defined — this should be one of the easier subsystems to replace.
