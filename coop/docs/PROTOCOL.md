# Co-op network protocol (version 1)

Implemented in `coop/net/` (`cn_session.c`, `cn_interp.c`). Pure C, no game headers.
All integers are little-endian. Transport is UDP (any datagram transport works).

## Topology and identity

- Star: guests talk only to the host. Host is **player 0**; guests get ids 1..3 (`CN_MAX_PLAYERS` = 4).
- A guest's identity is its **source address**, never a field in a packet. The host overwrites the player id on relayed car states, so a guest cannot impersonate another.
- Time: every peer has its own local millisecond clock. "Host time" is the host's local clock. Guests estimate `offset` so that `host_time = local + offset`.

## Datagram

```
u16 magic        0x4E43
u8  flags        bit0 = ack field is valid
u16 ack          next reliable seq this side expects from the peer (cumulative)
items...         until the end of the datagram (max 1200 bytes)
```

Item:

```
u8  kind         0 = unreliable, 1 = reliable
u16 seq          only if kind == 1
u16 len          payload length (<= 320)
u8  type
... payload
```

A bad magic, truncated item or unknown kind drops the rest of the datagram.

## Reliable channel

Per connection, one in-order stream (go-back-N):

- Sender numbers messages 0, 1, 2... (u16, wraps), keeps them queued (64 max) until acked, resends the first 16 unacked every 100 ms.
- Receiver accepts only `seq == expected`; anything else is ignored (and re-acked). Result: **in order, exactly once**.
- Acks are cumulative and ride on every datagram; a datagram with reliable data always triggers an ack.
- If the queue overflows (64 unacked) the connection is treated as dead (host drops the guest; guest reports `HOST_LOST`).
- Designed for rare control messages (a few per second at most), not bulk data.

## Messages

Unreliable (`kind` 0):

| Type | Name | Direction | Payload |
| --- | --- | --- | --- |
| 0x01 | HELLO | guest to host | u8 version, u32 nonce, u32 build_hash, char[16] name |
| 0x02 | WELCOME | host to guest | u8 player_id, u8 version, u32 host_time |
| 0x03 | REJECT | host to guest | u8 reason (1 version, 2 build, 3 full, 4 name taken, 5 race in progress) |
| 0x04 | PING | guest to host | u32 t0 (guest local), u16 last_rtt |
| 0x05 | PONG | host to guest | u32 t0, u32 host_time |
| 0x06 | HEARTBEAT | host to guest | none, every 500 ms |
| 0x10 | CAR_STATE | both ways | u8 player_id, u32 t (host time), car state (50 bytes) |

Reliable (`kind` 1):

| Type | Name | Direction | Payload |
| --- | --- | --- | --- |
| 0x20 | PLAYER_LIST | host to guest | u8 phase, u32 event_id, u32 race_start, u8 n=4, then per player: u8 state, char[16] name, u8 ready, u8 loaded, u8 finished, u8[32] car, u32 finish_ms, u16 ping |
| 0x21 | CAR_CHOICE | guest to host | u8[32] opaque car description (lobby only) |
| 0x22 | READY | guest to host | u8 (lobby only) |
| 0x23 | START_EVENT | host to guest | u32 event_id |
| 0x24 | LOADED | guest to host | none (loading phase only) |
| 0x25 | COUNTDOWN | host to guest | u32 race_start in host time |
| 0x26 | FINISH | guest to host | u32 finish_ms (racing phase only) |
| 0x27 | RESULTS | host to guest | u8 n, then per entry: u8 id, u8 finished, u8 rank, u32 finish_ms |
| 0x28 | BACK_TO_LOBBY | host to guest | none |
| 0x29 | LEAVE | both ways | none (also sent as 2 unreliable copies) |

Car state (50 bytes): `f32 pos[3], f32 rot[4] (quaternion xyzw), f32 vel[3], f32 steer, u16 flags`.
Whole CAR_STATE message = 55 bytes; at 30 Hz about 1.7 KB/s per car (plus relay).

