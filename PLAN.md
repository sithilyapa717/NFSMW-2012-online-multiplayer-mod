# NFS Most Wanted 2012: Co-op Mod, Build Plan

Companion to `COOP_MOD_HANDOFF.md` (design and background). This file is the
build plan: phases, tasks, exit criteria, risks, and the future roadmap.
Update the **Status** column and the **Log** at the bottom as work proceeds.

---

## 0. Goal

Co-op for **single-player races** in NFS Most Wanted (2012, PC). The host runs a
normal single-player game. Up to 3 friends join through an in-game overlay,
each picks a car from their own save, and everyone races the **same event**
together against AI. Each PC saves its own progress.

Non-goals for v1: cops/pursuits, traffic sync, Speed Runs, Ambushes,
free-roam spawning, public matchmaking, anti-cheat.

## 1. Fixed facts about this setup

| Item | Value |
| --- | --- |
| Game install | `C:\sithil\Apps\Steam\steamapps\common\Need for Speed(TM) Most Wanted` |
| Source | Steam, appid 1262560, buildid 10351185, `VERSION.TXT` 1.4.0.0 |
| `NFS13.exe` | 13,684,056 bytes, EA-signed, **EA-wrapped (OOA)**: code encrypted on disk |
| SHA-256 | `3AF4DC5F206D3B41D2DCF46EF8EF41575E56834AE73DE826708A2A44C44FE971` |
| Architecture | 32-bit (x86) |
| Compiler | Zig 0.17.1: `zig cc -target x86-windows-gnu` (verified, produces x86 PE) |
| Other tools present | Python 3.12, git, cmake, Java, WSL, Docker, Cheat Engine |
| Tools to install | Ghidra, x32dbg (locations not yet confirmed) |
| Upstream project | `nfs13/` (sn0wd0wn), AGPL-3.0, has `client/trace_hook/proxy_d3d11.c` |

Constraints:
- Claude cannot run the game. The user runs each in-game test and returns logs/screenshots.
- Never modify the original `NFS13.exe`. Never change real unlock data in memory. **Back up saves before every test.**
- Every player needs their own purchased copy of the **same build**.
- Addresses are build-specific. Every hook verifies bytes first and skips itself on mismatch.

## 2. Architecture (target)

```
 Each PC:  NFS13.exe  +  coop d3d11.dll (our proxy, loaded next to the exe)
              |  hooks: Present (overlay), race setup, car update, race end
              |  ImGui overlay: Host / Join / lobby / Ready
              |  net layer (UDP + reliable channel)
              v
 Host PC  <--- star topology --->  Guest PCs (each connects only to host)
```

Authority split:

| Thing | Simulated by |
| --- | --- |
| A player's own car | That player's PC |
| AI racers, cops, traffic | Host; guests see puppets |
| Race state (countdown, ranking, results) | Host |

Code layout (to be created in `coop/`):

| Path | Purpose |
| --- | --- |
| `coop/net/` | Game-independent networking library (plain C, no game headers) |
| `coop/net/sim/` | Standalone simulator: fake cars, 2+ processes, used for tests |
| `coop/hook/` | The `d3d11.dll` proxy: hooks, memory access, ImGui overlay |
| `coop/tools/` | Log analysers, memory-dump helpers, scripts |
| `coop/docs/` | Findings: addresses, struct layouts, protocol spec |

Design rule: `net/` never includes game code, so it can be tested without the game.

---

## 3. Phases

Legend for Status: `TODO`, `DOING`, `DONE`, `BLOCKED`.

### Phase 0: Setup and read-only recon  (Status: DOING)

Goal: working toolchain, understand the existing hook, no game changes.

| # | Task | Status |
| --- | --- | --- |
| 0.1 | Verify install is legitimate (Steam manifest, EA signature, installer files) | DONE |
| 0.2 | Zig compiles 32-bit PE | DONE |
| 0.3 | Install Ghidra and x32dbg; record their paths here | TODO (user) |
| 0.4 | Read `proxy_d3d11.c` and `hook_engine.h`; documented in `coop/docs/UPSTREAM_HOOK_NOTES.md`. (`patch_nfs13.py` not read yet; only needed for online mode) | DONE |
| 0.5 | Build the unmodified trace hook with Zig (97,280 bytes, works) | DONE |
| 0.6 | Create `coop/` skeleton and `coop/build.ps1` (run with `powershell -ExecutionPolicy Bypass -File`; script execution is blocked by default) | DONE (trace target only so far) |
| 0.7 | Back up the game's save folder; record its location. **Not found: game has likely never been run here (Steam LastPlayed 0). Run it once, then locate saves** | TODO (user) |
| 0.8 | Ask sn0wd0wn Matrix chat whether car/AI object addresses are known | TODO (user) |

