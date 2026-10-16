/* USER32 / GDI32 / WINMM on SDL. All game windows share one SDL window. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "w32.h"

#define A(i) (a[i])
#define S(i) G2H(const char *, a[i])

bool input_init(const char *config_path);

void *g_window;                 /* SDL_Window* */
static int desktop_w = 1920, desktop_h = 1080;

/* ======================================================================== windows */

struct wndclass { char name[64]; gptr wndproc; };
static struct wndclass classes[16];
static int nclasses;

struct window {
    uint32_t parent;
    gptr wndproc;
    uint32_t style, exstyle, userdata, dc;
    int x, y, w, h;
    bool destroyed;
};

#define DESKTOP_HWND 0x10010u

struct dc;
uint32_t dc_for_window(uint32_t hwnd);       /* gdi part below */

static struct window *get_window(uint32_t h)
{
    struct object *o = handle_get(h, OBJ_WINDOW);
    return o ? o->data : NULL;
}

/* Message queue (posted messages only; sent messages call the WndProc directly). */
struct msg { uint32_t hwnd, message, wparam, lparam; };
static struct msg queue[256];
static int qhead, qlen;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;

static void post(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    pthread_mutex_lock(&qlock);
    if (qlen < 256) queue[(qhead + qlen++) % 256] = (struct msg){ hwnd, message, wparam, lparam };
    pthread_mutex_unlock(&qlock);
}

static uint32_t send(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    struct window *w = get_window(hwnd);
    if (!w || !w->wndproc) return 0;
    uint32_t args[4] = { hwnd, message, wparam, lparam };
    return guest_call(w->wndproc, 4, args);
}

bool video_init(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    if (SDL_GL_LoadLibrary(NULL) != 0) {
        fprintf(stderr, "SDL_GL_LoadLibrary: %s\n", SDL_GetError());
        return false;
    }
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { desktop_w = dm.w; desktop_h = dm.h; }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    /* Bindings: $TGM3_BINDINGS, else bindings.conf beside the build directory if present. */
    const char *bindings = getenv("TGM3_BINDINGS");
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 32);
    if (!bindings && n > 0) {
        path[n] = 0;
        char *slash = strrchr(path, '/');
        if (slash) {
            strcpy(slash, "/../bindings.conf");
            if (access(path, R_OK) == 0) bindings = path;
        }
    }
    if (bindings) LOG("bindings: %s", bindings);
    return input_init(bindings);
}

static void ensure_sdl_window(const char *title, int w, int h)
{
    if (g_window) return;
    int scale = getenv("TGM3_SCALE") ? atoi(getenv("TGM3_SCALE")) : 2;
    if (scale < 1) scale = 1;
    while (scale > 1 && (w * scale > desktop_w || h * scale > desktop_h)) scale--;
    g_window = SDL_CreateWindow(title && *title ? title : "TGM3", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                w * scale, h * scale, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!g_window) w32_fatal("SDL_CreateWindow: %s", SDL_GetError());
    if (getenv("TGM3_FULLSCREEN") && atoi(getenv("TGM3_FULLSCREEN")))
        SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
}

static uint32_t u_RegisterClassA(uint32_t *a)
{
    gptr wc = A(0);
    if (nclasses == 16) return 0;
    struct wndclass *c = &classes[nclasses++];
    c->wndproc = rd32(wc + 4);
    gptr name = rd32(wc + 36);
    snprintf(c->name, sizeof c->name, "%s", name < 0x10000 ? "#atom" : G2H(const char *, name));
    LOG("RegisterClassA(%s, wndproc=%08x)", c->name, c->wndproc);
    return 0xC000u + (uint32_t)nclasses;
}

