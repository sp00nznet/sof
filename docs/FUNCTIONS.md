# SoF.exe Function Map

Identified through string reference analysis, export table, and Q2 cross-reference.
Total: 24 exports + 136+ string-referenced functions mapped so far.

## Source Path Discovery

The WON SDK code embedded in SoF.exe contains full source paths from the original build:
```
D:\projects\sof\code2\wonsdk\API\crypt\BFSymmetricKey.cpp
D:\projects\sof\code2\wonsdk\API\crypt\EGPrivateKey.cpp
D:\projects\sof\code2\wonsdk\API\crypt\EGPublicKey.cpp
D:\projects\sof\code2\wonsdk\API\crypt\CryptKeyBase.cpp
D:\projects\sof\code2\wonsdk\API\msg\tmessage.cpp
D:\projects\sof\code2\wonsdk\API\msg\Auth\TMsgAuth1*.cpp
D:\projects\sof\code2\wonsdk\API\msg\Routing\MMsgRouting*.cpp
D:\projects\sof\code2\wonsdk\API\msg\Dir\SMsgDirG2*.cpp
D:\projects\sof\code2\wonsdk\API\msg\Event\SMsgEventRecordEvent.cpp
D:\projects\sof\code2\wonsdk\API\msg\Comm\*.cpp
```

This tells us:
- SoF source was at `D:\projects\sof\code2\`
- The WON SDK was embedded directly in the source tree
- Project structure confirms Raven's development environment

## Known Exports (24 functions)

| RVA | Name | Q2 Source File |
|-----|------|---------------|
| `0x000045D0` | `CL_GetEntitySoundOrigin` | `client/cl_ents.c` |
| `0x0001A2F0` | `Cmd_Argc` | `qcommon/cmd.c` |
| `0x0001A300` | `Cmd_Argv` | `qcommon/cmd.c` |
| `0x0001A780` | `Cmd_AddCommand` | `qcommon/cmd.c` |
| `0x0001A830` | `Cmd_RemoveCommand` | `qcommon/cmd.c` |
| `0x0001DE70` | `Com_Printf` | `qcommon/common.c` |
| `0x0001E070` | `Com_DPrintf` | `qcommon/common.c` |
| `0x0001E130` | `Com_Error` | `qcommon/common.c` |
| `0x000203D0` | `Z_Free` | `qcommon/common.c` |
| `0x00020430` | `Z_Touch` | SoF-specific |
| `0x00020940` | `Z_Malloc` | `qcommon/common.c` |
| `0x00020B20` | `flrand` | SoF-specific |
| `0x00020B70` | `irand` | SoF-specific |
| `0x000240B0` | `Cvar_Get` | `qcommon/cvar.c` |
| `0x00024830` | `Cvar_Set` | `qcommon/cvar.c` |
| `0x00024A40` | `Cvar_SetValue` | `qcommon/cvar.c` |
| `0x000278A0` | `FS_FCloseFile` | `qcommon/files.c` |
| `0x000278D0` | `FS_FOpenFile` | `qcommon/files.c` |
| `0x00027E20` | `FS_LoadFile` | `qcommon/files.c` |
| `0x00027EE0` | `FS_FreeFile` | `qcommon/files.c` |
| `0x00058C00` | `VectorNormalize` | `game/q_shared.c` |
| `0x00058C90` | `VectorLength` | `game/q_shared.c` |
| `0x00058EF0` | `Com_sprintf` | `game/q_shared.c` |
| `0x0006AC20` | `Sys_Error` | `win32/sys_win.c` |

## Identified Internal Functions

### Engine Core

| RVA | Name | Evidence |
|-----|------|----------|
| `0x00020BB0` | `Qcommon_Init` | Refs `z_stats`, `error`, `disconnect`, `subliminal` — matches Q2 common init |
| `0x00023760` | `Com_Init` | Refs `cpu_mmx`, `cpu_amd3d`, `timescale`, `game` — CPU detection + core cvars |
| `0x00055640` | `Sys_ReadRegistry` | Refs `Software\Raven Software\SoF` — multiple registry access functions at 0x5563F–0x55D15 |

### Video/Renderer Loading

| RVA | Name | Evidence |
|-----|------|----------|
| `0x0006C671` | `VID_LoadRefresh` | Refs `vid_fullscreen`, `GetRefAPI`, `Reflib FreeLibrary failed`, `GetProcAddress failed on %s` — loads and initializes ref_gl.dll |

### Sound System

| RVA | Name | Evidence |
|-----|------|----------|
| `0x00058980` | `S_Init` / `Sys_LoadSoundDLL` | Refs `snd_dll`, `Sound Lib FreeLibrary failed`, `Sys_BeginFind` — sound DLL loading and init |

### Server

| RVA | Name | Evidence |
|-----|------|----------|
| `0x0005E767` | `SV_ServerCommand` | Refs `heartbeat`, `kick`, `status`, `serverinfo`, `adddownload` — server console commands |
| `0x00066A61` | `SV_SendClientMessages` | Refs `SV_Multicast: bad to:%i`, `SV_StartSound: volume = %f`, `SV_SendClientMessages: msglen > MaxMsgLen` |

### Client

| RVA | Name | Evidence |
|-----|------|----------|
| `0x0000A140` | `CL_Init` | Refs `%s/demos/%s`, `.dm2`, `deathmatch`, `rate`, `cl_maxfps` |
| `0x0000AA21` | `CL_ConnectionState` | Refs `maxclients`, `paused`, `qport`, `localhost`, `menu_won_error` |
| `0x0000D670` | `CL_LoadPlayerDLL` | Refs `items`, `weapons`, `dm_ctf`, `Client failed to load player DLL` |
| `0x00016E25` | `SCR_DrawHUD` | Refs `pics/menus/status/ch%i`, `viewsize`, `pics/console/net`, `menu_loading` |

### GHOUL Model System

| RVA | Name | Evidence |
|-----|------|----------|
| `0x00028AA1` | `FS_InitFilesystem` / `GHOUL_Init` | Refs `/BASE/`, `/GHOUL/`, `%s/pak%i.pak` — filesystem init with GHOUL paths |
| `0x000380F0` | `FX_SpawnEffect` | Refs `Sprite`, `Line`, `Oriented Sprite`, `Ghoul Object`, `Ghoul Persistent Debris` — particle/effect type dispatch |
| `0x0003A820` | `FX_Emitter` | Refs `From a Spot`, `From a Region`, `In a Line`, `In a Ring`, `In a Directed Line` — emitter shape types |
| `0x00038CF1` | `FX_LoadParticle` | Refs `fx_partadd`, `fx_partsub`, `textures/sprites/%s`, `.m32` |
| `0x00041900` | `FX_InitGoreEffects` | Refs `environ/bulletsplash`, `environ/splatgreen`, `environ/splatorange`, `environ/splatred` |
| `0x00096C82` | `GHOUL_SetupBolts` | Refs `FORCED_CAMERA`, `B_DOOR_LEFT`, `B_DOOR_RIGHT`, `B_BOLT1` |
| `0x000A8C50` | `GHOUL_LoadGHB` | Refs `Old file %s`, `Not a ghb file %s`, `Thunks are obsolete %s` |

### Menu System

| RVA | Name | Evidence |
|-----|------|----------|
| `0x000F4C65` | `Menu_LoadPackage` | Refs `menus/%s.rmf`, `Unable to include %s`, `Could not find token '%s'` |

### Networking

| RVA | Name | Evidence |
|-----|------|----------|
| `0x0014B33E` | `WON_WSAInit` | Refs all 46 Winsock function names — WON SDK's dynamic winsock loader |
| `0x0014C06C` | `NET_Init` | Refs `WSAStartup` |

### WON SDK (statically linked, ~68 functions identified)

Address range `0x00108000`–`0x00155000` is dominated by WON SDK code.
Source paths in the binary confirm these map to:
- `wonsdk/API/crypt/` — BFSymmetricKey, EGPrivateKey, EGPublicKey, CryptKeyBase
- `wonsdk/API/msg/Auth/` — Auth1 login, peer-to-peer auth
- `wonsdk/API/msg/Routing/` — Chat routing (~30 message types)
- `wonsdk/API/msg/Dir/` — Directory services
- `wonsdk/API/msg/Comm/` — Communication primitives
- `wonsdk/API/msg/Event/` — Event recording
- `wonsdk/API/msg/Fact/` — Fact services

### CD Key Validation

| RVA | Name | Evidence |
|-----|------|----------|
| `0x0012635E` | `WON_ValidateCDKey` | Refs `#######INVALID KEY######`, `CDKeys` |

