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
