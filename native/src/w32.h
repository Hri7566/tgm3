/*
 * Minimal Win32 environment for TGM3's game.exe.
 *
 * Every API is implemented as `uint32_t fn(uint32_t *args)`: args points at the guest's
 * stdcall arguments, pointers are guest addresses converted with G2H(). On the native
 * x86 loader guest == host, so G2H is a cast; a static recompilation can redefine it
 * (guest memory base + offset) and reuse these implementations unchanged.
 */
#ifndef W32_H
#define W32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t gptr;                                  /* guest pointer */
extern uint8_t *M;                                      /* guest address 0 on the host */
#ifdef GUEST_WINDOW
#define G2H(type, p)   ((type)(void *)(M + (uint32_t)(p)))
#define H2G(p)         ((gptr)((const uint8_t *)(p) - M))
#else
#define G2H(type, p)   ((type)(uintptr_t)(p))
#define H2G(p)         ((gptr)(uintptr_t)(p))
#endif

/* Little-endian guest memory accessors. */
static inline uint32_t rd32(gptr p) { uint32_t v; __builtin_memcpy(&v, G2H(void *, p), 4); return v; }
static inline uint16_t rd16(gptr p) { uint16_t v; __builtin_memcpy(&v, G2H(void *, p), 2); return v; }
static inline void wr32(gptr p, uint32_t v) { if (p) __builtin_memcpy(G2H(void *, p), &v, 4); }
static inline void wr16(gptr p, uint16_t v) { if (p) __builtin_memcpy(G2H(void *, p), &v, 2); }

typedef uint32_t (*w32_fn)(uint32_t *args);

struct w32_export {
    const char *name;
    int nargs;                 /* dwords popped by the stdcall callee */
    w32_fn fn;
    int ordinal;               /* 0 = by name only */
};

struct w32_dll {
    const char *name;
    const struct w32_export *exports;
};

extern const struct w32_export kernel32_exports[], user32_exports[], gdi32_exports[],
    winmm_exports[], opengl32_exports[], dsound_exports[];

/* Returns a guest-callable stdcall entry point for an export (creating its thunk). */
gptr w32_entry(const struct w32_export *e, const char *dll);
/* Guest-callable stdcall entry point for an arbitrary implementation (COM vtables etc.). */
gptr w32_make_entry(const char *name, int nargs, w32_fn fn);
/* Look up an export by DLL + name (or ordinal if name is NULL). */
gptr w32_resolve(const char *dll, const char *name, int ordinal);
/* Entry that aborts with a clear message if the game ever calls it. */
gptr w32_missing_stub(const char *dll, const char *name);

/* Guest memory (gmem.c). Everything handed to the game must live here. */
void gmem_init(void);
gptr gmem_reserve(gptr addr, uint32_t size);     /* 64 KiB aligned, inaccessible; 0 on failure */
void gmem_release(gptr base, uint32_t size);
void gmem_protect(gptr addr, uint32_t size, int prot);
void gmem_decommit(gptr addr, uint32_t size);
gptr guest_alloc(size_t size);                   /* zeroed */
void guest_free(gptr p);
uint32_t guest_size(gptr p);
gptr guest_realloc(gptr p, size_t size);
gptr guest_strdup(const char *s);

/* Call guest code: stdcall or cdecl, returns eax. */
uint32_t guest_call(gptr fn, int nargs, const uint32_t *args);

/* Per-thread environment (TEB on fs:, last error, TLS). */
#define TEB_SIZE       0x2000
#define TEB_EXCEPTION  0x00
#define TEB_STACKBASE  0x04
#define TEB_STACKLIMIT 0x08
#define TEB_SELF       0x18
#define TEB_PID        0x20
#define TEB_TID        0x24
#define TEB_PEB        0x30
#define TEB_LASTERROR  0x34
#define TEB_TLS        0xE10
extern __thread gptr g_teb;      /* this thread's TEB (guest address) */
void thread_setup(void);
uint32_t *teb_tls_slots(void);
void set_last_error(uint32_t e);
uint32_t get_last_error(void);
uint32_t teb_exception_list(void);
void teb_set_exception_list(uint32_t v);

/* Process-wide settings. */
extern char g_root[];            /* host directory that C:\ maps to (the game folder) */
extern bool g_trace;             /* log every API call */
extern int g_verbose;

void w32_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void w32_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
#define LOG(...)   do { if (g_verbose) w32_log(__VA_ARGS__); } while (0)
#define WARN(...)  w32_log(__VA_ARGS__)

/* Handle table shared by kernel32/user32/gdi32/opengl32. */
enum obj_type {
    OBJ_NONE, OBJ_FILE, OBJ_THREAD, OBJ_MUTEX, OBJ_COMM, OBJ_MODULE,
    OBJ_WINDOW, OBJ_DC, OBJ_BITMAP, OBJ_FONT, OBJ_BRUSH, OBJ_GLRC, OBJ_PBUFFER, OBJ_HEAP,
};
struct object {
    enum obj_type type;
    void *data;
    void (*destroy)(struct object *o);
};
uint32_t handle_new(enum obj_type type, void *data, void (*destroy)(struct object *));
struct object *handle_get(uint32_t h, enum obj_type type);   /* NULL if wrong type */
void handle_close(uint32_t h);

#define W32_TRUE  1u
#define W32_FALSE 0u

/* Windows error codes used here. */
#define ERROR_SUCCESS             0
#define ERROR_FILE_NOT_FOUND      2
#define ERROR_PATH_NOT_FOUND      3
#define ERROR_ACCESS_DENIED       5
#define ERROR_INVALID_HANDLE      6
#define ERROR_NOT_ENOUGH_MEMORY   8
#define ERROR_INVALID_PARAMETER   87
#define ERROR_FILE_EXISTS         80
#define ERROR_ALREADY_EXISTS      183
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define ERROR_NO_MORE_FILES       18

/* Host-side helpers shared between modules. */
const char *path_to_host(const char *win_path, char *out, size_t out_size, bool for_create);
void comm_init(void);
uint32_t comm_open(void);
void input_pump(void);            /* main thread: SDL events -> JVS input snapshot */
bool video_init(void);
extern void *g_window;            /* SDL_Window* */

#endif