static uint32_t u_CreateWindowExA(uint32_t *a)
{
    gptr cls = A(1);
    struct wndclass *c = NULL;
    for (int i = 0; i < nclasses; i++)
        if (cls < 0x10000 ? cls == 0xC001u + (uint32_t)i : !strcasecmp(classes[i].name, G2H(const char *, cls)))
            c = &classes[i];
    if (!c) { set_last_error(1407); return 0; }

    struct window *w = calloc(1, sizeof *w);
    w->wndproc = c->wndproc;
    w->parent = A(8);
    w->exstyle = A(0);
    w->style = A(3);
    w->x = (int32_t)A(4) == (int32_t)0x80000000 ? 0 : (int32_t)A(4);
    w->y = (int32_t)A(5) == (int32_t)0x80000000 ? 0 : (int32_t)A(5);
    w->w = (int32_t)A(6) == (int32_t)0x80000000 ? 640 : (int32_t)A(6);
    w->h = (int32_t)A(7) == (int32_t)0x80000000 ? 480 : (int32_t)A(7);
    uint32_t hwnd = handle_new(OBJ_WINDOW, w, NULL);
    const char *title = A(2) ? S(2) : "";
    LOG("CreateWindowExA(%s, \"%s\", style=%08x, %d,%d %dx%d) -> %x", c->name, title, w->style, w->x, w->y, w->w, w->h, hwnd);
    ensure_sdl_window(title, w->w, w->h);

    /* CREATESTRUCTA, then WM_NCCREATE / WM_CREATE like the real CreateWindowEx. */
    uint32_t fields[12] = { A(11), A(10), A(9), A(8), (uint32_t)w->h, (uint32_t)w->w, (uint32_t)w->y, (uint32_t)w->x,
                            w->style, A(2), A(1), A(0) };
    gptr cs = guest_alloc(sizeof fields);
    memcpy(G2H(void *, cs), fields, sizeof fields);
    bool ok = send(hwnd, 0x0081, 0, cs) && send(hwnd, 0x0001, 0, cs) != 0xFFFFFFFF;
    guest_free(cs);
    if (!ok) { handle_close(hwnd); return 0; }
    if (w->style & 0x10000000) {                 /* WS_VISIBLE */
        SDL_ShowWindow(g_window);
        send(hwnd, 0x001C, 1, 0);                /* WM_ACTIVATEAPP */
    }
    return hwnd;
}

static uint32_t u_DefWindowProcA(uint32_t *a)
{
    switch (A(1)) {
    case 0x0081: return 1;                       /* WM_NCCREATE */
    case 0x0010: {                               /* WM_CLOSE */
        uint32_t args[1] = { A(0) };
        extern uint32_t u_DestroyWindow(uint32_t *a);
        u_DestroyWindow(args);
        return 0;
    }
    }
    return 0;
}

uint32_t u_DestroyWindow(uint32_t *a)
{
    struct window *w = get_window(A(0));
    if (!w || w->destroyed) return W32_FALSE;
    w->destroyed = true;
    send(A(0), 0x0002, 0, 0);                    /* WM_DESTROY */
    LOG("DestroyWindow(%x)", A(0));
    return W32_TRUE;
}

static uint32_t u_ShowWindow(uint32_t *a)
{
    struct window *w = get_window(A(0));
    if (!w) return 0;
    bool was = (w->style & 0x10000000) != 0;
    if (A(1)) {
        w->style |= 0x10000000;
        SDL_ShowWindow(g_window);
        send(A(0), 0x0005, 0, (uint32_t)w->h << 16 | (uint32_t)w->w);    /* WM_SIZE */
        send(A(0), 0x001C, 1, 0);                                       /* WM_ACTIVATEAPP */
        send(A(0), 0x0006, 1, 0);                                       /* WM_ACTIVATE */
    } else {
        w->style &= ~0x10000000u;
    }
    return was;
}

static uint32_t u_PeekMessageA(uint32_t *a)
{
    input_pump();
    pthread_mutex_lock(&qlock);
    if (!qlen) { pthread_mutex_unlock(&qlock); return W32_FALSE; }
    struct msg m = queue[qhead];
    if (A(4) & 1) { qhead = (qhead + 1) % 256; qlen--; }
    pthread_mutex_unlock(&qlock);
    gptr p = A(0);
    wr32(p, m.hwnd);
    wr32(p + 4, m.message);
    wr32(p + 8, m.wparam);
    wr32(p + 12, m.lparam);
    wr32(p + 16, SDL_GetTicks());
    wr32(p + 20, 0);
    wr32(p + 24, 0);
    return W32_TRUE;
}

static uint32_t u_DispatchMessageA(uint32_t *a)
{
    gptr m = A(0);
    return send(rd32(m), rd32(m + 4), rd32(m + 8), rd32(m + 12));
}

