/*
 * OPENGL32 + WGL on SDL.
 *
 * GL entry points are the system's, behind stdcall->cdecl adapters. Two things differ from
 * a real window system:
 *  - the game renders into an FBO at its own resolution, which SwapBuffers scales into the
 *    (resizable / fullscreen) SDL window;
 *  - WGL_ARB_pbuffer (the game's "kgl offscreen texture") becomes a shared SDL context
 *    rendering into its own FBO, so state and objects behave like a real pbuffer context.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL.h>
#include <SDL_opengl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "w32.h"
#include "glwrap.h"

#define A(i) (a[i])
#define S(i) G2H(const char *, a[i])

uint32_t dc_new_pbuffer(uint32_t pbuffer);
uint32_t dc_window(uint32_t hdc, uint32_t *pbuffer);
int window_client_size(uint32_t hwnd, int *w, int *h);
void window_canvas(uint32_t hwnd, int *cw, int *ch, int *ox, int *oy);

/* ======================================================================== GL entry points */

struct adapter { const char *name; gptr entry; };
static struct adapter adapters[2048];
static int nadapters;
static pthread_mutex_t adapter_lock = PTHREAD_MUTEX_INITIALIZER;

static int cmp_gl_wrap(const void *key, const void *elem)
{
    return strcmp(key, ((const struct gl_wrap *)elem)->name);
}

/* glGetString returns static strings; the guest gets copies in its own memory. */
static uint32_t gl_GetString(uint32_t *a)
{
    static const GLubyte *(APIENTRY *get)(GLenum);
    static struct { GLenum name; gptr copy; } cache[8];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!get) get = SDL_GL_GetProcAddress("glGetString");
    const char *s = (const char *)get(A(0));
    if (!s) return 0;
    pthread_mutex_lock(&lock);
    gptr out = 0;
    for (int i = 0; i < 8 && !out; i++) {
        if (cache[i].copy && cache[i].name == A(0) && !strcmp(G2H(const char *, cache[i].copy), s)) out = cache[i].copy;
        else if (!cache[i].copy) { cache[i].name = A(0); out = cache[i].copy = guest_strdup(s); }
    }
    pthread_mutex_unlock(&lock);
    return out ? out : guest_strdup(s);
}

/* stdcall entry for a core/extension GL function, or 0 if unknown/unavailable. */
static gptr gl_entry(const char *name)
{
    pthread_mutex_lock(&adapter_lock);
    for (int i = 0; i < nadapters; i++)
        if (!strcmp(adapters[i].name, name)) { pthread_mutex_unlock(&adapter_lock); return adapters[i].entry; }
    pthread_mutex_unlock(&adapter_lock);

    gptr entry;
    if (!strcmp(name, "glGetString")) {
        entry = w32_make_entry("glGetString", 1, gl_GetString);
    } else {
        const struct gl_wrap *w = bsearch(name, gl_wraps, sizeof gl_wraps / sizeof *gl_wraps, sizeof *gl_wraps, cmp_gl_wrap);
        void *fn = w ? SDL_GL_GetProcAddress(name) : NULL;
        if (!fn) return 0;
        gw_ptr[w->id] = fn;
        entry = w32_make_entry(w->name, w->nargs, w->fn);
    }
    pthread_mutex_lock(&adapter_lock);
    if (nadapters < (int)(sizeof adapters / sizeof *adapters))
        adapters[nadapters++] = (struct adapter){ strdup(name), entry };
    pthread_mutex_unlock(&adapter_lock);
    return entry;
}

/* ======================================================================== FBO helpers */

static PFNGLGENFRAMEBUFFERSPROC pglGenFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC pglBindFramebuffer;
static PFNGLGENRENDERBUFFERSPROC pglGenRenderbuffers;
static PFNGLBINDRENDERBUFFERPROC pglBindRenderbuffer;
static PFNGLRENDERBUFFERSTORAGEPROC pglRenderbufferStorage;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC pglFramebufferRenderbuffer;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC pglCheckFramebufferStatus;
static PFNGLBLITFRAMEBUFFERPROC pglBlitFramebuffer;

