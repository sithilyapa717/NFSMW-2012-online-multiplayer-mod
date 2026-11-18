# Notes on the upstream trace hook (read 2026-11-17)

Source: `nfs13/client/trace_hook/proxy_d3d11.c`, `hook_engine.h` (Dantiox / sn0wd0wn, AGPL-3.0).

## How it works (reusable ideas)
- **Proxy DLL**: exports `D3D11CreateDevice` and `D3D11CreateDeviceAndSwapChain` (see `d3d11.def`), loads the real `system32\d3d11.dll`, forwards calls. Game loads it because it sits next to the exe.
- **Present hook**: after `D3D11CreateDeviceAndSwapChain` succeeds, patches `vtbl[8]` (IDXGISwapChain::Present) of the returned swap chain. **This is our per-frame tick and the place to draw the ImGui overlay.** The swap chain, and via it the device, is available right here.
- **Inline hook** (`HookFunction`): copies `len` stolen bytes to a trampoline, writes `E9 rel32` JMP at the target, NOPs the rest. Needs `len >= 5` and must end on an instruction boundary. Does not relocate relative instructions in the stolen bytes (so prologues must be position-independent).
- **InstallChecked**: compares first bytes at the absolute address to an expected prologue; skips the hook on mismatch. Good safety pattern; we keep it.
- **IAT hooks** for Winsock (sendto/recvfrom/bind/connect/socket/closesocket/gethostbyname/WSARecvFrom).
- Hooks are installed from a worker thread after `Sleep(500)`. This is a timing guess, not a real "code is decrypted" signal. With the EA-wrapped exe we should wait for a robust signal (e.g. poll until the expected prologue bytes appear, with timeout).
- The exe is non-relocated: absolute VAs like `0x00BBF3C0` are used directly (image base 0x400000).
- Calling conventions seen: mostly `__thiscall` (MSVC C++ methods); detours use `__thiscall` / `__fastcall` with Zig/gcc.

## Known upstream hooked addresses (online code only, nothing about gameplay/AI)
See `nfs13/client/trace_hook/README.md`. None of them touch vehicles, AI, races or the single-player event system. We must find those ourselves (Phase 2).

## Things our DLL must NOT copy
`DllMain` in the trace hook has side effects unrelated to tracing:
1. `EnsureRussianLocale()` writes `Locale=ru_RU` to **HKLM and HKCU** `...\EA Games\Need for Speed(TM) Most Wanted`. It changes the game's language registry value.
2. `Hook_Present` is a **60 FPS limiter that also forces VSync** (SyncInterval 0 becomes 1).
3. Sets `DXVK_CONFIG` / `DXVK_FRAME_RATE` env vars (frame cap 60).
4. Writes `nfs13_trace.log` in the current directory, with `fopen("w")` (truncates each launch).

For the coop DLL: no locale change, no frame limiter (or optional and off by default), log to a fixed path next to the DLL, with append or a timestamped name.

If the trace DLL is ever run on this PC for recon, expect the locale registry value to change; note the original value first (`reg query`), and restore afterwards.

## Build
`zig cc -target x86-windows-gnu -O2 -shared -s proxy_d3d11.c d3d11.def -o d3d11.dll -lws2_32`
Verified 2026-11-17 with Zig 0.17.1: builds, 97,280 bytes. Script: `coop/build.ps1`.