static uint32_t u_TranslateMessage(uint32_t *a) { (void)a; return W32_FALSE; }
static uint32_t u_PostMessageA(uint32_t *a) { post(A(0), A(1), A(2), A(3)); return W32_TRUE; }
static uint32_t u_PostQuitMessage(uint32_t *a) { post(0, 0x0012, A(0), 0); return 0; }

static uint32_t u_GetClientRect(uint32_t *a)
{
    struct window *w = get_window(A(0));
    int cw = w ? w->w : desktop_w, ch = w ? w->h : desktop_h;
    wr32(A(1), 0);
    wr32(A(1) + 4, 0);
    wr32(A(1) + 8, (uint32_t)cw);
    wr32(A(1) + 12, (uint32_t)ch);
    return W32_TRUE;
}

static uint32_t u_GetWindowRect(uint32_t *a)
{
    struct window *w = get_window(A(0));
    int x = w ? w->x : 0, y = w ? w->y : 0, cw = w ? w->w : desktop_w, ch = w ? w->h : desktop_h;
    wr32(A(1), (uint32_t)x);
    wr32(A(1) + 4, (uint32_t)y);
    wr32(A(1) + 8, (uint32_t)(x + cw));
    wr32(A(1) + 12, (uint32_t)(y + ch));
    return W32_TRUE;
}

static void move(uint32_t hwnd, int x, int y, int cw, int ch, bool size)
{
    struct window *w = get_window(hwnd);
    if (!w) return;
    w->x = x;
    w->y = y;
    if (size && (cw != w->w || ch != w->h)) {
        LOG("window %x resized to %dx%d", hwnd, cw, ch);
        w->w = cw;
        w->h = ch;
        send(hwnd, 0x0005, 0, (uint32_t)ch << 16 | (uint32_t)cw);
    }
}

static uint32_t u_SetWindowPos(uint32_t *a)
{
    move(A(0), (int32_t)A(2), (int32_t)A(3), (int32_t)A(4), (int32_t)A(5), !(A(6) & 1));   /* SWP_NOSIZE */
    return W32_TRUE;
}

static uint32_t u_MoveWindow(uint32_t *a)
{
    move(A(0), (int32_t)A(1), (int32_t)A(2), (int32_t)A(3), (int32_t)A(4), true);
    return W32_TRUE;
}

static uint32_t u_GetWindowLongA(uint32_t *a)
{
    struct window *w = get_window(A(0));
    if (!w) return 0;
    switch ((int32_t)A(1)) {
    case -4: return w->wndproc;
    case -16: return w->style;
    case -20: return w->exstyle;
    case -21: return w->userdata;
    }
    return 0;
}

static uint32_t u_SetWindowLongA(uint32_t *a)
{
    struct window *w = get_window(A(0));
    if (!w) return 0;
    uint32_t *field = NULL;
    switch ((int32_t)A(1)) {
    case -4: field = &w->wndproc; break;
    case -16: field = &w->style; break;
    case -20: field = &w->exstyle; break;
    case -21: field = &w->userdata; break;
    default: return 0;
    }
    uint32_t prev = *field;
    *field = A(2);
    return prev;
}

static uint32_t u_SetWindowTextA(uint32_t *a)
{
    if (g_window && A(1)) SDL_SetWindowTitle(g_window, S(1));
    return W32_TRUE;
}

static int cursor_count;
static uint32_t u_ShowCursor(uint32_t *a)
{
    cursor_count += A(0) ? 1 : -1;
    SDL_ShowCursor(cursor_count >= 0 ? SDL_ENABLE : SDL_DISABLE);
    return (uint32_t)cursor_count;
}

static uint32_t u_MessageBoxA(uint32_t *a)
{
    const char *text = A(1) ? S(1) : "", *caption = A(2) ? S(2) : "TGM3";
    WARN("MessageBox: %s: %s", caption, text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, caption, text, g_window);
    return 1;                                     /* IDOK */
}

static uint32_t u_GetDC(uint32_t *a) { return dc_for_window(A(0) ? A(0) : DESKTOP_HWND); }

static uint32_t u_BeginPaint(uint32_t *a)
{
    memset(G2H(void *, A(1)), 0, 64);
    uint32_t dc = dc_for_window(A(0));
    wr32(A(1), dc);
    return dc;
}