/* Core GL used by the presenter itself, also fetched through SDL (libGL isn't linked). */
#define CORE_GL(X) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClear, (GLbitfield)) \
    X(GLboolean, glIsEnabled, (GLenum)) \
    X(void, glGetBooleanv, (GLenum, GLboolean *)) \
    X(void, glGetFloatv, (GLenum, GLfloat *)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
#define DECLARE(ret, name, params) static ret (APIENTRY *p_##name) params;
CORE_GL(DECLARE)
#define glClearColor p_glClearColor
#define glClear p_glClear
#define glIsEnabled p_glIsEnabled
#define glGetBooleanv p_glGetBooleanv
#define glGetFloatv p_glGetFloatv
#define glEnable p_glEnable
#define glDisable p_glDisable
#define glColorMask p_glColorMask

static void load_fbo_procs(void)
{
    if (pglGenFramebuffers) return;
#define LOAD(ret, name, params) p_##name = SDL_GL_GetProcAddress(#name);
    CORE_GL(LOAD)
    pglGenFramebuffers = SDL_GL_GetProcAddress("glGenFramebuffers");
    pglBindFramebuffer = SDL_GL_GetProcAddress("glBindFramebuffer");
    pglGenRenderbuffers = SDL_GL_GetProcAddress("glGenRenderbuffers");
    pglBindRenderbuffer = SDL_GL_GetProcAddress("glBindRenderbuffer");
    pglRenderbufferStorage = SDL_GL_GetProcAddress("glRenderbufferStorage");
    pglFramebufferRenderbuffer = SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
    pglCheckFramebufferStatus = SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    pglBlitFramebuffer = SDL_GL_GetProcAddress("glBlitFramebuffer");
    if (!pglGenFramebuffers || !pglBlitFramebuffer) w32_fatal("OpenGL framebuffer objects are required");
}

struct glrc {
    SDL_GLContext ctx;
    GLuint fbo;
    int w, h;
    uint32_t hwnd;
    bool is_pbuffer;
};

struct pbuffer { int w, h; uint32_t dc; };

/* Create this context's render target (must run with the context current). */
static void make_fbo(struct glrc *rc, int w, int h)
{
    load_fbo_procs();
    GLuint rb[2];
    pglGenFramebuffers(1, &rc->fbo);
    pglGenRenderbuffers(2, rb);
    pglBindFramebuffer(GL_FRAMEBUFFER, rc->fbo);
    pglBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    pglRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    pglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    pglBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    pglRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    pglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    pglBindRenderbuffer(GL_RENDERBUFFER, 0);
    GLenum status = pglCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) w32_fatal("framebuffer incomplete (%#x)", status);
    rc->w = w;
    rc->h = h;
    /* Start from black like a freshly created window. */
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    LOG("%s render target %dx%d", rc->is_pbuffer ? "pbuffer" : "screen", w, h);
}

/* ======================================================================== WGL */

static struct glrc *main_rc;
static __thread uint32_t cur_rc_handle, cur_dc;

static struct glrc *get_rc(uint32_t h) { struct object *o = handle_get(h, OBJ_GLRC); return o ? o->data : NULL; }
static struct pbuffer *get_pb(uint32_t h) { struct object *o = handle_get(h, OBJ_PBUFFER); return o ? o->data : NULL; }

static uint32_t wgl_CreateContext(uint32_t *a)
{
    uint32_t pbuffer = 0;
    dc_window(A(0), &pbuffer);
    struct glrc *rc = calloc(1, sizeof *rc);
    rc->is_pbuffer = pbuffer != 0;

    SDL_GLContext prev = SDL_GL_GetCurrentContext();
    if (main_rc && prev != main_rc->ctx) SDL_GL_MakeCurrent(g_window, main_rc->ctx);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, main_rc ? 1 : 0);
    rc->ctx = SDL_GL_CreateContext(g_window);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (!rc->ctx) { WARN("SDL_GL_CreateContext: %s", SDL_GetError()); free(rc); return 0; }
    SDL_GL_MakeCurrent(g_window, prev);
    if (!main_rc && !rc->is_pbuffer) main_rc = rc;

    uint32_t h = handle_new(OBJ_GLRC, rc, NULL);
    LOG("wglCreateContext(dc %x%s) -> %x", A(0), rc->is_pbuffer ? ", pbuffer" : "", h);
    return h;
}

