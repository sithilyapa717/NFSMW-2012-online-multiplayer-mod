/*
 * coop_d3d11.c - our own proxy d3d11.dll for the NFSMW co-op mod (Phase 2 experiments).
 *
 * Idea (from the upstream nfs13 project, rewritten here): the game loads d3d11.dll from its own
 * folder, so a DLL with that name gets loaded into the game. We forward the two D3D11 functions the
 * game uses to the real system DLL, and patch IDXGISwapChain::Present to get a once-per-frame callback
 * on the render thread. We do NOT copy the upstream side effects (no registry changes, no frame
 * limiter, no VSync change, no environment variables).
 *
 * Experiment keys (while the game window is running):
 *   F8   write the player list (type, handle, position) to coop_hook.log
 *   F9   ask the game to teleport the first AI car (type 2) 12 m beside the local car
 *   F10  ask the game to teleport the LOCAL car 25 m along +X (tests the same teleport request path)
 * Everything is logged to coop_hook.log next to this DLL (appended, never truncated).
 *
 * Build: see coop/build.ps1 -Target hook
 */
#define WIN32_LEAN_AND_MEAN
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* ---------------------------------------------------------------- logging */
static char g_logpath[MAX_PATH];
static void logf_(const char *fmt, ...)
{
    FILE *f = fopen(g_logpath, "a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* ------------------------------------------------- safe reads of game memory */
static int rd(uint32_t a, void *out, uint32_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, out, n, &got) && got == n;
}
static int rd32(uint32_t a, uint32_t *v) { return rd(a, v, 4); }

/* ------------------------------------------------------- game structures
 * (all offsets are documented in coop/docs/GAME_MEMORY.md) */
#define ADDR_G            0x01177868u /* *G + 0x2F3DC = PlayerManager */
#define ADDR_VEH_MGR      0x010EDAE4u /* *x + 4 = vehicle data block */
#define ADDR_VEH_TABLE    0x010EDB68u /* entry pointers */
/* *0x01177870 is 0 in free roam AND in races (live log), so we use G = *0x01177868 instead:
 * the other callers do `ECX = <game object> + 0x25690` and FUN_00647E00 itself reads *this + 0x2F3DC as the
 * PlayerManager, so the object at G+0x25690 should start with G. We check that and log it. */
#define ADDR_TELEPORT_MGR ADDR_G
#define ADDR_TELEPORT_FN  0x00647E00u
#define ADDR_DEFAULT_DIR  0x00FBCB70u /* 4 floats the TeleportVehicle script uses when no direction is given */

static uint32_t player_manager(void)
{
    uint32_t g = 0, pm = 0;
    if (!rd32(ADDR_G, &g) || !g || !rd32(g + 0x2F3DC, &pm)) return 0;
    return pm;
}

/* vehicle entry pointer for a car handle, 0 if unknown */
static uint32_t vehicle_entry(uint32_t handle)
{
    uint32_t mgr = 0, data = 0, off = 0, entry = 0;
    if (!rd32(ADDR_VEH_MGR, &mgr) || !mgr || !rd32(mgr + 4, &data) || !data || !rd32(data + 0x293C, &off)) return 0;
    short idx = (short)((short)(off & 0xffff) + (short)handle);
    rd32(ADDR_VEH_TABLE + (uint32_t)(int)idx * 4, &entry);
    return entry;
}

static int car_position(uint32_t handle, float pos[4])
{
    uint32_t e = vehicle_entry(handle);
    return e && rd(e + 0x110, pos, 16);
}

typedef void(__thiscall *teleport_fn)(void *mgr, int handle, float *pos, float *dir, float speed, char flag, int p7,
                                      unsigned p8, int p9);

static int g_quiet; /* 1 = do not log each request (per-frame pin mode) */

static int request_teleport(uint32_t handle, const float pos[4], const char *why)
{
    uint32_t mgr = 0;
    /* the game loads these with MOVAPS (FUN_00647E00 @ 0x647EAA), so they MUST be 16-byte aligned */
    float dir[4] __attribute__((aligned(16)));
    int r1 = rd32(ADDR_TELEPORT_MGR, &mgr);
    int r2 = rd(ADDR_DEFAULT_DIR, dir, 16);
    if (r1 && mgr) mgr += 0x25690u; /* the game does ADD ECX,0x25690 before calling FUN_00647E00 */
    {
        uint32_t first = 0;
        rd32(mgr, &first);
        if (!g_quiet) logf_("teleport (%s): mgr object=%08x its first dword=%08x (expect G=%08x)\n", why, mgr,
              first, mgr - 0x25690u);
        if (first != mgr - 0x25690u) { logf_("teleport (%s): refused, wrong manager\n", why); return 0; }
    }
    if (!r1 || mgr == 0x25690u || !mgr || !r2) {
        logf_("teleport (%s): manager not available (read mgr=%d value=%08x, read dir=%d)\n", why, r1, mgr, r2);
        return 0;
    }
    float p[4] __attribute__((aligned(16))) = {pos[0], pos[1], pos[2], pos[3]};
    if (!g_quiet) logf_("teleport (%s): handle=%u to (%.1f, %.1f, %.1f) dir=(%.2f,%.2f,%.2f,%.2f) mgr=%08x\n", why, handle,
                        p[0], p[1], p[2], dir[0], dir[1], dir[2], dir[3], mgr);
    ((teleport_fn)(uintptr_t)ADDR_TELEPORT_FN)((void *)(uintptr_t)mgr, (int)handle, p, dir, 0.0f, 0, 3, 0, 0);
    if (!g_quiet) logf_("teleport (%s): request queued\n", why);
    return 1;
}

static void dump_players(void)
{
    uint32_t pm = player_manager();
    if (!pm) { logf_("players: no PlayerManager\n"); return; }
    uint32_t racers = 0, cops = 0, local = 0;
    rd32(pm + 0xA694, &racers);
    rd32(pm + 0xA660, &cops);
    rd32(pm + 0xA6B4, &local);
    {
        uint32_t tm = 0, vm = 0;
        int a = rd32(ADDR_TELEPORT_MGR, &tm), b = rd32(ADDR_VEH_MGR, &vm);
        logf_("globals: *0x01177870=%08x(ok=%d) *0x010EDAE4=%08x(ok=%d)\n", tm, a, vm, b);
    }
    logf_("players: PM=%08x racers=%u cops=%u localIdx=%d\n", pm, racers, cops, (int)local);
    for (int i = 0; i < 24; i++) {
        uint32_t p = 0, type = 0, h = 0;
        if (!rd32(pm + 0xA5D0 + i * 4, &p) || !p) continue;
        rd32(p + 0x9C, &type);
        rd32(p + 0xA0, &h);
        float pos[4] = {0, 0, 0, 0};
        int ok = (int)h != -1 && car_position(h, pos);
        logf_("  slot %2d type=%u handle=%d pos=(%.1f, %.1f, %.1f)%s\n", i, type, (int)h, pos[0], pos[1], pos[2],
              ok ? "" : " (no position)");
    }
}

/* first AI car (type 2) and the local car's handle */
static int find_cars(uint32_t *local_h, uint32_t *ai_h)
{
    uint32_t pm = player_manager();
    *local_h = *ai_h = (uint32_t)-1;
    if (!pm) return 0;
    for (int i = 0; i < 24; i++) {
        uint32_t p = 0, type = 0, h = 0;
        if (!rd32(pm + 0xA5D0 + i * 4, &p) || !p) continue;
        rd32(p + 0x9C, &type);
        rd32(p + 0xA0, &h);
        if ((int)h == -1) continue;
        if (type == 0 && *local_h == (uint32_t)-1) *local_h = h;
        if (type == 2 && *ai_h == (uint32_t)-1) *ai_h = h;
    }
    return 1;
}

/* after a teleport request, look 700 ms later whether the car really is near the requested spot */
/* up to 16 pending checks, so quick F9 presses are each judged separately */
#define NVERIFY 16
static struct { int active; uint32_t handle; float want[4]; float before_to_local; float before[4]; DWORD at; } g_verify[NVERIFY];

static float dist3(const float a[4], const float b[4])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void verify_tick(void)
{
    for (int i = 0; i < NVERIFY; i++) {
        if (!g_verify[i].active || GetTickCount() - g_verify[i].at < 1000) continue;
        g_verify[i].active = 0;
        float now[4];
        if (!car_position(g_verify[i].handle, now)) {
            logf_("verify: handle %u has no position any more\n", g_verify[i].handle);
            continue;
        }
        float to_spot = dist3(now, g_verify[i].want);
        float moved = dist3(now, g_verify[i].before);
        logf_("verify: handle %u was %.0f m from you before; after 1 s it is %.1f m from the requested spot "
              "(moved %.0f m) => %s\n",
              g_verify[i].handle, g_verify[i].before_to_local, to_spot, moved, to_spot < 40.0f ? "TELEPORTED" : "NOT teleported");
    }
}

/* F9 cycles through the AI cars (type 2) so every one gets tested, not always the first */
static void key_f9(void)
{
    static int turn;
    uint32_t pm = player_manager();
    uint32_t ai[24], n = 0, lh = (uint32_t)-1;
    float lp[4];
    if (!pm) { logf_("F9: no PlayerManager\n"); return; }
    for (int i = 0; i < 24; i++) {
        uint32_t p = 0, type = 0, h = 0;
        if (!rd32(pm + 0xA5D0 + i * 4, &p) || !p) continue;
        rd32(p + 0x9C, &type);
        rd32(p + 0xA0, &h);
        if ((int)h == -1) continue;
        if (type == 0 && lh == (uint32_t)-1) lh = h;
        if (type == 2) ai[n++] = h;
    }
    if (lh == (uint32_t)-1 || n == 0) { logf_("F9: need a local car and an AI car (start a race or wait for a cop)\n"); return; }
    if (!car_position(lh, lp)) { logf_("F9: cannot read local car position\n"); return; }
    uint32_t target = ai[turn++ % n];
    logf_("F9: %u AI cars, picking handle %u\n", n, target);
    lp[0] += 12.0f;
    float before[4] = {0, 0, 0, 0};
    car_position(target, before);
    float dist_before = dist3(before, lp);
    if (request_teleport(target, lp, "F9 AI car beside local")) {
        for (int i = 0; i < NVERIFY; i++) {
            if (g_verify[i].active) continue;
            g_verify[i].active = 1;
            g_verify[i].handle = target;
            g_verify[i].at = GetTickCount();
            g_verify[i].before_to_local = dist_before;
            memcpy(g_verify[i].want, lp, sizeof lp);
            memcpy(g_verify[i].before, before, sizeof before);
            break;
        }
    }
}

static void key_f10(void)
{
    uint32_t lh, ah;
    float lp[4];
    find_cars(&lh, &ah);
    if (lh == (uint32_t)-1 || !car_position(lh, lp)) { logf_("F10: no local car\n"); return; }
    lp[0] += 25.0f;
    request_teleport(lh, lp, "F10 local car +25 x");
}

/* F11: "pin" test. Every frame, ask the game to put the first AI car 12 m beside the local car.
 * This is what a puppet car needs: a teleport request EVERY frame. Logs the hit rate once a second. */
static void pin_tick(int on)
{
    static uint32_t frames, ok, tot;
    static int was_on;
    if (!on) { was_on = 0; return; }
    if (!was_on) { was_on = 1; frames = ok = tot = 0; }
    uint32_t pm = player_manager(), lh = (uint32_t)-1, ah = (uint32_t)-1;
    if (!pm) return;
    for (int i = 0; i < 24; i++) {
        uint32_t p = 0, type = 0, h = 0;
        if (!rd32(pm + 0xA5D0 + i * 4, &p) || !p) continue;
        rd32(p + 0x9C, &type);
        rd32(p + 0xA0, &h);
        if ((int)h == -1) continue;
        if (type == 0 && lh == (uint32_t)-1) lh = h;
        if (type == 2 && ah == (uint32_t)-1) ah = h;
    }
    float lp[4] __attribute__((aligned(16))), now[4];
    if (lh == (uint32_t)-1 || ah == (uint32_t)-1 || !car_position(lh, lp)) return;
    lp[0] += 12.0f;
    g_quiet = 1;
    request_teleport(ah, lp, "pin");
    g_quiet = 0;
    frames++;
    if (car_position(ah, now)) {
        tot++;
        if (dist3(now, lp) < 30.0f) ok++;
    }
    if (frames % 60 == 0) {
        logf_("pin: handle %u, last 60 frames: car was within 30 m of the target in %u of %u\n", ah, ok, tot);
        ok = tot = 0;
    }
}

/* ----------------------------------------------------------- per-frame tick */
static void frame_tick(void)
{
    static int down8, down9, down10, down11, pin_on;
    int k11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (k11 && !down11) { pin_on = !pin_on; logf_("F11: pin %s\n", pin_on ? "on" : "off"); }
    down11 = k11;
    pin_tick(pin_on);
    int k8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    int k9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    int k10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (k8 && !down8) dump_players();
    if (k9 && !down9) key_f9();
    if (k10 && !down10) key_f10();
    down8 = k8; down9 = k9; down10 = k10;
    verify_tick();
}

/* --------------------------------------------------------------- D3D11 proxy */
typedef HRESULT(WINAPI *PFN_Present)(void *self, UINT sync, UINT flags);
static PFN_Present g_present;

static HRESULT WINAPI hook_present(void *self, UINT sync, UINT flags)
{
    static int first = 1;
    if (first) { first = 0; logf_("first Present: per-frame tick is running\n"); }
    frame_tick();
    return g_present(self, sync, flags);
}

static void hook_swapchain(void *sc)
{
    if (!sc) return;
    void **vtbl = *(void ***)sc;
    if (!vtbl || vtbl[8] == (void *)hook_present) return;
    DWORD old;
    if (VirtualProtect(&vtbl[8], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
        g_present = (PFN_Present)vtbl[8];
        vtbl[8] = (void *)hook_present;
        VirtualProtect(&vtbl[8], sizeof(void *), old, &old);
        logf_("Present hooked\n");
    }
}

typedef HRESULT(WINAPI *PFN_CreateDevice)(void *, int, HMODULE, UINT, const void *, UINT, UINT, void **, void *, void **);
typedef HRESULT(WINAPI *PFN_CreateDeviceAndSwapChain)(void *, int, HMODULE, UINT, const void *, UINT, UINT, const void *,
                                                      void **, void **, void *, void **);
static HMODULE g_real;
static PFN_CreateDevice g_real_cd;
static PFN_CreateDeviceAndSwapChain g_real_cdsc;

static void load_real(void)
{
    if (g_real) return;
    char dir[MAX_PATH], path[MAX_PATH];
    GetSystemDirectoryA(dir, MAX_PATH);
    snprintf(path, sizeof path, "%s\\d3d11.dll", dir);
    g_real = LoadLibraryA(path);
    if (!g_real) { logf_("cannot load real d3d11.dll (%s)\n", path); return; }
    g_real_cd = (PFN_CreateDevice)GetProcAddress(g_real, "D3D11CreateDevice");
    g_real_cdsc = (PFN_CreateDeviceAndSwapChain)GetProcAddress(g_real, "D3D11CreateDeviceAndSwapChain");
    logf_("forwarding to %s\n", path);
}

HRESULT WINAPI proxy_D3D11CreateDevice(void *a, int dt, HMODULE sw, UINT fl, const void *lv, UINT nl, UINT sdk, void **dev,
                                       void *flo, void **ctx)
{
    load_real();
    return g_real_cd ? g_real_cd(a, dt, sw, fl, lv, nl, sdk, dev, flo, ctx) : E_FAIL;
}

HRESULT WINAPI proxy_D3D11CreateDeviceAndSwapChain(void *a, int dt, HMODULE sw, UINT fl, const void *lv, UINT nl, UINT sdk,
                                                   const void *desc, void **sc, void **dev, void *flo, void **ctx)
{
    load_real();
    if (!g_real_cdsc) return E_FAIL;
    HRESULT hr = g_real_cdsc(a, dt, sw, fl, lv, nl, sdk, desc, sc, dev, flo, ctx);
    if (SUCCEEDED(hr) && sc && *sc) hook_swapchain(*sc);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_logpath, MAX_PATH);
        char *slash = strrchr(g_logpath, '\\');
        if (slash) strcpy(slash + 1, "coop_hook.log");
        logf_("---- coop_d3d11 loaded ----\n");
    }
    return TRUE;
}
