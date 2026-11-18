# Game memory notes (NFS13.exe, Steam build 10351185, 13,684,056 bytes)

Findings from analysing a dump of the running game. Confidence is marked. Nothing here is verified by
changing the game yet.

## The dump

- Tool: `coop/tools/dump_image.c` (read-only). Taken 2026-11-18 with the game in free roam.
- Main module: base `0x00400000`, size `0xDDF000` (14,544,896 bytes), all 3,551 pages readable.
- **Verified:** the six function prologues that the upstream trace hook checks (`EventLookup 0x6E5DC0`, `ParsePlaylist 0x6EB0D0`, `ValidatePlaylist 0x6EA460`, `AdapterConnect 0xC0E4F0`, `ConnApiAddClient 0xB84430`, `NetworkAddPeer 0xC0E1B0`) all match in the dump. So: same build as upstream, code is decrypted in memory, and addresses are not relocated (fixed base 0x400000).
- The dump itself is not committed (it contains the game's code). It lives in `coop/build/dumps/` which is git-ignored.

Sections (RVA): `.text` 0x1000 (0xB80E81), `.rdata` 0xB82000, `.data` 0xCC1000, `.tls` 0xDBE000, `.rsrc` 0xDBF000, plus one more executable section at 0xDDD000 (0x12F6 bytes).
Virtual addresses = base + RVA, e.g. `.text` is 0x401000..0xF81E81.

## What kind of engine this is (high confidence)

- Only 207 RTTI class names exist and almost all belong to libraries (Origin SDK, std). **The game's own classes have no RTTI**, so class names cannot be read directly.
- The game is **script/data driven**: C++ code registers functions and properties by string name, and game logic lives in data (`GAMELOGIC\GAMEMODES.BNDL`, `GameModeManager`). Those names are in `.rdata` and give us entry points: find the string, find who references it.

## Leads: strings in `.rdata` (VA of the string)

| Area | Strings (VA) |
| --- | --- |
| AI racers | `AI racer count` 0xFC45F8, `RacerCount` 0xFC4608, `AI racer` 0xFC46C0, `NumOpponents` 0xFC45D8, `The number of valid opponents (does not include the local player)` 0xFC4850 |
| Spawning | `SpawnVehicle` 0xFBAC24, **`SpawnNetworkCar` 0xFBCF60**, `RequestRespawn` 0xFB9558, `RequestRespawnAtEventStart` 0xFB9568 |
| Player car | `GetPlayerVehicleIndex` 0xFBAD2C, `PlayerCarForward` 0xFA2574, `IsPlayerCarMoving` 0xFBAE30, `GetPlayerVehicleGear` 0xFC268C, `GetPlayerVehicleDistanceDriven` 0xFC28DC |
| Game modes | `GameModeManager` 0xFC5AC8, `StartGameModeTimer` 0xFB9720, `ResetGameMode` 0xFB9814, `SetGameModeWon` 0xFB9860, `GameModeEnd` 0xFBD054, `GetRaceResultsFeedback` 0xFEC50C |
| Traffic | `TrafficEntity` 0xFB1F28, `Game::Traffic::EngineSoundInstance` 0xFB1BD0, `TrafficLaneData` 0xF9C90F |

Full string and RTTI lists were extracted to `coop/build/dumps/*_strings.txt` and `*_rtti.txt` (git-ignored).

## Big lead: `SpawnNetworkCar`  (unverified)

The online mode already spawns cars that are driven by network data (that is how other players appear in online free roam, which the sn0wd0wn server revives). If that code path can be reused in single player, then **we may not need to hijack AI cars at all**: spawn a "network car" per friend and feed it our own packets. That would replace plan tasks 2.5-2.10 with: find `SpawnNetworkCar`, see what it needs (a network player object? a mesh connection?), and whether it can be called offline.

Two approaches for Phase 2 now, to be decided after reading that code:

1. **Network-car route:** reuse the game's own remote-car spawning and update path.
2. **AI-car route (original plan):** take over an AI opponent and overwrite its transform every frame.

## Next steps

1. Ghidra: import `nfs13_image_00400000.exe` (a PE rebuilt from the dump so file offset = RVA), run auto-analysis.
2. Follow the xrefs of `SpawnNetworkCar`, `AI racer count` and `GetPlayerVehicleIndex`.
3. Cross-check with Cheat Engine / x32dbg in a live single-player race.

## Ghidra findings (2026-11-18)

- **`SpawnNetworkCar` is only a message type**, not a spawn function. Its handler `FUN_0069a7e0` allocates a 0x40-byte message object (vtable `0x00FBD55C`) from a pool. The real spawning is done by whoever receives that message. So the "network-car route" needs the receiver found, and is not a ready-made call.
- `PlayerManager` (size 0xCC60, registered in `FUN_006d4350`): racer count `+0xA694`, racer pointer array `+0xA664`, local player index `+0xA6B4`, active flags `+0xA5D0`, local `Player` objects stride 0x7F0 from `+0x10`, `AIPlayer` objects stride 0x5E0 from `+0x18D0`.
- `PlayerBase` (size 0x250, `FUN_006c7f20`): `PlayerIndex +0x98`, `RaceCarIndex +0xA0`, `IsActive +0x23D`, `Wrecked +0x23E`, `Crashed +0x23F`. Getters for `GetPosition` (`FUN_006d81d0`), `GetTransform` (`FUN_006d8230`), `GetPlayerSpeed` (`FUN_006d7d30`).
- Script function table in `FUN_00693f90` (handler addresses): `AddAllAIPlayers` 0x687710, `AreAllAIPlayersLoaded` 0x67A710, `RemoveAllRaceCars` 0x675F90, `SetOnlineRaceCarFrozen` 0x68AEA0.
- `AIPlayer` (size 0x5E0, `FUN_006C6F00`) only exposes SearchWidth/SearchDistance.
- A player's car is an **int handle** in `PlayerBase+0xA0` (`RaceCarIndex`, -1 = none). Validity `FUN_00573390(h)`, position `FUN_0083be30(&vec4, h)`, speed `FUN_0083ea90(h)`, transform (4x4 float matrix, 64 bytes) via `FUN_006b9ed0`. `LocalPlayer` = `PlayerBase+0x9C == 0`. `AddAllAIPlayers` handler 0x687710 calls `FUN_00686f30`.
- Vehicle table: `FUN_00573390(mgr, h)` reads byte at `*(mgr+4) + h*0x1C40 + 0x3069` (valid flag), so each vehicle record is **0x1C40 bytes**. Speed `FUN_0083ea90` reads physics data `FUN_00571160(h)` (velocity at +0x120..0x128). `AddAllAIPlayers` = `FUN_00686f30`: loops over the event's racer entries and calls `FUN_006CE030(entry, &pos, &orient, &zero, ...)` to create each AI racer. Script function `TeleportVehicle` exists (string 0xFBAC34) and is a candidate for moving puppet cars.
- **AI racer creation** `FUN_006CE030` (thiscall on PlayerManager): refuses if active count (`+0xA6A0`) > 15; picks a free slot 12..23 (`+0xA600 + slot*4` == 0), initialises `AIPlayer` slot `FUN_006CC870(slot, entry, ...)`, stores object at `+0x18D0 + slot*0x5E0`, appends to racer list (`+0xA664`, count `+0xA694`) unless the entry flags (`entry+0xB3/0xB5`) mark it as cop/other (then separate list `+0xA630`, count `+0xA660`), registers in `+0xA5D0 + slot*4`.
- **Vehicle object**: `FUN_00571160(mgr, h)` = `DAT_010EDB68[ (short)(*(short*)(*(mgr+4)+0x293C) + h) ]`. Object fields: **position vec4 at +0x110**, **velocity at +0x120** (x,y,z). Its vtable slot 0 returns a pointer to the 4x4 transform (used by `FUN_0083C060`). Speed returned in `FUN_0083EA90` uses `_DAT_0118A060` as a unit scale.
- `PlayerBase+0x9C` = player type: **0 = local, 1 = network/remote (human), 2 = AI** (inferred from `FUN_006A7CA0`, the "player registered" routine: for type 1 it builds a `SpawnNetworkCar`-class message (vtable 0xFBD55C) from the *local* car's position (`FUN_0083BE30`), orientation (`FUN_0083C030`) etc. and sends it to that player's slot bit; type 2 is skipped). Other message vtables: 0xFBDAE8, 0xFBD864.
- Callers of `FUN_006CE030` (AI/racer creation): `FUN_006E8AE0`, `FUN_00686F30` (AddAllAIPlayers), `FUN_00748A40`, `FUN_00852E40`, `FUN_006D6110`. One of them is probably the path that creates remote human players.
- `FUN_006D6110` is the **PlayerManager message handler** (switch on message type; `this` = PlayerManager). Case 0xD = spawn a racer: calls `FUN_006CE030(vehicleSpec = msg[2], pos = msg+0x10, orient = msg+0x20, vel = msg+0x30, 0, 0, DAT_00F8F340)`; case 9 = remove player (slot `msg[2]`, calls vtable[4] of the player). `FUN_006CE030` is also called by `FUN_006E8AE0` (spawn queue), `FUN_00748A40` (cop/other spawn), `FUN_00852E40` (event-script spawn). So **we can create an extra racer by calling `FUN_006CE030` ourselves** (thiscall, ecx = PlayerManager) once we know the vehicle-spec pointer.
- PlayerManager pointer chain (UNVERIFIED, from decompiled code): `G = *(0x01177868)`, `PM = *(G + 0x2F3DC)`. Local player = `PM + idx*0x7F0 + 0x10` (idx at `PM+0xA6B4`).
- `coop/tools/peek_players.c` (read-only) checks this chain live: prints every slot with type, car handle, position, velocity.

## Live verification (2026-11-18, free roam, `peek_players.exe`)

- **Verified live:** `G = *(0x01177868)`, `PM = *(G+0x2F3DC)`, slots at `PM+0xA5D0+i*4`. Local player = slot 0, `type=0`, car handle 0. A cop appears in slot 12 with `type=2` (AI), handles 1..3 reused as cops spawn and despawn; `cops` count at `PM+0xA660` goes 0 <-> 1. So the player-manager structure and the type field (0 local, 2 AI) are right.
- **Not right yet:** the vehicle entries at `DAT_010EDB68[...]` are only 0x10 bytes apart (`0991FEA0`, `FEB0`, `FEC0`), so they are small handle records, not the big physics object. Reading `+0x110` gives NaN/0. Use `peek_players deep` to find where position really lives. Alternative that needs no offsets: call the game's own `FUN_0083BE30(&vec4, handle)` (position) and `FUN_0083C060` (transform).
- **Correction:** the vehicle manager (ECX of `FUN_00573390/00573420/00571160`, used by `FUN_0083BE30` position, `FUN_0083C060` transform, `FUN_0083BDE0` valid) is the pointer stored at **`0x010EDAE4`**, not `0x010EDB04` (that one is used by the speed function `FUN_0083EA90` and others). Lookup: `T = *(mgr+4)`; `entry = DAT_010EDB68[(short)(*(short*)(T+0x293C) + handle)]`; position = 16 bytes at `entry+0x110`; validity byte at `T + handle*0x1C40 + 0x3068`.
- **Verified live (2026-11-18): car position = 3 floats (x, y, z) at `entry + 0x110`** (y is height, about 63 here), where `entry = DAT_010EDB68[(short)(*(short*)(T+0x293C) + handle)]`, `T = *(*(0x010EDAE4) + 4)`. Position changed as the local car moved. Velocity at `entry + 0x120` is correct for the AI cop (e.g. -3.8, -0.1, -17.3); it read 0 for the local car at the moment of the snapshot (car was probably stopped).
- **Writing the position field does not work (tested live):** setting `entry+0x110` snaps back to the same place; it is a copy refreshed from the physics body each frame.
- **`TeleportVehicle` script function** is registered at `0x0068D6xx` (`push handler; push name`): handler = **`FUN_0067C1A0`**. It reads script args (`FUN_00EC00C0(vm, n)` = float arg n, `FUN_00EBFCA0` = arg count, `FUN_00EC0140(vm,5)` = bool), resolves the vehicle with `FUN_00EDA610()`, then calls the real teleport **`FUN_00647E00(vehicle, &pos, &orient, speed, flag, 3, 0, 0)`**. Args: 1 = vehicle, 2..4 = position, 5 = flag, optional 6..8 = direction vector (normalized by the handler), 9 = speed.
- **`FUN_00647E00` is a teleport REQUEST queue, not an instant move** (thiscall; `this` = a manager object, probably the respawn/teleport manager). Signature as called from `TeleportVehicle`: `(handle, &pos[4 floats], &orient[4 floats], speed(float), bool flag, int 3, 0, 0)`. It zero-fills and fills a 0x90-byte request slot at `this + handle*0x90 + 0x20` (pos at +0x30 and +0x50, orient at +0x40 and +0x60, speed at +0x70, flags), sets a pending byte at `this + 4 + handle` and `this[handle + 0x248] = param_7`; the manager's update later performs the move. For local players it validates the target first (`FUN_0054B540`). Nine callers: `FUN_0064A0E0, 006E4770, 0067AEF0, 0067C1A0, 0067C650, 00680CA0, 006D0180, 00761D70 (x2)`.
- **Teleport manager = `*(0x01177870) + 0x25690`** (confirmed in the `TeleportVehicle` handler: `MOV ECX,[0x01177870]; ADD ECX,0x25690; CALL 0x00647E00`). `*(0x01177870)` is **0 in free roam** (live log), so it is probably only set during an event/race; the teleport can only be tested there. 61 code references read this global (`FUN_0067ace0 ... FUN_00688060`), it is a main "game/event state" object.