static uint32_t make_current(uint32_t hdc, uint32_t hrc)
{
    if (!hrc) {
        SDL_GL_MakeCurrent(g_window, NULL);
        cur_rc_handle = cur_dc = 0;
        return W32_TRUE;
    }
    struct glrc *rc = get_rc(hrc);
    if (!rc) { set_last_error(ERROR_INVALID_HANDLE); return W32_FALSE; }
    if (SDL_GL_MakeCurrent(g_window, rc->ctx) != 0) {
        WARN("SDL_GL_MakeCurrent: %s", SDL_GetError());
        return W32_FALSE;
    }
    cur_rc_handle = hrc;
    cur_dc = hdc;
    if (!rc->fbo) {
        uint32_t pbuffer = 0, hwnd = dc_window(hdc, &pbuffer);
        int w = 640, h = 480;
        struct pbuffer *pb = get_pb(pbuffer);
        if (pb) { w = pb->w; h = pb->h; }
        else window_client_size(hwnd, &w, &h);
        rc->hwnd = hwnd;
        make_fbo(rc, w, h);
        if (rc == main_rc) {
            const char *vsync = getenv("TGM3_VSYNC");
            SDL_GL_SetSwapInterval(vsync ? atoi(vsync) : 0);
        }
    }
    return W32_TRUE;
}

static uint32_t wgl_MakeCurrent(uint32_t *a) { return make_current(A(0), A(1)); }
static uint32_t wgl_MakeContextCurrentARB(uint32_t *a) { return make_current(A(0), A(2)); }

static uint32_t wgl_DeleteContext(uint32_t *a)
{
    struct glrc *rc = get_rc(A(0));
    if (!rc) return W32_FALSE;
    if (A(0) == cur_rc_handle) { SDL_GL_MakeCurrent(g_window, NULL); cur_rc_handle = cur_dc = 0; }
    if (rc == main_rc) main_rc = NULL;
    SDL_GL_DeleteContext(rc->ctx);
    handle_close(A(0));
    free(rc);
    return W32_TRUE;
}

static uint32_t wgl_GetCurrentContext(uint32_t *a) { (void)a; return cur_rc_handle; }
static uint32_t wgl_GetCurrentDC(uint32_t *a) { (void)a; return cur_dc; }
static uint32_t wgl_ShareLists(uint32_t *a) { (void)a; return W32_TRUE; }   /* contexts are created shared */

static const char wgl_extensions[] =
    "WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_ARB_pixel_format WGL_ARB_pbuffer "
    "WGL_ARB_make_current_read WGL_EXT_swap_control";

static uint32_t wgl_GetExtensionsString(uint32_t *a)
{
    (void)a;
    static gptr copy;
    if (!copy) copy = guest_strdup(wgl_extensions);
    return copy;
}

static uint32_t wgl_ChoosePixelFormatARB(uint32_t *a)
{
    if (A(3) && A(4)) wr32(A(4), 1);
    wr32(A(5), 1);
    return W32_TRUE;
}

static uint32_t pixel_format_attrib(uint32_t attr)
{
    switch (attr) {
    case 0x2001: case 0x202D: case 0x2010: case 0x2011: return 1;   /* window/pbuffer/opengl/double */
    case 0x2003: return 0x2027;                                      /* full acceleration */
    case 0x2013: return 0x202B;                                      /* RGBA */
    case 0x2014: return 32;
    case 0x2015: case 0x2017: case 0x2019: case 0x201B: return 8;
    case 0x2022: return 24;
    case 0x2023: return 8;
    case 0x202E: case 0x202F: return 4096;                           /* max pbuffer size */
    case 0x2030: return 4096 * 4096;
    }
    return 0;
}

static uint32_t wgl_GetPixelFormatAttribivARB(uint32_t *a)
{
    for (uint32_t i = 0; i < A(3); i++) wr32(A(5) + 4 * i, pixel_format_attrib(rd32(A(4) + 4 * i)));
    return W32_TRUE;
}

static uint32_t wgl_GetPixelFormatAttribfvARB(uint32_t *a)
{
    for (uint32_t i = 0; i < A(3); i++) {
        float f = (float)pixel_format_attrib(rd32(A(4) + 4 * i));
        uint32_t bits;
        memcpy(&bits, &f, 4);
        wr32(A(5) + 4 * i, bits);
    }
    return W32_TRUE;
}

static uint32_t wgl_CreatePbufferARB(uint32_t *a)
{
    struct pbuffer *pb = calloc(1, sizeof *pb);
    pb->w = (int)A(2);
    pb->h = (int)A(3);
    uint32_t h = handle_new(OBJ_PBUFFER, pb, NULL);
    LOG("wglCreatePbufferARB(%dx%d) -> %x", pb->w, pb->h, h);
    return h;
}