Exit: `coop/` builds with one command; we know exactly how upstream hooks work; saves backed up.

### Phase 1: Networking core, no game needed  (Status: DONE)

Goal: a tested network layer and a fake-car simulator.

| # | Task | Result |
| --- | --- | --- |
| 1.1 | Write protocol spec in `coop/docs/PROTOCOL.md`: packet header (protocol version, seq, ack, type), message types (see section 5) | DONE: `coop/docs/PROTOCOL.md` |
| 1.2 | UDP transport: non-blocking sockets, host listens, guests connect; handshake with version check and build-hash check | DONE: handshake (version, build hash, name, nonce), real UDP in `cn_transports.c` |
| 1.3 | Reliable channel on top of UDP: resend until ack, for Start / Finish / PlayerLeft / Ready | DONE: in-order go-back-N, 64-message queue; fine for control messages, not bulk |
| 1.4 | Unreliable state channel: numbered packets, newest wins, drop stale; 20-30 Hz send rate | DONE: unreliable CAR_STATE stamped with host time; duplicates and stale ignored |
| 1.5 | Host relay: host forwards each player's state to the other guests | DONE: host relays and overwrites the player id (spoof-tested) |
| 1.6 | Clock sync: several pings to host, take the lowest-RTT samples, compute offset; "race starts at host time X" | DONE: median of 3 best-RTT samples; error under 5 ms in tests |
| 1.7 | Interpolation buffer: render ~100 ms in the past; extrapolate up to ~0.5 s; smooth catch-up after a lag spike; ghost after ~5 s silence; remove after timeout | DONE, and better than planned: the delay adapts (50-300 ms) to measured transit time |
| 1.8 | Lobby state machine: players, chosen car (opaque blob for now), Ready flags, host start, "loaded" reports, synced countdown | DONE |
| 1.9 | Results: each peer reports finish time; host ranks and broadcasts final table | DONE: ties by id, dropped players are DNF |
| 1.10 | Rejoin: host sends full snapshot | DONE: rejoin by name replays PLAYER_LIST, START_EVENT, COUNTDOWN, RESULTS as normal events (no separate Snapshot message) |
| 1.11 | Simulator: N processes, each drives a fake car on a loop; visualises others (console or tiny window) | DONE: `coop/net/sim/sim.c`, console output, prints its own interpolation error vs ground truth |
| 1.12 | Fault injection: configurable packet loss, jitter, reorder, duplicate, latency; automated tests under each | DONE: `cn_fault` wrapper, used by tests and by the real-UDP simulator |
| 1.13 | Unit tests for sequence handling, clock sync, interpolation, ranking | DONE: 253 checks in `coop/net/test_net.c` |

Exit: 3 simulator processes (also across two PCs) complete lobby, countdown, race and ranking correctly with 5% loss and 100 ms jitter; host drop ends the race for all; guest drop does not.

**Phase 1 results (2026-11-17):**
- `coop/run_tests.ps1`: 253 checks pass, native 64-bit, 32-bit (`x86-windows-gnu`) and with `-fsanitize=undefined`. Covers garbage packets, handshake rejections, reliable delivery under 30% loss, clock sync, two full 4-player races back to back (5% loss, 30 ms + 100 ms jitter, clock skew), guest drop, host drop, leave, rejoin, car relay, spoofing.
- `coop/run_sim.ps1`: 4 separate processes over real UDP on this PC. Identical results on every peer. Remote-car error: 0.01 m avg on a clean link, 0.08 m avg (max under 1 m) at 8% loss, 40 ms latency, 60 ms jitter.
- **Two-PC test, done 2026-11-18:** host on a desktop, guest on a laptop over the home network (Wi-Fi-like rtt 46-63 ms, clocks 56 s apart). Lobby, synced countdown, race and results all correct; remote-car error 0.016 m avg (host view) and 0.010 m avg (guest view), max 0.41 m. Clean run only; not yet tried with extra loss or latency on top.
- Limits to keep in mind: no encryption or authentication; reliable channel is for rare messages (about 25/s worked, 80/s at 30% loss overflowed the queue and failed the link, which is by design); the library is not thread-safe (call from one thread).

