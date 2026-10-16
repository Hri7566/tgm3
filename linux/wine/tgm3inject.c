/*
 * tgm3inject.exe <game.exe> <bridge.dll> [game args...]
 *
 * Stand-in for typex_loader.exe: start the game suspended, load the DLL into it, then let
 * it run. typex_loader used Detours' import-table rewrite; under Wine a remote
 * LoadLibraryA thread is enough and needs no detoured.dll.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <game.exe> <bridge.dll> [args...]\n", argv[0]);
        return 2;
    }

    char cmdline[4096] = "", dll[MAX_PATH], exe[MAX_PATH];
    for (int i = 1; i < argc; i++) {
        if (i == 2) continue;
        if (strlen(cmdline) + strlen(argv[i]) + 4 >= sizeof cmdline) return 2;
        strcat(cmdline, cmdline[0] ? " \"" : "\"");
        strcat(cmdline, argv[i]);
        strcat(cmdline, "\"");
    }
    /* Wine's CreateProcess search doesn't reliably include the cwd, so resolve both paths up front. */
    if (!GetFullPathNameA(argv[1], MAX_PATH, exe, NULL) || !GetFullPathNameA(argv[2], MAX_PATH, dll, NULL)) return 2;

    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmdline, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "tgm3inject: cannot start %s (error %lu)\n", argv[1], GetLastError());
        return 1;
    }

    DWORD module = 0;
    void *remote = VirtualAllocEx(pi.hProcess, NULL, sizeof dll, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    LPTHREAD_START_ROUTINE load = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE thread = NULL;
    if (remote && WriteProcessMemory(pi.hProcess, remote, dll, strlen(dll) + 1, NULL))
        thread = CreateRemoteThread(pi.hProcess, NULL, 0, load, remote, 0, NULL);
    if (thread) {
        WaitForSingleObject(thread, 10000);
        GetExitCodeThread(thread, &module);
        CloseHandle(thread);
    }
    if (!module) {
        fprintf(stderr, "tgm3inject: failed to load %s into the game\n", dll);
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