## Handshake and rejoin

1. Guest sends HELLO every 250 ms for up to 5 s (random `nonce` per process start).
2. Host checks, in this order: version, build hash, same address (duplicate HELLO or restarted client), a lost player with the same name (rejoin), name already connected, race in progress, free slot, otherwise REJECT.
3. Host answers WELCOME (repeated for duplicate HELLOs with the same nonce). The reliable streams start at seq 0 on both sides.
4. Host immediately queues the state a joiner needs: PLAYER_LIST, then START_EVENT if loading or later, COUNTDOWN if counting down or racing, RESULTS if results are showing. A rejoining guest receives these as normal events (START_EVENT, COUNTDOWN, then RACE_GO once its clock passes the start time).
5. A different nonce from a known address means the client restarted: the connection state is reset and it counts as a rejoin.

## Clock sync

Guest sends PING every 100 ms for the first second, then every 1 s. On PONG: `rtt = now - t0`, `offset = host_time + rtt/2 - now`. The estimate is the median offset of the (up to) 3 lowest-RTT samples among the last 8. Error is bounded by half the one-way asymmetry (tests: within 5 ms with 40 ms latency and 20 ms jitter). `clock_synced` becomes true after 3 samples.

## Phases (host-driven)

```
LOBBY --start_event (all connected players ready)--> LOADING
LOADING --all connected players sent LOADED (or 60 s timeout drops the laggards)--> COUNTDOWN
COUNTDOWN  race_start = host_now + 3000 ms, broadcast; at host_time >= race_start --> RACING
RACING --everyone connected finished, or 30 s after the first finisher, or 10 min--> RESULTS
RESULTS --host_return_to_lobby--> LOBBY (ready flags cleared, lost players removed)
```

Each peer fires `RACE_GO` when its own estimate of host time passes `race_start`, so countdowns end together to within the clock error (a few ms).

Ranking: finishers by finish time (ties by lower id), then DNFs by id. Players who dropped mid-race are DNF.

## Failure handling

| Situation | Behaviour |
| --- | --- |
| Guest silent for 5 s | Host: `PLAYER_LOST`. In lobby the slot is freed; later it becomes DISCONNECTED and the player can rejoin by name. Race continues, player is DNF. |
| Guest sends LEAVE | Host: `PLAYER_LEFT`, same as above, immediately. |
| Host silent for 5 s or sends LEAVE | Guest: `HOST_LOST`, link FAILED. **The race is over for the guest.** |
| Packet loss / duplication / reordering | Reliable stream recovers; car states are numbered by timestamp, duplicates and stale ones are ignored. |
| Garbage datagrams | Counted in `stat_bad_packets` and ignored (fuzz-tested). |

## Remote cars: interpolation

`cn_interp` per remote player, in host time:

- **Adaptive render delay**: worst transit time (sample age on arrival) over the last 32 samples + 43 ms, clamped 50-300 ms, slewed at most 10% of real time. A relayed car (guest to host to guest) has two hops, so its delay is roughly double a direct car's.
- Between samples: linear position and velocity, normalised lerp of the rotation (shortest path).
- Late data: **extrapolate** with velocity for up to 500 ms (`EXTRAP`), then **hold** frozen as a ghost (`GHOST`) until 5 s (`REMOVED`).
- When data resumes after prediction: the difference is applied as an offset that decays with a 150 ms time constant (no teleport). Corrections over 30 m snap.
- After 5 s of silence a new sample starts a fresh stream.

Measured over real UDP, 4 processes: avg error 0.01 m on a clean link; 0.08 m with 8% loss, 40 ms latency and 60 ms jitter per direction.

## Not covered yet

- Encryption or authentication (LAN/VPN use only; the host trusts anyone who knows its address and build hash).
- Collision/hit messages (`Hit`), result screens and anything game-specific: later phases.
- Bandwidth reduction (quantised positions). Currently 55 bytes per car state.
- Dead-player cleanup of long-lived DISCONNECTED slots (they clear when returning to lobby).