### Phase 2: Game recon: find and control one AI car  (Status: TODO)  **make-or-break**

Goal: prove we can move an AI car from our DLL. If this fails, the project design changes.

| # | Task |
| --- | --- |
| 2.1 | Run game with the trace hook only; confirm it loads and logs on this build |
| 2.2 | Take a memory dump of the running game (code is OOA-encrypted on disk, so analyse the decrypted image in memory); load into Ghidra with correct base and sections |
| 2.3 | Reproduce the upstream hook addresses in the dump to confirm the dump is aligned with `0x00BBF3C0` etc. |
| 2.4 | Start a single-player race. With Cheat Engine, find the player car's position (floats), speed and rotation; find the pointer chain that stays valid across races |
| 2.5 | Find the AI vehicle list: how AI cars are created, stored, iterated, and how they are driven (AI controller vs. physics body) |
| 2.6 | In x32dbg, set write breakpoints on an AI car's position to find the function that updates it per frame |
| 2.7 | Document struct layouts in `coop/docs/GAME_MEMORY.md`: vehicle object, position, rotation, velocity, steering, AI controller pointer, with offsets and confidence level |
| 2.8 | In our DLL: hook the per-frame update (or use the `Present` hook as a fallback tick), walk the AI list, log each AI car's position |
| 2.9 | Overwrite one AI car's position/rotation/velocity every frame to a fixed spot, then to a moving circular path |
| 2.10 | Determine how to stop the game's own AI/physics fighting the override (disable AI controller for that car, or write after its update) |
| 2.11 | Confirm no crash through race start, finish, restart, quit-to-menu |

Exit: during a normal single-player race, one chosen AI car follows a path our code dictates, stably through a full race and a restart.

Risks: object layout may be complicated (physics body separate from render object); the game may rewrite the position after our write; AI may crash if disabled. Mitigation: try writing after the update hook; try zeroing the AI controller per car; ask upstream team.

### Phase 3: Puppet cars over the network  (Status: TODO)

Goal: your real car drives a puppet on another PC.

| # | Task |
| --- | --- |
| 3.1 | Read the local player's car state each frame (position, rotation, velocity, steering, wheel/body data needed for believable rendering) |
| 3.2 | Pack state into the Phase 1 state message; send at 20-30 Hz |
| 3.3 | Receiving side: feed state into the interpolation buffer; each frame write the interpolated state into the puppet (the taken-over AI car) |
| 3.4 | Two-PC test (or two game instances if the game allows) on LAN |
| 3.5 | Tune: interpolation delay, extrapolation limit, snap-vs-smooth thresholds |
| 3.6 | Collision: bounce off puppet locally; send "hit with force F at point P" to the owner; owner applies impulse |
| 3.7 | Handle puppet visuals: wheels turning, brake lights, damage state if easy; otherwise skip for now |

Exit: two PCs, two cars, each sees the other driving smoothly through a lap with no visible teleporting under normal LAN.

### Phase 4: Overlay lobby  (Status: TODO)

Goal: Host / Join / Ready UI inside the game.

