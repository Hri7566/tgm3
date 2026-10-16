/* x86 entry thunks: how guest stdcall code reaches the C implementations (native loader only). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>

#include "w32.h"

struct entry_info {
    const char *dll;
    const char *name;
    int nargs;
    w32_fn fn;
};

static uint8_t *code_base, *code_next;
static size_t code_left;
static pthread_mutex_t code_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t *code_alloc(size_t n)
{
    pthread_mutex_lock(&code_lock);
    if (code_left < n) {
        size_t size = 1 << 20;
        code_base = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code_base == MAP_FAILED) w32_fatal("thunk mmap failed");
        code_next = code_base;
        code_left = size;
    }
    uint8_t *p = code_next;
    code_next += (n + 15) & ~15u;
    code_left -= (n + 15) & ~15u;
    pthread_mutex_unlock(&code_lock);
    return p;
}

static inline uint8_t *put8(uint8_t *p, uint8_t v) { *p = v; return p + 1; }
static inline uint8_t *put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); return p + 2; }
static inline uint8_t *put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); return p + 4; }

/* Called (cdecl, 16-byte aligned stack) by every API thunk. */
__attribute__((used)) static uint32_t w32_dispatch(const struct entry_info *e, uint32_t *args)
{
    if (g_trace) {
        char buf[160];
        int n = 0;
        for (int i = 0; i < e->nargs && i < 8 && n < (int)sizeof buf - 12; i++)
            n += snprintf(buf + n, sizeof buf - n, "%s%x", i ? ", " : "", args[i]);
        buf[n] = 0;
        w32_log("-> %s!%s(%s)", e->dll, e->name, buf);
    }
    return e->fn(args);
}

gptr w32_entry(const struct w32_export *x, const char *dll)
{
    struct entry_info *e = malloc(sizeof *e);
    *e = (struct entry_info){ dll, x->name, x->nargs, x->fn };

    /*  push ebp; mov ebp,esp; and esp,-16; sub esp,8; lea eax,[ebp+8]; push eax; push e;
        mov eax,w32_dispatch; call eax; leave; ret nargs*4 */
    uint8_t *code = code_alloc(40), *p = code;
    p = put8(p, 0x55);
    p = put8(p, 0x89); p = put8(p, 0xE5);
    p = put8(p, 0x83); p = put8(p, 0xE4); p = put8(p, 0xF0);
    p = put8(p, 0x83); p = put8(p, 0xEC); p = put8(p, 0x08);
    p = put8(p, 0x8D); p = put8(p, 0x45); p = put8(p, 0x08);
    p = put8(p, 0x50);
    p = put8(p, 0x68); p = put32(p, H2G(e));
    p = put8(p, 0xB8); p = put32(p, H2G(&w32_dispatch));
    p = put8(p, 0xFF); p = put8(p, 0xD0);
    p = put8(p, 0xC9);
    p = put8(p, 0xC2); p = put16(p, (uint16_t)(x->nargs * 4));
    return H2G(code);
}

/* Imports we don't implement get a stub that stops with the name, rather than jumping to 0. */
__attribute__((noreturn, force_align_arg_pointer)) static void missing_fatal(const char *what)
{
    w32_fatal("game called unimplemented import %s", what);
}

gptr w32_missing_stub(const char *dll, const char *name)
{
    char *what = malloc(strlen(dll) + strlen(name) + 2);
    sprintf(what, "%s!%s", dll, name);
    /* push what; mov eax,missing_fatal; call eax */
    uint8_t *code = code_alloc(16), *p = code;
    p = put8(p, 0x68); p = put32(p, H2G(what));
    p = put8(p, 0xB8); p = put32(p, H2G(&missing_fatal));
    p = put8(p, 0xFF); p = put8(p, 0xD0);
    return H2G(code);
}
