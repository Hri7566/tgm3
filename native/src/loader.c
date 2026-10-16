/*
 * tgm3-native: run TGM3's game.exe on x86 Linux without Wine.
 *
 * Maps the PE at its fixed base (it has no relocations), points its import table at the
 * C implementations in this directory, gives each thread a Windows TEB through fs:, and
 * jumps to the entry point.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <asm/ldt.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "w32.h"

static uint32_t image_base, image_size, import_rva;
static int fs_entry = -1;

/* ---- TEB / fs ---- */

static uint8_t *peb;
static __thread uint8_t *teb;

void thread_setup(void)
{
    teb = mmap(NULL, TEB_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (teb == MAP_FAILED) w32_fatal("TEB alloc failed");
    gptr t = H2G(teb);
    g_teb = t;
    wr32(t + TEB_EXCEPTION, 0xFFFFFFFF);
    pthread_attr_t attr;
    void *stack_lo;
    size_t stack_size;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &stack_lo, &stack_size);
        wr32(t + TEB_STACKBASE, H2G(stack_lo) + (uint32_t)stack_size);
        wr32(t + TEB_STACKLIMIT, H2G(stack_lo));
        pthread_attr_destroy(&attr);
    }
    wr32(t + TEB_SELF, t);
    wr32(t + TEB_PID, (uint32_t)getpid());
    wr32(t + TEB_TID, (uint32_t)syscall(SYS_gettid));
    wr32(t + TEB_PEB, H2G(peb));

    struct user_desc desc = {
        .entry_number = (unsigned)fs_entry, .base_addr = t, .limit = TEB_SIZE / 4096 - 1,
        .seg_32bit = 1, .contents = 0, .read_exec_only = 0, .limit_in_pages = 1,
        .seg_not_present = 0, .useable = 1,
    };
    if (syscall(SYS_set_thread_area, &desc) != 0) w32_fatal("set_thread_area: %s", strerror(errno));
    fs_entry = (int)desc.entry_number;
    uint16_t sel = (uint16_t)(desc.entry_number << 3 | 3);
    __asm__ volatile("mov %0, %%fs" : : "r"(sel));

    /* Windows starts threads with 53-bit x87 precision; Linux uses 64-bit. */
    uint16_t cw = 0x27F;
    __asm__ volatile("fldcw %0" : : "m"(cw));
}


/* ---- PE loading ---- */

static uint8_t *read_file(const char *path, size_t *size)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) return NULL;
    uint8_t *buf = malloc((size_t)st.st_size);
    ssize_t got = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (got != st.st_size) { free(buf); return NULL; }
    *size = (size_t)st.st_size;
    return buf;
}

