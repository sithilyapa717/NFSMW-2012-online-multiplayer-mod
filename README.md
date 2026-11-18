# NFS Most Wanted (2012): co-op mod

Work in progress: co-op for the single-player races of Need for Speed: Most Wanted (2012, PC).
Friends join the host's race, each in their own car, against the AI.

Requires your own legally purchased copy of the game. No game files are included here.
Not affiliated with or endorsed by Electronic Arts or Criterion Games.

## State

| Part | State |
| --- | --- |
| Networking library (lobby, countdown, race, results, rejoin, smoothing of remote cars) | Done, tested: `coop/net/` |
| Network simulator with fake cars (no game needed) | Done: `coop/net/sim/` |
| Finding and controlling AI cars inside the game | Not started (Phase 2) |
| In-game overlay lobby, car swap, shared results | Not started |

Full plan, phases and roadmap: [PLAN.md](PLAN.md). Protocol: [coop/docs/PROTOCOL.md](coop/docs/PROTOCOL.md).

## Try the simulator (no game needed)

Needs [Zig](https://ziglang.org) 0.17 to build. Windows.

```
powershell -ExecutionPolicy Bypass -File coop\build.ps1 -Target sim
powershell -ExecutionPolicy Bypass -File coop\run_tests.ps1
sim.exe host --port 47000 --name Host --players 2        (on the host PC)
sim.exe join --addr <host-ip>:47000 --name Friend          (on the other PC)
```

Open UDP 47000 in the host's firewall. Same LAN or a VPN such as WireGuard.

## Credits

The idea of loading code through a proxy `d3d11.dll` comes from the
[nfs13](https://github.com/178lavash178-create/nfs13) project by sn0wd0wn (AGPL-3.0), which revives the game's
online mode. That project is separate; this repo does not contain its code.
