/*
 * tgm3-recomp host side: loads game.exe's data into guest memory, connects the recompiled
 * code to the Win32 layer in ../native/src, and starts it.
 *
 * Win32 functions are reached through "entry addresses" in a range no guest code occupies;
 * the dispatcher recognizes them and runs the C implementation against the guest stack.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "w32.h"
#include "rt_host.h"

__thread CPU *g_cpu;

/* ---- Win32 entry points ---- */

#define HLE_BASE 0xF0000000u
#define HLE_STRIDE 16u
#define HLE_MAX 8192

struct hle {
    const char *dll, *name;
    int nargs;
    w32_fn fn;
};
static struct hle hle_tab[HLE_MAX];
static int nhle;
static pthread_mutex_t hle_lock = PTHREAD_MUTEX_INITIALIZER;

static gptr hle_new(const char *dll, const char *name, int nargs, w32_fn fn)
{
    pthread_mutex_lock(&hle_lock);
    if (nhle == HLE_MAX) w32_fatal("too many Win32 entry points");
    int i = nhle;
    hle_tab[i] = (struct hle){ dll, name, nargs, fn };
    __atomic_store_n(&nhle, i + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&hle_lock);
    return HLE_BASE + (uint32_t)i * HLE_STRIDE;
}

gptr w32_entry(const struct w32_export *x, const char *dll)
{
    return hle_new(dll, x->name, x->nargs, x->fn);
}

static uint32_t missing(uint32_t *a) { (void)a; return 0; }

gptr w32_missing_stub(const char *dll, const char *name)
{
    char *what = malloc(strlen(dll) + strlen(name) + 2);
    sprintf(what, "%s!%s", dll, name);
    return hle_new("missing", what, 0, missing);
}

int hle_call(CPU *c, uint32_t t)
{
    uint32_t i = (t - HLE_BASE) / HLE_STRIDE;
    if (t < HLE_BASE || (t - HLE_BASE) % HLE_STRIDE || i >= (uint32_t)__atomic_load_n(&nhle, __ATOMIC_ACQUIRE)) return 0;
    const struct hle *e = &hle_tab[i];
    if (e->fn == missing) rt_fatal(c, "game called unimplemented import %s", e->name);
    uint32_t ret = RD32(c->esp);
    uint32_t *args = G2H(uint32_t *, c->esp + 4);
    if (g_trace) {
        char buf[160];
        int n = 0;
        for (int k = 0; k < e->nargs && k < 8 && n < (int)sizeof buf - 12; k++)
            n += snprintf(buf + n, sizeof buf - n, "%s%x", k ? ", " : "", args[k]);
        buf[n] = 0;
        w32_log("-> %s!%s(%s) from %08x", e->dll, e->name, buf, ret);
    }
    uint32_t r = e->fn(args);
    c->eax = r;
    c->esp += 4 + 4 * (uint32_t)e->nargs;
    c->ret_to = ret;
    return 1;
}

#define GUEST_RET_MAGIC 0xFEEDF00Du

uint32_t guest_call(gptr fn, int nargs, const uint32_t *args)
{
    CPU *c = g_cpu;
    CPU saved = *c;
    uint32_t esp = c->esp & ~3u;
    for (int i = nargs - 1; i >= 0; i--) { esp -= 4; WR32(esp, args[i]); }
    esp -= 4;
    WR32(esp, GUEST_RET_MAGIC);
    c->esp = esp;
    rt_call(c, fn);
    uint32_t r = c->eax;
    c->eax = saved.eax; c->ecx = saved.ecx; c->edx = saved.edx; c->ebx = saved.ebx;
    c->esp = saved.esp; c->ebp = saved.ebp; c->esi = saved.esi; c->edi = saved.edi;
    c->ret_to = saved.ret_to;
    c->eip = saved.eip;
    return r;
}

/* ---- threads ---- */

#define GUEST_STACK (4u << 20)
static gptr peb;

void thread_setup(void)
{
    CPU *c = calloc(1, sizeof *c);
    gptr teb = guest_alloc(TEB_SIZE);
    gptr stack = guest_alloc(GUEST_STACK);
    if (!teb || !stack) w32_fatal("out of guest memory for a new thread");
    g_teb = teb;
    wr32(teb + TEB_EXCEPTION, 0xFFFFFFFF);
    wr32(teb + TEB_STACKBASE, stack + GUEST_STACK);
    wr32(teb + TEB_STACKLIMIT, stack);
    wr32(teb + TEB_SELF, teb);
    wr32(teb + TEB_PID, (uint32_t)getpid());
    wr32(teb + TEB_TID, (uint32_t)syscall(SYS_gettid));
    wr32(teb + TEB_PEB, peb);
    c->fs_base = teb;
    c->esp = stack + GUEST_STACK - 64;
    c->fcw = 0x27F;                  /* Windows: 53-bit precision, round to nearest */
    g_cpu = c;
}