static uint32_t load_pe(const char *path)
{
    size_t size;
    uint8_t *file = read_file(path, &size);
    if (!file) w32_fatal("cannot read %s", path);
    uint32_t pe = *(uint32_t *)(file + 0x3C);
    if (memcmp(file, "MZ", 2) || pe + 0xF8 > size || memcmp(file + pe, "PE\0\0", 4))
        w32_fatal("%s is not a PE file", path);
    uint8_t *coff = file + pe + 4, *opt = coff + 20;
    uint16_t nsections = *(uint16_t *)(coff + 2), opt_size = *(uint16_t *)(coff + 16);
    if (*(uint16_t *)coff != 0x14C || *(uint16_t *)opt != 0x10B) w32_fatal("%s is not a 32-bit x86 PE", path);
    image_base = *(uint32_t *)(opt + 28);
    image_size = *(uint32_t *)(opt + 56);
    uint32_t entry = *(uint32_t *)(opt + 16), headers = *(uint32_t *)(opt + 60);

    void *want = (void *)(uintptr_t)image_base;
    void *img = mmap(want, image_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (img != want) w32_fatal("cannot map image at %#x (%s)", image_base, strerror(errno));
    memcpy(img, file, headers);
    uint8_t *sec = opt + opt_size;
    for (int i = 0; i < nsections; i++, sec += 40) {
        uint32_t vaddr = *(uint32_t *)(sec + 12), raw_size = *(uint32_t *)(sec + 16), raw = *(uint32_t *)(sec + 20);
        if (raw_size && raw + raw_size <= size)
            memcpy((uint8_t *)img + vaddr, file + raw, raw_size);
    }

    import_rva = *(uint32_t *)(opt + 96 + 8);
    free(file);
    return image_base + entry;
}

/* Point every IAT slot at our implementation (needs video_init first for OpenGL). */
static void resolve_imports(void)
{
    uint8_t *img = (uint8_t *)(uintptr_t)image_base;
    int unresolved = 0;
    for (uint8_t *d = img + import_rva; *(uint32_t *)(d + 12); d += 20) {
        const char *dll = (const char *)img + *(uint32_t *)(d + 12);
        uint32_t lookup = *(uint32_t *)d ? *(uint32_t *)d : *(uint32_t *)(d + 16);
        uint32_t *names = (uint32_t *)((uint8_t *)img + lookup);
        uint32_t *iat = (uint32_t *)((uint8_t *)img + *(uint32_t *)(d + 16));
        for (int i = 0; names[i]; i++) {
            const char *name = NULL;
            int ordinal = 0;
            char ordname[16];
            if (names[i] & 0x80000000u) {
                ordinal = (int)(names[i] & 0xFFFF);
                snprintf(ordname, sizeof ordname, "#%d", ordinal);
            } else {
                name = (const char *)img + names[i] + 2;
            }
            gptr fn = w32_resolve(dll, name, ordinal);
            if (!fn) {
                LOG("unresolved import %s!%s", dll, name ? name : ordname);
                unresolved++;
                fn = w32_missing_stub(dll, name ? name : ordname);
            }
            iat[i] = fn;
        }
    }
    if (unresolved) WARN("%d import(s) unresolved; they abort if called", unresolved);
}

/* ---- crash reporting ---- */

static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    greg_t *r = uc->uc_mcontext.gregs;
    uint32_t eip = (uint32_t)r[REG_EIP];
    fprintf(stderr, "\ntgm3-native: %s at eip=%08x%s (fault addr %p)\n", strsignal(sig), eip,
            eip >= image_base && eip < image_base + image_size ? " [game.exe]" : "", si->si_addr);
    fprintf(stderr, "  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
            (uint32_t)r[REG_EAX], (uint32_t)r[REG_EBX], (uint32_t)r[REG_ECX], (uint32_t)r[REG_EDX],
            (uint32_t)r[REG_ESI], (uint32_t)r[REG_EDI], (uint32_t)r[REG_EBP], (uint32_t)r[REG_ESP]);
    /* Return addresses into game.exe found on the stack: a rough call trace. */
    uint32_t *sp = (uint32_t *)(uintptr_t)r[REG_ESP];
    fprintf(stderr, "  stack game.exe addrs:");
    for (int i = 0, n = 0; i < 512 && n < 16; i++)
        if (sp[i] >= image_base + 0x1000 && sp[i] < image_base + 0x67000) { fprintf(stderr, " %08x", sp[i]); n++; }
    fputc('\n', stderr);
    _exit(128 + sig);
}

static void install_crash_handler(void)
{
    static uint8_t altstack[65536];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, NULL);
    struct sigaction sa = { .sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | SA_ONSTACK };
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-v] [--trace] [game-dir]\n"
            "  game-dir defaults to $TGM3_DIR or the current directory; it must contain game.exe\n", argv0);
}

int main(int argc, char **argv)
{
    const char *dir = getenv("TGM3_DIR");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose++;
        else if (!strcmp(argv[i], "--trace")) { g_trace = true; g_verbose++; }
        else if (argv[i][0] == '-') { usage(argv[0]); return 2; }
        else dir = argv[i];
    }
    if (!realpath(dir ? dir : ".", g_root)) w32_fatal("bad game directory: %s", strerror(errno));

    char exe[4200];
    snprintf(exe, sizeof exe, "%s/game.exe", g_root);
    install_crash_handler();
    peb = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    uint32_t entry = load_pe(exe);               /* map first so nothing else lands at 0x400000 */
    wr32(H2G(peb) + 8, image_base);              /* PEB.ImageBaseAddress */
    if (!video_init()) return 1;
    resolve_imports();
    comm_init();
    thread_setup();

    LOG("entering game.exe at %08x", entry);
    guest_call(entry, 0, NULL);
    return 0;
}