static uint32_t u_EndPaint(uint32_t *a) { (void)a; return W32_TRUE; }
static uint32_t u_InvalidateRect(uint32_t *a) { (void)a; return W32_TRUE; }
static uint32_t u_IsIconic(uint32_t *a) { (void)a; return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED); }
static uint32_t u_GetDesktopWindow(uint32_t *a) { (void)a; return DESKTOP_HWND; }
static uint32_t u_LoadCursorA(uint32_t *a) { (void)a; return 0x10003; }
static uint32_t u_LoadIconA(uint32_t *a) { (void)a; return 0x10005; }

/* DEVMODEA: dmSize@36 dmFields@40 dmBitsPerPel@104 dmPelsWidth@108 dmPelsHeight@112 dmDisplayFrequency@120 */
static uint32_t u_EnumDisplaySettingsA(uint32_t *a)
{
    static const int modes[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 } };
    int w, h;
    if (A(1) >= 0xFFFFFFFE) { w = desktop_w; h = desktop_h; }
    else if (A(1) < 3) { w = modes[A(1)][0]; h = modes[A(1)][1]; }
    else if (A(1) == 3) { w = desktop_w; h = desktop_h; }
    else return W32_FALSE;
    gptr dm = A(2);
    wr32(dm + 40, 0x5C0000);                     /* BITSPERPEL | PELSWIDTH | PELSHEIGHT | DISPLAYFREQUENCY */
    wr32(dm + 104, 32);
    wr32(dm + 108, (uint32_t)w);
    wr32(dm + 112, (uint32_t)h);
    wr32(dm + 116, 0);
    wr32(dm + 120, 60);
    return W32_TRUE;
}

static uint32_t u_ChangeDisplaySettingsA(uint32_t *a)
{
    if (A(0)) LOG("ChangeDisplaySettingsA(%ux%u@%u, flags %x) - ignored, window is scaled instead",
                  rd32(A(0) + 108), rd32(A(0) + 112), rd32(A(0) + 104), A(1));
    return 0;                                    /* DISP_CHANGE_SUCCESSFUL */
}

static uint32_t g_FillRect(uint32_t *a);     /* user32 export, implemented with the GDI code */

const struct w32_export user32_exports[] = {
    { "RegisterClassA", 1, u_RegisterClassA, 0 },
    { "CreateWindowExA", 12, u_CreateWindowExA, 0 },
    { "DefWindowProcA", 4, u_DefWindowProcA, 0 },
    { "DestroyWindow", 1, u_DestroyWindow, 0 },
    { "ShowWindow", 2, u_ShowWindow, 0 },
    { "PeekMessageA", 5, u_PeekMessageA, 0 },
    { "DispatchMessageA", 1, u_DispatchMessageA, 0 },
    { "TranslateMessage", 1, u_TranslateMessage, 0 },
    { "PostMessageA", 4, u_PostMessageA, 0 },
    { "PostQuitMessage", 1, u_PostQuitMessage, 0 },
    { "GetClientRect", 2, u_GetClientRect, 0 },
    { "GetWindowRect", 2, u_GetWindowRect, 0 },
    { "SetWindowPos", 7, u_SetWindowPos, 0 },
    { "MoveWindow", 6, u_MoveWindow, 0 },
    { "GetWindowLongA", 2, u_GetWindowLongA, 0 },
    { "SetWindowLongA", 3, u_SetWindowLongA, 0 },
    { "SetWindowTextA", 2, u_SetWindowTextA, 0 },
    { "ShowCursor", 1, u_ShowCursor, 0 },
    { "MessageBoxA", 4, u_MessageBoxA, 0 },
    { "GetDC", 1, u_GetDC, 0 },
    { "BeginPaint", 2, u_BeginPaint, 0 },
    { "EndPaint", 2, u_EndPaint, 0 },
    { "InvalidateRect", 3, u_InvalidateRect, 0 },
    { "IsIconic", 1, u_IsIconic, 0 },
    { "GetDesktopWindow", 0, u_GetDesktopWindow, 0 },
    { "LoadCursorA", 2, u_LoadCursorA, 0 },
    { "LoadIconA", 2, u_LoadIconA, 0 },
    { "EnumDisplaySettingsA", 3, u_EnumDisplaySettingsA, 0 },
    { "ChangeDisplaySettingsA", 2, u_ChangeDisplaySettingsA, 0 },
    { "FillRect", 3, g_FillRect, 0 },
    { NULL, 0, NULL, 0 },
};

/* ======================================================================== GDI */

