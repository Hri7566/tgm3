/* Pieces of the Win32 layer shared by the native loader and the recompiled build:
   logging, the handle table, per-thread TEB fields, and import resolution. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "w32.h"

char g_root[4096];
bool g_trace;
int g_verbose;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void w32_log(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "[%5ld] ", (long)syscall(SYS_gettid));
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&log_lock);
}

void w32_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("tgm3-native: fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    _exit(1);
}

/* ---- handles ---- */

#define MAX_HANDLES 4096
#define HANDLE_BASE 0x1000
static struct object handles[MAX_HANDLES];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t handle_new(enum obj_type type, void *data, void (*destroy)(struct object *))
{
    pthread_mutex_lock(&handle_lock);
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (handles[i].type != OBJ_NONE) continue;
        handles[i] = (struct object){ type, data, destroy };
        pthread_mutex_unlock(&handle_lock);
        return HANDLE_BASE + (uint32_t)i * 4;
    }
    pthread_mutex_unlock(&handle_lock);
    w32_fatal("out of handles");
}

struct object *handle_get(uint32_t h, enum obj_type type)
{
    if (h < HANDLE_BASE || (h - HANDLE_BASE) % 4) return NULL;
    uint32_t i = (h - HANDLE_BASE) / 4;
    if (i >= MAX_HANDLES || handles[i].type != type) return NULL;
    return &handles[i];
}

void handle_close(uint32_t h)
{
    if (h < HANDLE_BASE || (h - HANDLE_BASE) % 4 || (h - HANDLE_BASE) / 4 >= MAX_HANDLES) return;
    struct object *o = &handles[(h - HANDLE_BASE) / 4];
    pthread_mutex_lock(&handle_lock);
    struct object copy = *o;
    o->type = OBJ_NONE;
    pthread_mutex_unlock(&handle_lock);
    if (copy.type != OBJ_NONE && copy.destroy) copy.destroy(&copy);
}

/* ---- TEB fields (the TEB itself is set up by the loader's thread_setup) ---- */

__thread gptr g_teb;

uint32_t *teb_tls_slots(void) { return G2H(uint32_t *, g_teb + TEB_TLS); }
void set_last_error(uint32_t e) { if (g_teb) wr32(g_teb + TEB_LASTERROR, e); }
uint32_t get_last_error(void) { return g_teb ? rd32(g_teb + TEB_LASTERROR) : 0; }
uint32_t teb_exception_list(void) { return rd32(g_teb + TEB_EXCEPTION); }
void teb_set_exception_list(uint32_t v) { wr32(g_teb + TEB_EXCEPTION, v); }

/* ---- import resolution ---- */

static const struct w32_dll dlls[] = {
    { "kernel32.dll", kernel32_exports },
    { "user32.dll",   user32_exports },
    { "gdi32.dll",    gdi32_exports },
    { "winmm.dll",    winmm_exports },
    { "opengl32.dll", opengl32_exports },
    { "dsound.dll",   dsound_exports },
};

/* Cache of created entries so repeated lookups (GetProcAddress) return the same address. */
struct cached { const struct w32_export *e; gptr entry; };
static struct cached cache[512];
static int ncache;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

gptr w32_resolve(const char *dll, const char *name, int ordinal)
{
    for (size_t d = 0; d < sizeof dlls / sizeof *dlls; d++) {
        if (strcasecmp(dll, dlls[d].name)) continue;
        for (const struct w32_export *e = dlls[d].exports; e->name; e++) {
            if (name ? strcmp(e->name, name) : e->ordinal != ordinal) continue;
            pthread_mutex_lock(&cache_lock);
            for (int i = 0; i < ncache; i++)
                if (cache[i].e == e) { pthread_mutex_unlock(&cache_lock); return cache[i].entry; }
            pthread_mutex_unlock(&cache_lock);
            gptr entry = w32_entry(e, dlls[d].name);
            pthread_mutex_lock(&cache_lock);
            if (ncache < (int)(sizeof cache / sizeof *cache)) cache[ncache++] = (struct cached){ e, entry };
            pthread_mutex_unlock(&cache_lock);
            return entry;
        }
        if (!strcasecmp(dll, "opengl32.dll")) {
            gptr opengl_resolve(const char *name);
            return opengl_resolve(name);
        }
    }
    return 0;
}

gptr w32_make_entry(const char *name, int nargs, w32_fn fn)
{
    return w32_entry(&(struct w32_export){ name, nargs, fn, 0 }, "com");
}