static uint32_t wgl_GetPbufferDCARB(uint32_t *a)
{
    struct pbuffer *pb = get_pb(A(0));
    if (!pb) return 0;
    if (!pb->dc) pb->dc = dc_new_pbuffer(A(0));
    return pb->dc;
}

static uint32_t wgl_ReleasePbufferDCARB(uint32_t *a) { (void)a; return 1; }
static uint32_t wgl_DestroyPbufferARB(uint32_t *a) { handle_close(A(0)); return W32_TRUE; }

static uint32_t wgl_QueryPbufferARB(uint32_t *a)
{
    struct pbuffer *pb = get_pb(A(0));
    if (!pb) return W32_FALSE;
    wr32(A(2), A(1) == 0x2034 ? (uint32_t)pb->w : A(1) == 0x2035 ? (uint32_t)pb->h : 0);
    return W32_TRUE;
}

static uint32_t wgl_SwapIntervalEXT(uint32_t *a) { (void)a; return W32_TRUE; }
static uint32_t wgl_GetSwapIntervalEXT(uint32_t *a) { (void)a; return 1; }
static uint32_t wgl_GetCurrentReadDCARB(uint32_t *a) { (void)a; return cur_dc; }

static const struct w32_export wgl_ext_exports[] = {
    { "wglGetExtensionsStringARB", 1, wgl_GetExtensionsString, 0 },
    { "wglGetExtensionsStringEXT", 0, wgl_GetExtensionsString, 0 },
    { "wglChoosePixelFormatARB", 6, wgl_ChoosePixelFormatARB, 0 },
    { "wglGetPixelFormatAttribivARB", 6, wgl_GetPixelFormatAttribivARB, 0 },
    { "wglGetPixelFormatAttribfvARB", 6, wgl_GetPixelFormatAttribfvARB, 0 },
    { "wglCreatePbufferARB", 5, wgl_CreatePbufferARB, 0 },
    { "wglGetPbufferDCARB", 1, wgl_GetPbufferDCARB, 0 },
    { "wglReleasePbufferDCARB", 2, wgl_ReleasePbufferDCARB, 0 },
    { "wglDestroyPbufferARB", 1, wgl_DestroyPbufferARB, 0 },
    { "wglQueryPbufferARB", 3, wgl_QueryPbufferARB, 0 },
    { "wglSwapIntervalEXT", 1, wgl_SwapIntervalEXT, 0 },
    { "wglGetSwapIntervalEXT", 0, wgl_GetSwapIntervalEXT, 0 },
    { "wglMakeContextCurrentARB", 3, wgl_MakeContextCurrentARB, 0 },
    { "wglGetCurrentReadDCARB", 0, wgl_GetCurrentReadDCARB, 0 },
    { NULL, 0, NULL, 0 },
};

static uint32_t wgl_GetProcAddress(uint32_t *a)
{
    const char *name = S(0);
    gptr fn = 0;
    if (!strncmp(name, "wgl", 3)) {
        fn = w32_resolve("opengl32.dll", name, 0);
        for (const struct w32_export *e = wgl_ext_exports; !fn && e->name; e++)
            if (!strcmp(e->name, name)) fn = w32_entry(e, "opengl32.dll");
    } else {
        fn = gl_entry(name);
    }
    if (g_verbose > 1) LOG("wglGetProcAddress(%s) -> %08x", name, fn);
    return fn;
}

/* ======================================================================== present */

static uint64_t next_frame_ns;

static void frame_limit(void)
{
    const char *fps_env = getenv("TGM3_FPS");
    double fps = fps_env ? atof(fps_env) : 60.0;
    if (fps <= 0) return;
    uint64_t period = (uint64_t)(1e9 / fps);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (!next_frame_ns || now > next_frame_ns + 100000000ull) next_frame_ns = now;
    next_frame_ns += period;
    if (next_frame_ns > now) {
        ts.tv_sec = (time_t)(next_frame_ns / 1000000000ull);
        ts.tv_nsec = (long)(next_frame_ns % 1000000000ull);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    }
}