struct dc {
    uint32_t hwnd;          /* window DC, or 0 */
    uint32_t pbuffer;       /* pbuffer DC, or 0 */
    uint32_t bitmap, font, brush;
    uint32_t text_color, bk_color;
};

struct bitmap { uint8_t *bits; int w, h, bpp, stride; bool top_down; };

uint32_t dc_for_window(uint32_t hwnd)
{
    struct window *w = get_window(hwnd);
    static uint32_t desktop_dc;
    uint32_t *slot = w ? &w->dc : &desktop_dc;
    if (!*slot) {
        struct dc *d = calloc(1, sizeof *d);
        d->hwnd = hwnd;
        d->text_color = 0xFFFFFF;
        *slot = handle_new(OBJ_DC, d, NULL);
    }
    return *slot;
}

uint32_t dc_new_pbuffer(uint32_t pbuffer);
uint32_t dc_new_pbuffer(uint32_t pbuffer)
{
    struct dc *d = calloc(1, sizeof *d);
    d->pbuffer = pbuffer;
    return handle_new(OBJ_DC, d, NULL);
}

uint32_t dc_window(uint32_t hdc, uint32_t *pbuffer);
uint32_t dc_window(uint32_t hdc, uint32_t *pbuffer)
{
    struct object *o = handle_get(hdc, OBJ_DC);
    if (!o) return 0;
    struct dc *d = o->data;
    if (pbuffer) *pbuffer = d->pbuffer;
    return d->hwnd;
}

int window_client_size(uint32_t hwnd, int *w, int *h);
void window_canvas(uint32_t hwnd, int *cw, int *ch, int *ox, int *oy);
int window_client_size(uint32_t hwnd, int *cw, int *ch)
{
    struct window *w = get_window(hwnd);
    if (!w) return 0;
    *cw = w->w;
    *ch = w->h;
    return 1;
}

/* What the game would have shown on screen: a child window inside its top-level parent. */
void window_canvas(uint32_t hwnd, int *cw, int *ch, int *ox, int *oy)
{
    struct window *w = get_window(hwnd), *p = w ? get_window(w->parent) : NULL;
    *ox = *oy = 0;
    if (!w) return;
    *cw = w->w;
    *ch = w->h;
    if (p && p->w >= w->w && p->h >= w->h) {
        *cw = p->w;
        *ch = p->h;
        *ox = w->x;
        *oy = w->y;
    }
}

static struct dc *get_dc(uint32_t h) { struct object *o = handle_get(h, OBJ_DC); return o ? o->data : NULL; }

static uint32_t g_CreateCompatibleDC(uint32_t *a)
{
    (void)a;
    struct dc *d = calloc(1, sizeof *d);
    d->text_color = 0;
    d->bk_color = 0xFFFFFF;
    return handle_new(OBJ_DC, d, NULL);
}

static uint32_t g_CreateDIBSection(uint32_t *a)
{
    gptr bmi = A(1);
    int32_t w = (int32_t)rd32(bmi + 4), h = (int32_t)rd32(bmi + 8);
    int bpp = rd16(bmi + 14);
    struct bitmap *b = calloc(1, sizeof *b);
    b->w = w;
    b->h = h < 0 ? -h : h;
    b->top_down = h < 0;
    b->bpp = bpp;
    b->stride = ((w * bpp + 31) / 32) * 4;
    b->bits = G2H(uint8_t *, guest_alloc((size_t)b->stride * (size_t)b->h));
    wr32(A(3), H2G(b->bits));
    LOG("CreateDIBSection(%dx%d, %d bpp)", w, h, bpp);
    return handle_new(OBJ_BITMAP, b, NULL);
}

static uint32_t g_CreateFontIndirectA(uint32_t *a)
{
    int32_t height = (int32_t)rd32(A(0));
    LOG("CreateFontIndirectA(height %d, face \"%s\")", height, G2H(const char *, A(0) + 28));
    return handle_new(OBJ_FONT, (void *)(intptr_t)(height < 0 ? -height : height ? height : 16), NULL);
}

static uint32_t g_GetStockObject(uint32_t *a) { return 0x20000u + A(0); }

