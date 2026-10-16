/*
 * jvsbridge.dll - loaded into game.exe under Wine by tgm3inject.exe.
 *
 * Same trick as typex_io.dll: patch game.exe's kernel32 import slots so COM2 becomes a
 * fake handle. Instead of emulating the I/O board in-process, bytes go to the native
 * Linux jvsd over 127.0.0.1. The one thing a socket (or a pty) can't carry is the JVS
 * "sense" line, which the game reads as CTS via GetCommModemStatus, so it's derived here
 * by watching the game's reset (F0) and set-address (F1) packets.
 */
#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define COM_NAME       "COM2"
#define DEFAULT_PORT   26100
/* Kernel handles are multiples of 4, so an odd value can never collide with a real one. */
#define FAKE_COM       ((HANDLE)(ULONG_PTR)0x4A56530B)

static SOCKET sock = INVALID_SOCKET;
static COMMTIMEOUTS timeouts;
static DCB dcb = { sizeof(DCB), 115200 };
static BOOL sense;          /* CTS: set once the board has been given an address */
static HANDLE log_file = INVALID_HANDLE_VALUE;

/* TX sniffer state: position after sync (-1 = idle), pending escape, destination. */
static int tx_pos = -1;
static BOOL tx_escape;
static BYTE tx_dest;

static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static BOOL (WINAPI *real_CloseHandle)(HANDLE);
static BOOL (WINAPI *real_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL (WINAPI *real_WriteFile)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL (WINAPI *real_SetupComm)(HANDLE, DWORD, DWORD);
static BOOL (WINAPI *real_GetCommState)(HANDLE, LPDCB);
static BOOL (WINAPI *real_SetCommState)(HANDLE, LPDCB);
static BOOL (WINAPI *real_SetCommMask)(HANDLE, DWORD);
static BOOL (WINAPI *real_GetCommTimeouts)(HANDLE, LPCOMMTIMEOUTS);
static BOOL (WINAPI *real_SetCommTimeouts)(HANDLE, LPCOMMTIMEOUTS);
static BOOL (WINAPI *real_ClearCommError)(HANDLE, LPDWORD, LPCOMSTAT);
static BOOL (WINAPI *real_GetCommModemStatus)(HANDLE, LPDWORD);
static BOOL (WINAPI *real_EscapeCommFunction)(HANDLE, DWORD);

static void log_msg(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    DWORD written;
    if (log_file == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\r';
    buf[n++] = '\n';
    real_WriteFile(log_file, buf, n, &written, NULL);
}

static void lost_jvsd(void)
{
    log_msg("jvsd closed the connection, exiting");
    ExitProcess(0);
}

static void sniff(const BYTE *p, DWORD n)
{
    for (DWORD i = 0; i < n; i++) {
        BYTE b = p[i];
        if (b == 0xE0) { tx_pos = 0; tx_escape = FALSE; continue; }
        if (tx_pos < 0) continue;
        if (b == 0xD0) { tx_escape = TRUE; continue; }
        if (tx_escape) { b++; tx_escape = FALSE; }
        switch (tx_pos++) {
        case 0: tx_dest = b; break;
        case 1: break;                                   /* length */
        case 2:                                          /* first command */
            if (b == 0xF0) sense = FALSE;
            else if (b == 0xF1 && tx_dest == 0xFF) sense = TRUE;
            tx_pos = -1;
            break;
        }
    }
}

static BOOL connect_jvsd(void)
{
    char env[16];
    int port = DEFAULT_PORT;
    WSADATA wsa;
    if (GetEnvironmentVariableA("JVSD_PORT", env, sizeof env)) port = atoi(env);
    WSAStartup(MAKEWORD(2, 2), &wsa);

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for (int attempt = 0; attempt < 50; attempt++) {     /* jvsd may still be starting: wait up to 5s */
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connect(sock, (struct sockaddr *)&addr, sizeof addr) == 0) {
            BOOL one = TRUE;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            log_msg("connected to jvsd on port %d", port);
            return TRUE;
        }
        closesocket(sock);
        sock = INVALID_SOCKET;
        Sleep(100);
    }
    log_msg("could not reach jvsd on port %d", port);
    return FALSE;
}

static BOOL is_com(LPCSTR name)
{
    if (!name) return FALSE;
    if (!strncmp(name, "\\\\.\\", 4)) name += 4;
    return !lstrcmpiA(name, COM_NAME);
}

static HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disposition, DWORD flags, HANDLE tmpl)
{
    if (!is_com(name))
        return real_CreateFileA(name, access, share, sa, disposition, flags, tmpl);
    log_msg("opened com port");
    if (sock == INVALID_SOCKET && !connect_jvsd()) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    sense = FALSE;
    return FAKE_COM;
}

static BOOL WINAPI hook_CloseHandle(HANDLE h)
{
    if (h != FAKE_COM) return real_CloseHandle(h);
    log_msg("closed com port");
    closesocket(sock);
    sock = INVALID_SOCKET;
    return TRUE;
}

static BOOL WINAPI hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED ov)
{
    if (h != FAKE_COM) return real_WriteFile(h, buf, n, written, ov);
    sniff(buf, n);
    for (DWORD done = 0; done < n;) {
        int r = send(sock, (const char *)buf + done, (int)(n - done), 0);
        if (r <= 0) lost_jvsd();
        done += (DWORD)r;
    }
    if (written) *written = n;
    return TRUE;
}

/* Wait for data per COMMTIMEOUTS semantics; returns FALSE on timeout. */
static BOOL wait_readable(DWORD ms)
{
    fd_set set;
    struct timeval tv = { (long)(ms / 1000), (long)(ms % 1000) * 1000 };
    FD_ZERO(&set);
    FD_SET(sock, &set);
    return select(0, &set, NULL, NULL, ms == INFINITE ? NULL : &tv) > 0;
}

