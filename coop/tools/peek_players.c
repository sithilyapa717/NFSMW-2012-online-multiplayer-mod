/*
 * peek_players: READ-ONLY look at the running game's PlayerManager (Phase 2 check).
 * Prints every active player slot: type (0 local, 1 network, 2 AI), car handle,
 * and the car's position and velocity. Run it while the game is in free roam or in a race.
 *
 *   peek_players.exe            one snapshot
 *   peek_players.exe 500        repeat every 500 ms (Ctrl+C to stop)
 *
 * Build: zig cc -target x86-windows-gnu -O2 peek_players.c -o peek_players.exe
 * Chain used (from Ghidra, still unverified live):
 *   G  = *(0x01177868);  PM = *(G + 0x2F3DC);  slots: *(PM + 0xA5D0 + i*4), i = 0..23
 *   player type = *(p + 0x9C);  car handle = *(p + 0xA0)
 *   vehicle object = *(0x010EDB68 + 4 * (short)(*(short*)(*(*(0x010EDB04) + 4) + 0x293C) + handle))
 *   position (x,y,z,w) at obj + 0x110, velocity at obj + 0x120
 */
#define WIN32_LEAN_AND_MEAN
#include <stdint.h>
#include <stdio.h>
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

static void snapshot(void)
{
    uint32_t G = 0, PM = 0;
    if (!rd32(0x01177868, &G) || !G) { printf("G (0x01177868) not readable/null\n"); return; }
    if (!rd32(G + 0x2F3DC, &PM) || !PM) { printf("G=%08x but PlayerManager pointer is null\n", G); return; }
    uint32_t racers = 0, cops = 0, localIdx = 0, active = 0;
    rd32(PM + 0xA694, &racers); rd32(PM + 0xA660, &cops);
    rd32(PM + 0xA6B4, &localIdx); rd32(PM + 0xA6A0, &active);
    printf("G=%08x PM=%08x racers=%u cops=%u active=%u localIdx=%d\n", G, PM, racers, cops, active, (int)localIdx);

    uint32_t mgr = 0, data = 0, off = 0;
    int haveVeh = rd32(0x010EDB04, &mgr) && mgr && rd32(mgr + 4, &data) && data && rd32(data + 0x293C, &off);
    for (int i = 0; i < 24; i++) {
        uint32_t p = 0;
        if (!rd32(PM + 0xA5D0 + i * 4, &p) || !p) continue;
        uint32_t type = 0, h = 0, flags = 0;
        rd32(p + 0x9C, &type); rd32(p + 0xA0, &h);
        rd(p + 0x23D, &flags, 1);
        printf("  slot %2d obj=%08x type=%u carHandle=%d active=%u", i, p, type, (int)h, flags & 0xff);
        if (haveVeh && (int)h != -1) {
            short idx = (short)((short)(off & 0xffff) + (short)h);
            uint32_t veh = 0;
            if (rd32(0x010EDB68 + (uint32_t)(int)idx * 4, &veh) && veh) {
                float pos[4], vel[3];
                if (rd(veh + 0x110, pos, 16) && rd(veh + 0x120, vel, 12))
                    printf("  veh=%08x pos=(%.1f, %.1f, %.1f) vel=(%.2f, %.2f, %.2f)", veh, pos[0], pos[1], pos[2],
                           vel[0], vel[1], vel[2]);
            }
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    int every = argc > 1 ? atoi(argv[1]) : 0;
    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
    if (s != INVALID_HANDLE_VALUE && Process32First(s, &pe))
        do { if (_stricmp(pe.szExeFile, "NFS13.exe") == 0) { pid = pe.th32ProcessID; break; } } while (Process32Next(s, &pe));
    if (s != INVALID_HANDLE_VALUE) CloseHandle(s);
    if (!pid) { fprintf(stderr, "NFS13.exe is not running\n"); return 2; }
    g_h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!g_h) { fprintf(stderr, "cannot open process (error %lu); try an Administrator terminal\n", GetLastError()); return 3; }
    do {
        snapshot();
        if (every) { printf("\n"); Sleep(every); }
    } while (every);
    return 0;
}
