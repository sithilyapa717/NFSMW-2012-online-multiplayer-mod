/*
 * dump_image: copies the main module of a RUNNING NFS13.exe out of memory.
 *
 * Why: the Steam/EA build of NFS13.exe is encrypted on disk and only turns
 * into real code once the game is running, so we analyse a dump of memory.
 *
 * Read-only: it only calls ReadProcessMemory. It never writes to the game.
 *
 *   dump_image.exe [process name, default NFS13.exe] [output folder, default .]
 *
 * Output:
 *   nfs13_image_<base>.bin   flat image, byte i = memory at base + i (unreadable pages = 0)
 *   nfs13_image_<base>.txt   base, size, readable/unreadable page counts, sanity checks
 *
 * In Ghidra: File > Import, format "Raw Binary", language x86:LE:32:default,
 * options: base address = the base printed in the .txt (normally 0x400000).
 *
 * Build (32-bit, like the game):
 *   zig cc -target x86-windows-gnu -O2 dump_image.c -o dump_image.exe
 */
#define WIN32_LEAN_AND_MEAN
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>

/* Prologues the upstream trace hook expects at these addresses. If they match
 * in our dump, the dump is aligned and decrypted as we think. */
static const struct {
    const char *name;
    uint32_t va;
    uint8_t bytes[8];
    int n;
} kChecks[] = {
    {"EventLookup", 0x006E5DC0, {0x53, 0x55, 0x8b, 0x6c, 0x24, 0x10, 0x57}, 7},
    {"ParsePlaylist", 0x006EB0D0, {0x81, 0xec, 0x94, 0x00, 0x00, 0x00}, 6},
    {"ValidatePlaylist", 0x006EA460, {0x56, 0x8b, 0xf1, 0x83, 0xbe, 0x40, 0x01, 0x00}, 8},
    {"AdapterConnect", 0x00C0E4F0, {0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x0c}, 7},
    {"ConnApiAddClient", 0x00B84430, {0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x08}, 7},
    {"NetworkAddPeer", 0x00C0E1B0, {0x55, 0x8b, 0xec, 0x81, 0xec, 0x9c, 0x00, 0x00}, 8},
};

int main(int argc, char **argv)
{
    const char *proc = argc > 1 ? argv[1] : "NFS13.exe";
    const char *outdir = argc > 2 ? argv[2] : ".";

    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (snap != INVALID_HANDLE_VALUE && Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, proc) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    if (snap != INVALID_HANDLE_VALUE)
        CloseHandle(snap);
    if (!pid) {
        fprintf(stderr, "%s is not running. Start the game first (get to the main menu or free roam).\n", proc);
        return 2;
    }

    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) {
        fprintf(stderr, "cannot open process %lu (error %lu). Try running this from an Administrator terminal.\n",
                (unsigned long)pid, (unsigned long)GetLastError());
        return 3;
    }

    /* main module = first module of the process */
    uintptr_t base = 0;
    uint32_t size = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    if (snap != INVALID_HANDLE_VALUE && Module32First(snap, &me)) {
        base = (uintptr_t)me.modBaseAddr;
        size = me.modBaseSize;
    }
    if (snap != INVALID_HANDLE_VALUE)
        CloseHandle(snap);
    if (!base || !size || size > 256u * 1024 * 1024) {
        fprintf(stderr, "could not find the main module (base %p size %u)\n", (void *)base, size);
        return 4;
    }

    uint8_t *img = (uint8_t *)calloc(1, size);
    if (!img)
        return 5;
    uint32_t ok_pages = 0, bad_pages = 0;
    for (uint32_t off = 0; off < size; off += 4096) {
        SIZE_T got = 0;
        uint32_t n = size - off < 4096 ? size - off : 4096;
        if (ReadProcessMemory(h, (LPCVOID)(base + off), img + off, n, &got) && got == n)
            ok_pages++;
        else {
            bad_pages++;
            memset(img + off, 0, n);
        }
    }
    CloseHandle(h);

    char binp[512], txtp[512];
    snprintf(binp, sizeof(binp), "%s\\nfs13_image_%08lx.bin", outdir, (unsigned long)base);
    snprintf(txtp, sizeof(txtp), "%s\\nfs13_image_%08lx.txt", outdir, (unsigned long)base);
    FILE *f = fopen(binp, "wb");
    if (!f || fwrite(img, 1, size, f) != size) {
        fprintf(stderr, "cannot write %s\n", binp);
        return 6;
    }
    fclose(f);

    FILE *t = fopen(txtp, "w");
    FILE *outs[2] = {stdout, t};
    for (int k = 0; k < 2; k++) {
        FILE *o = outs[k];
        if (!o)
            continue;
        fprintf(o, "process        %s (pid %lu)\n", proc, (unsigned long)pid);
        fprintf(o, "image base     0x%08lx\n", (unsigned long)base);
        fprintf(o, "image size     0x%x (%u bytes)\n", size, size);
        fprintf(o, "pages          %u readable, %u unreadable (left as zero)\n", ok_pages, bad_pages);
        fprintf(o, "dump file      %s\n", binp);
        fprintf(o, "sanity checks against the upstream trace hook's expected function prologues:\n");
    }
    int matched = 0, checked = 0;
    for (size_t i = 0; i < sizeof(kChecks) / sizeof(kChecks[0]); i++) {
        uint32_t va = kChecks[i].va;
        int in = va >= base && va - base + kChecks[i].n <= size;
        int match = in && memcmp(img + (va - base), kChecks[i].bytes, (size_t)kChecks[i].n) == 0;
        checked++;
        matched += match;
        for (int k = 0; k < 2; k++)
            if (outs[k])
                fprintf(outs[k], "  %-18s 0x%08x  %s\n", kChecks[i].name, va, match ? "MATCH" : (in ? "differs" : "outside image"));
    }
    for (int k = 0; k < 2; k++)
        if (outs[k])
            fprintf(outs[k], "%d of %d prologues match%s\n", matched, checked,
                    matched == checked ? ": this is the same build and the code is decrypted" : "");
    if (t)
        fclose(t);
    free(img);
    return matched == checked ? 0 : 1;
}