/* Save the game's frame (not the scaled window) as a BMP. */
static void snapshot(struct glrc *rc, const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, rc->w, rc->h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!s) return;
    pglBindFramebuffer(GL_READ_FRAMEBUFFER, rc->fbo);
    p_glReadPixels(0, 0, rc->w, rc->h, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
    /* GL rows are bottom-up. */
    uint8_t *row = malloc((size_t)s->pitch);
    for (int y = 0; y < rc->h / 2; y++) {
        uint8_t *top = (uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch;
        uint8_t *bot = (uint8_t *)s->pixels + (size_t)(rc->h - 1 - y) * (size_t)s->pitch;
        memcpy(row, top, (size_t)s->pitch);
        memcpy(top, bot, (size_t)s->pitch);
        memcpy(bot, row, (size_t)s->pitch);
    }
    free(row);
    if (SDL_SaveBMP(s, path) == 0) WARN("saved %s", path);
    SDL_FreeSurface(s);
}

static uint64_t frame_count;
uint64_t gl_frame_count(void) { return frame_count; }
bool g_snapshot_requested;

static void maybe_snapshot(struct glrc *rc)
{
    frame_count++;
    if (g_verbose && frame_count % 600 == 0) {
        static uint64_t last_ns;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        if (last_ns) LOG("frame %llu: %.3f fps", (unsigned long long)frame_count, 600e9 / (double)(now - last_ns));
        last_ns = now;
    }
    /* TGM3_MEMDUMP=frame[,frame...]: write game.exe's .data/.bss at those frames, for diffing
       the game state of two builds. */
    for (const char *p = getenv("TGM3_MEMDUMP"); p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        if (strtoull(p, NULL, 10) != frame_count) continue;
        char path[64];
        snprintf(path, sizeof path, "tgm3-mem%06llu.bin", (unsigned long long)frame_count);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(G2H(void *, 0x474000u), 1, 0x653000u - 0x474000u, f); fclose(f); }
    }
    const char *list = getenv("TGM3_SNAPSHOT");
    bool want = g_snapshot_requested;
    g_snapshot_requested = false;
    for (const char *p = list; p && *p && !want; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL)
        if (strtoull(p, NULL, 10) == frame_count) want = true;
    if (!want) return;
    char path[64];
    snprintf(path, sizeof path, "tgm3-frame%06llu.bmp", (unsigned long long)frame_count);
    snapshot(rc, path);
}

uint32_t gl_swap_buffers(uint32_t hdc)
{
    (void)hdc;
    struct glrc *rc = get_rc(cur_rc_handle);
    if (!rc || rc->is_pbuffer || !rc->fbo) return W32_TRUE;
    maybe_snapshot(rc);

    int ww, wh, cw = rc->w, ch = rc->h, ox = 0, oy = 0;
    SDL_GL_GetDrawableSize(g_window, &ww, &wh);
    window_canvas(rc->hwnd, &cw, &ch, &ox, &oy);
    /* Fit the game's canvas (e.g. its 640x448 child centred in 640x480) into the window,
       keeping the aspect ratio; integer scales stay sharp. Canvas y grows downward. */
    double sx = (double)ww / cw, sy = (double)wh / ch, s = sx < sy ? sx : sy;
    int cx0 = (ww - (int)(cw * s)) / 2, cy0 = (wh - (int)(ch * s)) / 2;
    int dx = cx0 + (int)(ox * s), dw = (int)(rc->w * s), dh = (int)(rc->h * s);
    int dy = wh - (cy0 + (int)(oy * s)) - dh;
    bool integer = s == (int)s;

    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST), mask[4];
    GLfloat clear[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);

    pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    pglBindFramebuffer(GL_READ_FRAMEBUFFER, rc->fbo);
    pglBlitFramebuffer(0, 0, rc->w, rc->h, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, integer ? GL_NEAREST : GL_LINEAR);
    SDL_GL_SwapWindow(g_window);
    pglBindFramebuffer(GL_FRAMEBUFFER, rc->fbo);

    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    frame_limit();
    return W32_TRUE;
}

/* ======================================================================== export table */

const struct w32_export opengl32_exports[] = {
    { "wglCreateContext", 1, wgl_CreateContext, 0 },
    { "wglMakeCurrent", 2, wgl_MakeCurrent, 0 },
    { "wglDeleteContext", 1, wgl_DeleteContext, 0 },
    { "wglGetCurrentContext", 0, wgl_GetCurrentContext, 0 },
    { "wglGetCurrentDC", 0, wgl_GetCurrentDC, 0 },
    { "wglShareLists", 2, wgl_ShareLists, 0 },
    { "wglGetProcAddress", 1, wgl_GetProcAddress, 0 },
    { NULL, 0, NULL, 0 },
};

/* Called by w32_resolve for opengl32 imports that aren't wgl functions. */
gptr opengl_resolve(const char *name);
gptr opengl_resolve(const char *name) { return name ? gl_entry(name) : 0; }
