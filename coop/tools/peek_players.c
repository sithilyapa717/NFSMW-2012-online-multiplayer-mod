/*
 * peek_players: READ-ONLY look at the running game's PlayerManager (Phase 2 check).
 * Never writes to the game.
 *
 *   peek_players.exe          one snapshot: every active player slot (type 0 local, 1 network, 2 AI),
 *                             car handle, and what we think is the car's position/velocity
 *   peek_players.exe deep     for the LOCAL player's car: lists every float with a plausible world
 *                             value in the vehicle object / vehicle record. Run it twice while the
 *                             car is at different places and compare which offsets changed.
 *   peek_players.exe 500      snapshot every 500 ms (Ctrl+C to stop)
 *
 * Build: zig cc -target x86-windows-gnu -O2 peek_players.c -o peek_players.exe
 * Chain (from Ghidra; the player part is VERIFIED live, the car part is not):
 *   G  = *(0x01177868);  PM = *(G + 0x2F3DC);  slots: *(PM + 0xA5D0 + i*4), i = 0..23
 *   player type = *(p + 0x9C);  car handle = *(p + 0xA0)
 *   vehicle entry = *(0x010EDB68 + 4 * (short)(*(short*)(*(*(0x010EDAE4) + 4) + 0x293C) + handle))
 */
#define WIN32_LEAN_AND_MEAN
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>

static HANDLE g_h;

static int rd(uint32_t a, void *out, uint32_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(g_h, (LPCVOID)(uintptr_t)a, out, n, &got) && got == n;
}
static int rd32(uint32_t a, uint32_t *v) { return rd(a, v, 4); }

static uint32_t vehicle_entry(uint32_t h, uint32_t *mgr_o, uint32_t *data_o, uint32_t *off_o)
{
    uint32_t mgr = 0, data = 0, off = 0, veh = 0;
    if (!rd32(0x010EDAE4, &mgr) || !mgr || !rd32(mgr + 4, &data) || !data || !rd32(data + 0x293C, &off))
        return 0;
    if (mgr_o) *mgr_o = mgr;
    if (data_o) *data_o = data;
    if (off_o) *off_o = off;
    short idx = (short)((short)(off & 0xffff) + (short)h);
    rd32(0x010EDB68 + (uint32_t)(int)idx * 4, &veh);
    return veh;
}

static void snapshot(void)
{
    uint32_t G = 0, PM = 0;
    if (!rd32(0x01177868, &G) || !G) { printf("G (0x01177868) not readable/null\n"); return; }
    if (!rd32(G + 0x2F3DC, &PM) || !PM) { printf("G=%08x but PlayerManager pointer is null\n", G); return; }
    uint32_t racers = 0, cops = 0, localIdx = 0, active = 0;
    rd32(PM + 0xA694, &racers); rd32(PM + 0xA660, &cops);
    rd32(PM + 0xA6B4, &localIdx); rd32(PM + 0xA6A0, &active);
    printf("G=%08x PM=%08x racers=%u cops=%u active=%u localIdx=%d\n", G, PM, racers, cops, active, (int)localIdx);
    for (int i = 0; i < 24; i++) {
        uint32_t p = 0;
        if (!rd32(PM + 0xA5D0 + i * 4, &p) || !p) continue;
        uint32_t type = 0, h = 0, flags = 0;
        rd32(p + 0x9C, &type); rd32(p + 0xA0, &h);
        rd(p + 0x23D, &flags, 1);
        printf("  slot %2d obj=%08x type=%u carHandle=%d active=%u", i, p, type, (int)h, flags & 0xff);
        if ((int)h != -1) {
            uint32_t veh = vehicle_entry(h, 0, 0, 0);
            if (veh) {
                float pos[4], vel[3];
                if (rd(veh + 0x110, pos, 16) && rd(veh + 0x120, vel, 12))
                    printf("  veh=%08x pos=(%.1f, %.1f, %.1f) vel=(%.2f, %.2f, %.2f)", veh, pos[0], pos[1], pos[2],
                           vel[0], vel[1], vel[2]);
            }
        }
        printf("\n");
    }
}

/* prints every float in [addr, addr+len) with a plausible world magnitude */
static void floats(const char *label, uint32_t addr, uint32_t len)
{
    static uint8_t buf[0x2000];
    if (len > sizeof buf || !rd(addr, buf, len)) { printf("%s: unreadable\n", label); return; }
    printf("%s @%08x:", label, addr);
    for (uint32_t o = 0; o + 4 <= len; o += 4) {
        float f;
        memcpy(&f, buf + o, 4);
        if (f == f && ((f > 1.0f && f < 100000.0f) || (f < -1.0f && f > -100000.0f)))
            printf(" +%x=%.2f", o, f);
    }
    printf("\n");
}

static void deep(void)
{
    uint32_t G = 0, PM = 0, mgr = 0, data = 0, off = 0, p = 0, h = 0;
    rd32(0x01177868, &G);
    rd32(G + 0x2F3DC, &PM);
    rd32(PM + 0xA5D0, &p);
    rd32(p + 0xA0, &h);
    uint32_t veh = vehicle_entry(h, &mgr, &data, &off);
    printf("mgr=%08x data=%08x off=%u local player=%08x handle=%d veh=%08x\n", mgr, data, off, p, (int)h, veh);
    floats("veh object", veh, 0x400);
    floats("record (data+0x3000+h*0x1c40)", data + 0x3000 + h * 0x1c40, 0x1c40);
}

int main(int argc, char **argv)
{
    int dodeep = argc > 1 && strcmp(argv[1], "deep") == 0;
    int every = (!dodeep && argc > 1) ? atoi(argv[1]) : 0;
    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (s != INVALID_HANDLE_VALUE && Process32First(s, &pe))
        do {
            if (_stricmp(pe.szExeFile, "NFS13.exe") == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(s, &pe));
    if (s != INVALID_HANDLE_VALUE) CloseHandle(s);
    if (!pid) { fprintf(stderr, "NFS13.exe is not running\n"); return 2; }
    g_h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!g_h) { fprintf(stderr, "cannot open process (error %lu); try an Administrator terminal\n", GetLastError()); return 3; }
    if (dodeep) { deep(); return 0; }
    do {
        snapshot();
        if (every) { printf("\n"); Sleep(every); }
    } while (every);
    return 0;
}