| # | Task |
| --- | --- |
| 4.1 | Hook `Present`; add Dear ImGui with the D3D11 backend (confirm the game's D3D11 device/context are reachable) |
| 4.2 | Input: capture mouse/keyboard only while the overlay is open; hotkey to toggle; never swallow game input when closed |
| 4.3 | Screens: main (Host / Join), join dialog (address + port), lobby (player list, car, ready), in-race status line |
| 4.4 | Wire to net layer: host start, join, leave, Ready, errors shown as plain text |
| 4.5 | Clock-synced countdown overlay; block the game's own start until everyone is loaded |
| 4.6 | Block pause in co-op; handle restart request (applies to all) |
| 4.7 | Config file (`coop.ini`): host address, port, player name, hotkey |

Exit: host and a friend can open the overlay, connect, see each other in the lobby and both press Ready.

### Phase 5: Start the same event on every PC  (Status: TODO)

| # | Task |
| --- | --- |
| 5.1 | Find how the game starts an event (event id, car, setup functions); document in `GAME_MEMORY.md` |
| 5.2 | Host: capture the event id when the host launches an event; broadcast to guests |
| 5.3 | Guests: trigger the same event locally by calling the game's start function (or setting the same state the menu sets) |
| 5.4 | Guest must be in a valid state (not in a race, not in a menu that blocks); show errors otherwise |
| 5.5 | "Loaded" report from each PC when its race is ready to start; host waits for all, then sends start time |
| 5.6 | Guests whose game lacks the event unlocked are filtered out earlier (Phase 7); here assume all unlocked |

Exit: host launches an event and all PCs load it and start on the same synchronised countdown.

### Phase 6: Friend's car swapped in for an AI racer  (Status: TODO)

| # | Task |
| --- | --- |
| 6.1 | Find where the race setup picks the AI racers' cars; hook it |
| 6.2 | Read the guest's chosen car identity (and colour/parts if feasible) from their own save/memory; define the car-selection message |
| 6.3 | On every PC, replace one AI slot per friend with that friend's car (loading its model/physics); on the guest's own PC the slot for themselves is their own car |
| 6.4 | Check enough AI slots exist; offer only events with enough AI racers |
| 6.5 | Decide car-class rules (any car vs. similar class) and implement |
| 6.6 | Make the puppet for that car become the net-driven one (Phase 3 code) |

Exit: 2-3 players, each in their chosen car, in one event; others appear as the correct cars.

### Phase 7: Results, saves and progress filter  (Status: TODO)

| # | Task |
| --- | --- |
| 7.1 | Each PC times its own race (own start to own finish), sends finish time to host |
| 7.2 | Host ranks everyone, handles DNF and disconnects, broadcasts final table |
| 7.3 | Override results screen/rewards with the host's ranking so all see the same order; each game awards and saves its own progress |
| 7.4 | Progress filter: each player sends their unlocked cars/events (read-only from memory/save); lobby offers only the intersection |
| 7.5 | **Never write to unlock data.** Test that autosave during a co-op session does not change a guest's progress unexpectedly |
| 7.6 | Return to lobby after the race: change car, Ready again, next event |

Exit: full loop works for 2 players: lobby, race, shared results, back to lobby, next race; saves intact.

### Phase 8: 3-4 players, hardening, packaging  (Status: TODO)

| # | Task |
| --- | --- |
| 8.1 | Test 3 and 4 players; confirm host upload stays small (target 10-20 KB/s) |
| 8.2 | Disconnect cases: guest drops mid-race, rejoin with snapshot, host drops (race ends for all) |
| 8.3 | Version/build-hash handshake so mismatched builds refuse cleanly |
| 8.4 | Crash safety: any hook failure disables co-op and leaves the game running normally |
| 8.5 | Log file (`coop_trace.log`) with enough detail for remote debugging |
| 8.6 | Installer/readme: place DLL, config, firewall/VPN (WireGuard) instructions |
| 8.7 | Document known issues |

Exit: 4 players can finish several races in a row; failures are clean and logged.

---

## 4. Future roadmap (after v1)

Each item lists what is hard about it.

| Item | Notes |
| --- | --- |
| **Traffic** | Spawns randomly per PC. Options: disable in co-op; shared random seed; sync as puppets. Start with "disabled". |
| **Cops and pursuits** | Run separately per PC. First: no cops; then host-simulated cops as puppets. Pursuit scoring/busted logic needs thought. |
| **Speed Runs** | No AI opponents to take over. Needs spawning extra cars or ghost-style parallel timing. |
| **Ambush events** | Same: no AI racers. |
| **Free-roam co-op** | Spawn friends in the host's world as puppets; shared events from free roam; Jack Spot / billboard sharing rules. |
| **More than 4 players** | Code loops over players from the start; test and tune bandwidth. |
| **Shared Most Wanted list progress** | Optional mode: host's progression applies to all, or a merged campaign. |
| **Spectating** | Friends watch the host's race after finishing. |
| **Rematch / playlists** | Chain several events, cumulative points. |
| **Car visuals sync** | Colour, vinyls, parts, damage, wheel rotation, lights. |
| **Voice/text chat** | Overlay text chat first. |
| **Steam/EA App build support** | Re-verify addresses on other builds; add per-build address tables (currently unwrapped 13,672,448 and wrapped 13,684,056 are the known builds). |
| **Mod coexistence with sn0wd0wn online mode** | Same DLL slot (`d3d11.dll`): merge with or chain-load the upstream trace hook. |
| **Public matchmaking / relay** | Needs NAT traversal or a relay server. Currently LAN/VPN only. |

## 5. Message list (draft, refine in Phase 1)

| Type | Channel | Content |
| --- | --- | --- |
| Hello / HelloAck | reliable | protocol version, exe hash, player name |
| PlayerList | reliable | ids, names, car, ready, ping |
| CarChoice | reliable | car identity, colour, parts |
| Ready | reliable | bool |
| StartEvent | reliable | event id, host time |
| Loaded | reliable | bool |
| Countdown | reliable | race start time in host clock |
| CarState | unreliable | seq, timestamp, position, rotation, velocity, steering, flags |
| Hit | reliable | victim id, impulse, point |
| Finish | reliable | player id, finish time |
| Results | reliable | ranked list |
| Ping / Pong | unreliable | for RTT and clock offset |
| Leave / Kick | reliable | reason |
| ~~Snapshot~~ | n/a | replaced by replaying PLAYER_LIST / START_EVENT / COUNTDOWN / RESULTS (see PROTOCOL.md) |

## 6. Test strategy

- **Phase 1:** automated tests and simulator runs with fault injection. Claude can run these itself.
- **Phase 2 onward:** user-run in-game tests. Each test has a short script: what to do, what to send back (`coop_trace.log`, screenshot). Claude writes the script before each test.
- Every test starts with a save backup. A failed or crashed test is recorded in the Log.
- Keep a `coop/docs/TESTS.md` table: date, phase, build hash, result, notes.

## 7. Top risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Cannot control an AI car cleanly (Phase 2) | Project design changes | Do Phase 2 early after minimal Phase 0; ask upstream; try alternative (override AI input instead of position) |
| OOA-encrypted exe is hard to analyse | Slow recon | Work from memory dumps; the exe is decrypted at runtime |
| Build-specific addresses | Breaks on other builds | Byte checks, skip-on-mismatch, per-build tables |
| Save corruption | User loses progress | Backups, read-only access to unlock data, no writes |
| Desync (traffic, cops, physics) | Weird races | Out of scope v1; host-authoritative results |
| Claude cannot run the game | Slow test loop | Verbose logging; scripted test steps; simulator for everything not game-dependent |
| EA/anti-tamper or AV flags DLL | Blocks loading | Offline/LAN only; document |

## 8. Decisions still open

- Can friends bring any car, or only a similar class? (Phase 6)
- Does the game allow two instances on one PC for testing? (Phase 3)
- Overlay hotkey default. (Phase 4)
- Port numbers for co-op (must not clash with 3659/17500/42127/44125/8080).

## 9. Findings so far

- Upstream hook gives us a ready **Present hook** (per-frame tick and overlay point) and a safe inline-hook pattern. It contains **no hooks for gameplay, vehicles, AI or races**; all of that is ours to find.
- **Upstream DllMain has side effects we must not copy:** it writes `Locale=ru_RU` to the registry (HKLM and HKCU), forces a 60 FPS cap plus VSync, and sets DXVK env vars. Details in `coop/docs/UPSTREAM_HOOK_NOTES.md`. If we run the trace DLL for recon, record the original locale value first.
- Hooks are installed after a blind `Sleep(500)`; for the EA-wrapped exe we want a robust "code is decrypted" check (poll expected prologue bytes).
- Image base is fixed at 0x400000 (absolute addresses work).

## 10. Log

| Date | Entry |
| --- | --- |
| 2026-11-17 | Plan written. Install verified (Steam, EA-signed, EA-wrapped 13,684,056 bytes). Zig 0.17.1 builds x86. Ghidra and x32dbg not yet found. |
| 2026-11-17 | Phase 0: read upstream hook, built trace hook with Zig, created `coop/` skeleton and build script. Save folder not found (game never run on this PC). |
| 2026-11-17 | Phase 1 done: net library (`coop/net/`), 253 automated checks, 4-process real-UDP simulator, `coop/docs/PROTOCOL.md`. Adaptive interpolation delay added. Two-PC test still open. |
| 2026-11-18 | Phase 1 two-PC test passed (desktop host, laptop guest, real network). Phase 1 closed. Next: Phase 2 (needs Ghidra, x32dbg, game run once, saves backed up). |