static uint32_t g_SelectObject(uint32_t *a)
{
    struct dc *d = get_dc(A(0));
    if (!d) return 0;
    uint32_t prev = 0;
    if (handle_get(A(1), OBJ_BITMAP)) { prev = d->bitmap; d->bitmap = A(1); }
    else if (handle_get(A(1), OBJ_FONT)) { prev = d->font; d->font = A(1); }
    else { prev = d->brush; d->brush = A(1); }
    return prev ? prev : 0x20000u + 13;
}

static uint32_t g_SetTextColor(uint32_t *a) { struct dc *d = get_dc(A(0)); if (!d) return 0; uint32_t p = d->text_color; d->text_color = A(1); return p; }
static uint32_t g_SetBkColor(uint32_t *a) { struct dc *d = get_dc(A(0)); if (!d) return 0; uint32_t p = d->bk_color; d->bk_color = A(1); return p; }

static int font_height(struct dc *d)
{
    struct object *o = d ? handle_get(d->font, OBJ_FONT) : NULL;
    return o ? (int)(intptr_t)o->data : 16;
}

static uint32_t g_GetTextExtentPoint32A(uint32_t *a)
{
    int h = font_height(get_dc(A(0)));
    wr32(A(3), (uint32_t)((int32_t)A(2) * (h / 2)));
    wr32(A(3) + 4, (uint32_t)h);
    return W32_TRUE;
}

static uint32_t g_TextOutA(uint32_t *a)
{
    static bool warned;
    if (!warned) {
        WARN("TextOutA(\"%.*s\") - GDI text rendering is not implemented", (int)A(4), G2H(const char *, A(3)));
        warned = true;
    }
    return W32_TRUE;
}

static uint32_t g_FillRect(uint32_t *a)
{
    struct dc *d = get_dc(A(0));
    struct object *o = d ? handle_get(d->bitmap, OBJ_BITMAP) : NULL;
    if (!o) return W32_TRUE;
    struct bitmap *b = o->data;
    if (b->bpp != 32) return W32_TRUE;
    uint32_t color = A(2) == 0x20000u + 4 ? 0 : A(2) == 0x20000u ? 0xFFFFFF : 0;     /* BLACK_BRUSH / WHITE_BRUSH */
    int32_t l = (int32_t)rd32(A(1)), t = (int32_t)rd32(A(1) + 4), r = (int32_t)rd32(A(1) + 8), btm = (int32_t)rd32(A(1) + 12);
    for (int32_t y = t < 0 ? 0 : t; y < btm && y < b->h; y++)
        for (int32_t x = l < 0 ? 0 : l; x < r && x < b->w; x++)
            memcpy(b->bits + (size_t)(b->top_down ? y : b->h - 1 - y) * (size_t)b->stride + (size_t)x * 4, &color, 4);
    return W32_TRUE;
}

static uint32_t g_ChoosePixelFormat(uint32_t *a) { (void)a; return 1; }
static uint32_t g_SetPixelFormat(uint32_t *a) { (void)a; return W32_TRUE; }

uint32_t gl_swap_buffers(uint32_t hdc);
static uint32_t g_SwapBuffers(uint32_t *a) { return gl_swap_buffers(A(0)); }

const struct w32_export gdi32_exports[] = {
    { "CreateCompatibleDC", 1, g_CreateCompatibleDC, 0 },
    { "CreateDIBSection", 6, g_CreateDIBSection, 0 },
    { "CreateFontIndirectA", 1, g_CreateFontIndirectA, 0 },
    { "GetStockObject", 1, g_GetStockObject, 0 },
    { "SelectObject", 2, g_SelectObject, 0 },
    { "SetTextColor", 2, g_SetTextColor, 0 },
    { "SetBkColor", 2, g_SetBkColor, 0 },
    { "GetTextExtentPoint32A", 4, g_GetTextExtentPoint32A, 0 },
    { "TextOutA", 5, g_TextOutA, 0 },
    { "ChoosePixelFormat", 2, g_ChoosePixelFormat, 0 },
    { "SetPixelFormat", 3, g_SetPixelFormat, 0 },
    { "SwapBuffers", 1, g_SwapBuffers, 0 },
    { NULL, 0, NULL, 0 },
};

/* ======================================================================== WINMM */

static uint32_t w_timeGetTime(uint32_t *a)
{
    (void)a;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

const struct w32_export winmm_exports[] = {
    { "timeGetTime", 0, w_timeGetTime, 0 },
    { NULL, 0, NULL, 0 },
};