static BOOL WINAPI hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read_out, LPOVERLAPPED ov)
{
    if (h != FAKE_COM) return real_ReadFile(h, buf, n, read_out, ov);

    DWORD interval = timeouts.ReadIntervalTimeout;
    DWORD mult = timeouts.ReadTotalTimeoutMultiplier;
    DWORD total = timeouts.ReadTotalTimeoutConstant;
    BOOL immediate = interval == MAXDWORD && mult == 0 && total == 0;
    BOOL forever = interval == 0 && mult == 0 && total == 0;
    if (mult == MAXDWORD) mult = 0;
    DWORD budget = mult * n + total, start = GetTickCount(), got = 0;

    while (got < n) {
        DWORD ms;
        if (immediate) ms = 0;
        else if (forever) ms = INFINITE;
        else {
            DWORD spent = GetTickCount() - start;
            ms = spent >= budget ? 0 : budget - spent;
            if (got && interval && interval != MAXDWORD && interval < ms) ms = interval;
            if (!budget && interval && interval != MAXDWORD) ms = got ? interval : INFINITE;
        }
        if (!wait_readable(ms)) break;
        int r = recv(sock, (char *)buf + got, (int)(n - got), 0);
        if (r <= 0) lost_jvsd();
        got += (DWORD)r;
        if (immediate) break;
    }
    if (read_out) *read_out = got;
    return TRUE;
}

static BOOL WINAPI hook_ClearCommError(HANDLE h, LPDWORD errors, LPCOMSTAT stat)
{
    if (h != FAKE_COM) return real_ClearCommError(h, errors, stat);
    u_long avail = 0;
    ioctlsocket(sock, FIONREAD, &avail);
    if (errors) *errors = 0;
    if (stat) {
        ZeroMemory(stat, sizeof *stat);
        stat->cbInQue = avail;
    }
    return TRUE;
}

static BOOL WINAPI hook_GetCommModemStatus(HANDLE h, LPDWORD status)
{
    if (h != FAKE_COM) return real_GetCommModemStatus(h, status);
    *status = MS_DSR_ON | (sense ? MS_CTS_ON : 0);
    return TRUE;
}

static BOOL WINAPI hook_EscapeCommFunction(HANDLE h, DWORD func)
{
    return h == FAKE_COM ? TRUE : real_EscapeCommFunction(h, func);
}

static BOOL WINAPI hook_SetupComm(HANDLE h, DWORD in, DWORD out)
{
    if (h != FAKE_COM) return real_SetupComm(h, in, out);
    log_msg("SetupComm(%lu, %lu)", in, out);
    return TRUE;
}

static BOOL WINAPI hook_SetCommMask(HANDLE h, DWORD mask)
{
    return h == FAKE_COM ? TRUE : real_SetCommMask(h, mask);
}

static BOOL WINAPI hook_GetCommState(HANDLE h, LPDCB d)
{
    if (h != FAKE_COM) return real_GetCommState(h, d);
    *d = dcb;
    return TRUE;
}

static BOOL WINAPI hook_SetCommState(HANDLE h, LPDCB d)
{
    if (h != FAKE_COM) return real_SetCommState(h, d);
    dcb = *d;
    log_msg("SetCommState(baud %lu)", d->BaudRate);
    return TRUE;
}

static BOOL WINAPI hook_GetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t)
{
    if (h != FAKE_COM) return real_GetCommTimeouts(h, t);
    *t = timeouts;
    return TRUE;
}

static BOOL WINAPI hook_SetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t)
{
    if (h != FAKE_COM) return real_SetCommTimeouts(h, t);
    timeouts = *t;
    log_msg("SetCommTimeouts(%lu, %lu, %lu)", t->ReadIntervalTimeout,
            t->ReadTotalTimeoutMultiplier, t->ReadTotalTimeoutConstant);
    return TRUE;
}

/* Replace every import slot in module that currently points at real with hook. */
static int patch_iat(HMODULE module, void *real, void *hook)
{
    BYTE *base = (BYTE *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int patched = 0;
    if (!dir.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        for (IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + d->FirstThunk); t->u1.Function; t++) {
            if ((void *)t->u1.Function != real) continue;
            DWORD old;
            VirtualProtect(&t->u1.Function, sizeof t->u1.Function, PAGE_READWRITE, &old);
            t->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&t->u1.Function, sizeof t->u1.Function, old, &old);
            patched++;
        }
    }
    return patched;
}

#define HOOK(name) { #name, (void **)&real_##name, (void *)hook_##name }

static void install_hooks(void)
{
    static const struct { const char *name; void **real; void *hook; } hooks[] = {
        HOOK(CreateFileA), HOOK(CloseHandle), HOOK(ReadFile), HOOK(WriteFile),
        HOOK(SetupComm), HOOK(GetCommState), HOOK(SetCommState), HOOK(SetCommMask),
        HOOK(GetCommTimeouts), HOOK(SetCommTimeouts), HOOK(ClearCommError),
        HOOK(GetCommModemStatus), HOOK(EscapeCommFunction),
    };
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll"), game = GetModuleHandleA(NULL);

    /* Resolve everything first: log_msg needs real_WriteFile. */
    for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++)
        *hooks[i].real = (void *)GetProcAddress(kernel32, hooks[i].name);

    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("JVSBRIDGE_LOG", path, sizeof path))
        lstrcpyA(path, "jvsbridge.log");
    log_file = real_CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    log_msg("jvsbridge attached");

    for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++) {
        int n = patch_iat(game, *hooks[i].real, hooks[i].hook);
        log_msg("hooked %s (%d slot%s)", hooks[i].name, n, n == 1 ? "" : "s");
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        install_hooks();
    }
    return TRUE;
}