### CRT / Support

| RVA | Name | Evidence |
|-----|------|----------|
| `0x0015496D` | `CRT_misc` | Refs `am/pm`, `.exe`, `.cmd`, `.bat` — MSVC CRT helpers |
| `0x0015E5A5` | `Sys_MessageBox` | Refs `user32.dll`, `MessageBoxA`, `GetActiveWindow` — dynamic MessageBox loading |
| `0x0015E99C` | `CRT_float` | Refs `1#SNAN`, `1#IND`, `1#INF`, `1#QNAN` — MSVC float formatting |

## Address Space Layout

Based on function clustering, we can map the SoF.exe code regions:

| Address Range | Size | Subsystem |
|---------------|------|-----------|
| `0x00001000`–`0x00010000` | 60 KB | Client (CL_*) |
| `0x00010000`–`0x0001A000` | 40 KB | Screen/HUD (SCR_*) |
| `0x0001A000`–`0x0001F000` | 20 KB | Command/Console (Cmd_*, Com_*) |
| `0x0001F000`–`0x00025000` | 24 KB | Memory/Cvar (Z_*, Cvar_*) |
| `0x00025000`–`0x0002D000` | 32 KB | Filesystem (FS_*) |
| `0x0002D000`–`0x00045000` | 96 KB | Effects/Particles (FX_*) |
| `0x00045000`–`0x00060000` | 108 KB | System/Sound loading (Sys_*, S_*) |
| `0x00060000`–`0x0006D000` | 52 KB | Server (SV_*), Video (VID_*) |
| `0x0006D000`–`0x000A0000` | 204 KB | Net/Game bridge |
| `0x000A0000`–`0x00108000` | 416 KB | GHOUL engine |
| `0x00108000`–`0x00155000` | 308 KB | WON SDK |
| `0x00155000`–`0x0017E000` | 164 KB | MSVC CRT |

## Statistics

- **8,872** readable strings in .rdata + .data
- **2,872** string references found in .text
- **136+** unique functions with string references
- **~68** WON SDK functions (can likely be stubbed out)
- **24** known exports
- WON SDK accounts for ~20% of the code section — removing it significantly reduces recomp scope
