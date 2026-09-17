/*
 * memgrab_test.c — ShieldCord memory-read probe (QA tool)
 * ----------------------------------------------------------
 * Simulates a memory-dumping grabber: it opens a PROCESS_VM_READ handle on a
 * protected process (Discord / Edge / Chrome / Brave) and HOLDS it.
 *
 * With the real service's Memory Monitor in kill mode, ShieldCord should
 * detect this untrusted VM_READ handle and TERMINATE this process within the
 * poll interval (~200 ms). If this tool is still alive after 15 seconds, the
 * kill did NOT happen — that is a finding.
 *
 * BUILD (host):  cl /nologo memgrab_test.c /Fe:memgrab_test.exe
 * RUN (VM):      memgrab_test.exe        (a protected app must be running)
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

static const wchar_t* kTargets[] = {
    L"discord.exe", L"msedge.exe", L"chrome.exe", L"brave.exe"
};

int wmain(void)
{
    wprintf(L"===============================================================\n");
    wprintf(L" ShieldCord memory-read probe\n");
    wprintf(L" Opens a PROCESS_VM_READ handle on a protected app and holds it.\n");
    wprintf(L"===============================================================\n");

    /* Find the first running protected process */
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                for (int i = 0; i < 4; i++) {
                    if (_wcsicmp(pe.szExeFile, kTargets[i]) == 0) {
                        pid = pe.th32ProcessID;
                        wprintf(L"[*] Target: %s (pid=%lu)\n", pe.szExeFile, pid);
                        break;
                    }
                }
                if (pid) break;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    if (!pid) {
        wprintf(L"[-] No protected app running (discord / msedge / chrome / brave).\n");
        wprintf(L"    Launch one first, then re-run this probe.\n");
        return 2;
    }

    HANDLE h = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        wprintf(L"[-] OpenProcess(PROCESS_VM_READ) failed, err=%lu\n", GetLastError());
        wprintf(L"    The target may have exited. Launch it and retry.\n");
        return 2;
    }

    wprintf(L"[+] VM_READ handle opened. Holding for 15 s...\n");
    wprintf(L"    ShieldCord's Memory Monitor should terminate me in ~1 s.\n");
    Sleep(15000);

    wprintf(L"[!] STILL ALIVE after 15 s - memory grab was NOT killed!\n");
    CloseHandle(h);
    return 3;
}