/* ---- PE loading ---- */

static uint32_t image_base, image_size, import_rva;

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

/* The code is already compiled in; only headers and data matter, but all sections are
   copied so the guest sees its image exactly as Windows would map it. */
static void load_pe(const char *path)
{
    size_t size;
    uint8_t *file = read_file(path, &size);
    if (!file) w32_fatal("cannot read %s", path);
    uint32_t pe = *(uint32_t *)(file + 0x3C);
    if (memcmp(file, "MZ", 2) || pe + 0xF8 > size || memcmp(file + pe, "PE\0\0", 4))
        w32_fatal("%s is not a PE file", path);
    uint8_t *coff = file + pe + 4, *opt = coff + 20;
    uint16_t nsections = *(uint16_t *)(coff + 2), opt_size = *(uint16_t *)(coff + 16);
    image_base = *(uint32_t *)(opt + 28);
    image_size = *(uint32_t *)(opt + 56);
    uint32_t entry = image_base + *(uint32_t *)(opt + 16), headers = *(uint32_t *)(opt + 60);
    if (entry != rc_entry) w32_fatal("%s is not the game.exe this build was recompiled from", path);

    if (gmem_reserve(image_base, image_size) != image_base) w32_fatal("cannot map image at %#x", image_base);
    gmem_protect(image_base, image_size, PROT_READ | PROT_WRITE);
    uint8_t *img = G2H(uint8_t *, image_base);
    memcpy(img, file, headers);
    uint8_t *sec = opt + opt_size;
    for (int i = 0; i < nsections; i++, sec += 40) {
        uint32_t vaddr = *(uint32_t *)(sec + 12), raw_size = *(uint32_t *)(sec + 16), raw = *(uint32_t *)(sec + 20);
        if (raw_size && raw + raw_size <= size) memcpy((uint8_t *)img + vaddr, file + raw, raw_size);
    }
    import_rva = *(uint32_t *)(opt + 96 + 8);
    free(file);
}

static void resolve_imports(void)
{
    int unresolved = 0;
    for (gptr d = image_base + import_rva; rd32(d + 12); d += 20) {
        const char *dll = G2H(const char *, image_base + rd32(d + 12));
        gptr names = image_base + (rd32(d) ? rd32(d) : rd32(d + 16));
        gptr iat = image_base + rd32(d + 16);
        for (int i = 0; rd32(names + 4u * i); i++) {
            uint32_t v = rd32(names + 4u * i);
            const char *name = NULL;
            int ordinal = 0;
            char ordname[16];
            if (v & 0x80000000u) {
                ordinal = (int)(v & 0xFFFF);
                snprintf(ordname, sizeof ordname, "#%d", ordinal);
            } else {
                name = G2H(const char *, image_base + v + 2);
            }
            gptr fn = w32_resolve(dll, name, ordinal);
            if (!fn) {
                LOG("unresolved import %s!%s", dll, name ? name : ordname);
                unresolved++;
                fn = w32_missing_stub(dll, name ? name : ordname);
            }
            wr32(iat + 4u * i, fn);
        }
    }
    if (unresolved) WARN("%d import(s) unresolved; they abort if called", unresolved);
}

/* ---- crash reporting ---- */

static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "\ntgm3-recomp: %s (fault addr %p, guest %08x)\n", strsignal(sig), si->si_addr,
            (uint32_t)((uint8_t *)si->si_addr - M));
    if (g_cpu) rt_dump(g_cpu);
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
    gmem_init();
    load_pe(exe);
    peb = guest_alloc(0x1000);
    wr32(peb + 8, image_base);                   /* PEB.ImageBaseAddress */
    rt_init_dispatch();
    if (!video_init()) return 1;
    resolve_imports();
    comm_init();
    thread_setup();

    LOG("entering recompiled game.exe at %08x", rc_entry);
    CPU *c = g_cpu;
    c->esp -= 4;
    WR32(c->esp, GUEST_RET_MAGIC);
    rt_call(c, rc_entry);
    return (int)c->eax;
}
