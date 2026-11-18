# coop/net: game-independent networking library

Plain C11, no dependencies, no game headers. Protocol: [../docs/PROTOCOL.md](../docs/PROTOCOL.md).

| File | What |
| --- | --- |
| `cn_common.h`, `cn_buf.h` | Types, bounds-checked reader/writer |
| `cn_session.[ch]` | Host and guest: handshake, reliable channel, clock sync, lobby, countdown, race, results, rejoin |
| `cn_interp.[ch]` | Smoothing/prediction for remote cars |
| `cn_transports.[ch]` | In-memory network (tests), fault injection (loss, dup, latency, jitter), real UDP (Windows) |
| `test_net.c` | 253 automated checks |
| `sim/sim.c` | One process = one player with a fake car, over real UDP |

## Use (what the game DLL will do later)

```c
cn_config cfg = { .tr = cn_udp_transport(&udp), .build_hash = HASH, .name = "Alice", .seed = rand() };
cn_session s;                      // ~100 KB: keep it static or on the heap
cn_host_init(&s, &cfg, now);       // or cn_guest_init(&s, &cfg, host_addr, now)
for (;;) {                         // once per frame (or faster)
    cn_update(&s, now);            // now = local monotonic ms
    cn_event e;
    while (cn_poll_event(&s, &e)) { /* CN_EV_START_EVENT, CN_EV_COUNTDOWN, CN_EV_RACE_GO, CN_EV_RESULTS, ... */ }
    cn_send_car_state(&s, &my_car, now);                    // 20-30 Hz while racing
    cn_presence p = cn_remote_state(&s, id, now, &car);     // each frame, per remote player
}
```

## Build and test (from the repo root)

```powershell
powershell -ExecutionPolicy Bypass -File coop\run_tests.ps1                 # unit + network tests
powershell -ExecutionPolicy Bypass -File coop\build.ps1 -Target sim         # sim.exe (64-bit) and sim32.exe (32-bit)
powershell -ExecutionPolicy Bypass -File coop\run_sim.ps1 -Players 4 -Loss 5 -Lat 30 -Jitter 40
```

Both 32-bit (`-target x86-windows-gnu`) and 64-bit builds are tested; the library also passes `-fsanitize=undefined`.
