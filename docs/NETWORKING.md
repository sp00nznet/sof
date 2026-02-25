# Networking Architecture

## Three Networking Layers

SoF has three distinct networking subsystems stacked on top of each other:

### 1. Quake II Net Layer (UDP game traffic)

Standard Q2-derived networking using WSOCK32.dll (Winsock 1.1) imported by ordinal.
Uses non-blocking UDP sockets with polling (no `select()`).

**WSOCK32.dll Ordinal Imports (21 functions):**

| Ordinal | Function | Purpose |
|---------|----------|---------|
| 2 | `bind` | Bind socket to local address/port |
| 3 | `closesocket` | Close socket |
| 4 | `connect` | Establish connection |
| 6 | `getsockname` | Get local address |
| 9 | `htonl` | Host to network byte order (32-bit) |
| 10 | `htons` | Host to network byte order (16-bit) |
| 12 | `ioctlsocket` | Socket I/O mode (FIONBIO for non-blocking) |
| 14 | `listen` | Listen for connections |
| 15 | `ntohl` | Network to host byte order (32-bit) |
| 16 | `ntohs` | Network to host byte order (16-bit) |
| 17 | `recv` | Receive data (connected) |
| 19 | `recvfrom` | Receive data (connectionless) |
| 20 | `send` | Send data (connected) |
| 21 | `sendto` | Send data to address |
| 22 | `setsockopt` | Set socket options (SO_BROADCAST) |
| 23 | `socket` | Create socket |
| 52 | `gethostbyname` | DNS resolution |
| 57 | `gethostname` | Get local hostname |
| 111 | `WSAGetLastError` | Get last error code |
| 115 | `WSAStartup` | Initialize Winsock |
| 116 | `WSACleanup` | Cleanup Winsock |

Notable: No `select()` — engine uses `ioctlsocket(FIONBIO)` non-blocking polling.

**Netchan Protocol (Q2 heritage):**
```
Connection:  "getchallenge %d" → "challenge %i %d %i" → "connect %i %i %i %d %4.2f \"%s\""
Sequenced packets with reliable/unreliable channels
Packet format: sequence | ack | reliable flag | data
```

**Network CVars:**
```
net_latency, net_receiverate, net_sendrate, showpackets, net_shownet
net_socksEnabled, net_socksServer, net_socksPort
net_socksUsername, net_socksPassword
ip_hostport, ip_clientport, hostport, port, clientport, qport
noudp, noipx, rate, cl_timeout, cl_maxfps, maxclients
```

**SOCKS5 Proxy Support:** Full implementation present (connection, authentication, UDP relay).

**IPX Support:** Present alongside UDP (era-typical for LAN gaming).

### 2. WON (World Opponent Network) SDK

Statically linked WON SDK for online authentication, server browser, and chat.
Dynamically loads its own copy of `wsock32.dll` via `LoadLibrary` at runtime.

**WON Servers:**
```
sof.east.won.net        — East coast
sof.west.won.net        — West coast
sof.central.won.net     — Central
sof.ravensoft.com       — Raven Software
```

**Service Identifiers:**
```
SoldierOfFortuneClient   — Client identity
SoldierOfFortuneServer   — Server identity
RoutingServSoFChat       — Chat routing server
```

**WON Console Commands:**
```
won_login <user> <pass>
won_create_account <user> <pass>
won_set_key <key>
won_list_rooms / won_join_room / won_exit_room / won_create_room
won_list_users / won_talk / won_get_motd
won_checklogin / won_markconnection / won_markdisconnection
```

**WON CVars:** `won_username`, `won_password`, `won_error`, `won_nomenus`, `no_won`

**WON Crypto (from RTTI class names):**
- Blowfish symmetric encryption
- ElGamal public key encryption/decryption
- Auth1 certificate system
- CD key validation (`ClientCDKey@WONCDKey`)

**Original source paths (from debug info):**
```
D:\projects\sof\code2\wonsdk\API\crypt\BFSymmetricKey.cpp
D:\projects\sof\code2\wonsdk\API\crypt\EGPrivateKey.cpp
D:\projects\sof\code2\wonsdk\API\msg\Auth\TMsgAuth1LoginHW.cpp
D:\projects\sof\code2\wonsdk\API\msg\Routing\MMsgRouting*.cpp
D:\projects\sof\code2\wonsdk\API\msg\Dir\SMsgDirG2*.cpp
```

### 3. HTTP Client

Embedded in the WON SDK for MOTD/update checks. Basic HTTP/1.1 client.

## Server Administration

```
kick <userid>
rcon_password — Remote console authentication
status — Show connected players
"num score ping name lastmsg address qport" — Status format
```

**IP Filtering/Banning:**
```
Filter_remove <ip-mask>
listip.cfg — Stored ban list
```

## Download System

```
allow_download, allow_download_maps, allow_download_models
allow_download_players, allow_download_sounds, allow_download_stringpackage
```

Standard Q2 server-to-client file transfer for custom content.

## Registry

```
Software\Raven Software\SoF         — Main game key
Software\Raven Software\SoF\CDKeys  — CD key storage
```

## Recompilation Implications

1. **Winsock 1.1 → Modern:** Replace with Winsock 2 or cross-platform sockets (SDL_net, ENet)
2. **WON is dead:** WON shut down in 2004. Options:
   - Strip WON entirely, replace with direct IP connect
   - Implement a lightweight master server protocol
   - Integration with open-source server browsers
3. **IPX:** Can be safely removed (no modern use)
4. **SOCKS5:** Keep — still useful for players behind proxies
5. **CD key auth:** Remove — preservation project, no DRM
