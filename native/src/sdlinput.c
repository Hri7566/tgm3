/* SDL keyboard + game controller input -> JVS switch state. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jvs.h"
#include "w32.h"

uint64_t gl_frame_count(void);

#define MAX_BINDINGS 8
#define MAX_PADS     JVS_PLAYERS

enum action {
    A_TEST, A_QUIT, A_FULLSCREEN,
    A_START, A_SERVICE, A_COIN, A_UP, A_DOWN, A_LEFT, A_RIGHT,
    A_B1, A_B2, A_B3, A_B4, A_B5, A_B6,
    PLAYER_ACTIONS = A_B6 - A_START + 1,
    ACTION_COUNT = A_START + JVS_PLAYERS * PLAYER_ACTIONS,
};

static const char *const player_action_names[PLAYER_ACTIONS] = {
    "start", "service", "coin", "up", "down", "left", "right", "b1", "b2", "b3", "b4", "b5", "b6",
};

enum kind { KEY, PAD_BUTTON, PAD_AXIS };
struct binding { enum kind kind; int code; int dir; };

/* pad:* bindings on p2.* read the second controller, and so on. */
static const char default_config[] =
    "test = 9\n"
    "quit = Escape\n"
    "fullscreen = F11\n"
    "p1.start = 1, pad:start\n"
    "p1.coin = 5, pad:back\n"
    "p1.service = 6\n"
    "p1.up = Up, pad:dpup, pad:lefty-\n"
    "p1.down = Down, pad:dpdown, pad:lefty+\n"
    "p1.left = Left, pad:dpleft, pad:leftx-\n"
    "p1.right = Right, pad:dpright, pad:leftx+\n"
    "p1.b1 = Z, pad:a\n"
    "p1.b2 = X, pad:b\n"
    "p1.b3 = C, pad:x\n"
    "p1.b4 = V, pad:y\n"
    "p2.start = 2, pad:start\n"
    "p2.coin = 7, pad:back\n"
    "p2.service = 8\n"
    "p2.up = I, pad:dpup, pad:lefty-\n"
    "p2.down = K, pad:dpdown, pad:lefty+\n"
    "p2.left = J, pad:dpleft, pad:leftx-\n"
    "p2.right = L, pad:dpright, pad:leftx+\n"
    "p2.b1 = Q, pad:a\n"
    "p2.b2 = W, pad:b\n"
    "p2.b3 = E, pad:x\n"
    "p2.b4 = R, pad:y\n";

static struct binding bind[ACTION_COUNT][MAX_BINDINGS];
static int nbind[ACTION_COUNT];
static SDL_GameController *pads[MAX_PADS];
static struct jvs_inputs snapshot;
static pthread_mutex_t snap_lock = PTHREAD_MUTEX_INITIALIZER;
static bool prev_fullscreen;

static int action_from_name(const char *name)
{
    if (!strcmp(name, "test")) return A_TEST;
    if (!strcmp(name, "quit")) return A_QUIT;
    if (!strcmp(name, "fullscreen")) return A_FULLSCREEN;
    if ((name[0] == 'p' || name[0] == 'P') && name[1] >= '1' && name[1] < '1' + JVS_PLAYERS && name[2] == '.')
        for (int a = 0; a < PLAYER_ACTIONS; a++)
            if (!strcmp(name + 3, player_action_names[a]))
                return A_START + (name[1] - '1') * PLAYER_ACTIONS + a;
    return -1;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int parse_config(const char *text, const char *origin)
{
    char *copy = strdup(text), *save = NULL;
    int line_no = 0, errors = 0;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        line_no++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) {
            if (*trim(line)) { fprintf(stderr, "%s:%d: expected 'name = binding, ...'\n", origin, line_no); errors++; }
            continue;
        }
        *eq = 0;
        int action = action_from_name(trim(line));
        if (action < 0) { fprintf(stderr, "%s:%d: unknown action '%s'\n", origin, line_no, trim(line)); errors++; continue; }
        nbind[action] = 0;
        char *save2 = NULL;
        for (char *tok = strtok_r(eq + 1, ",", &save2); tok; tok = strtok_r(NULL, ",", &save2)) {
            char *name = trim(tok);
            if (!*name) continue;
            struct binding b = { 0 };
            if (!strncasecmp(name, "pad:", 4)) {
                name += 4;
                size_t len = strlen(name);
                if (len && (name[len - 1] == '+' || name[len - 1] == '-')) {
                    b.dir = name[len - 1] == '+' ? 1 : -1;
                    name[len - 1] = 0;
                    b.kind = PAD_AXIS;
                    b.code = SDL_GameControllerGetAxisFromString(name);
                    if (b.code == SDL_CONTROLLER_AXIS_INVALID) b.code = -1;
                } else {
                    b.kind = PAD_BUTTON;
                    b.code = SDL_GameControllerGetButtonFromString(name);
                    if (b.code == SDL_CONTROLLER_BUTTON_INVALID) b.code = -1;
                }
            } else {
                /* Spelled-out names for keys whose SDL name is punctuation (',' separates bindings). */
                static const struct { const char *alias; SDL_Scancode code; } aliases[] = {
                    { "Comma", SDL_SCANCODE_COMMA }, { "Period", SDL_SCANCODE_PERIOD }, { "Slash", SDL_SCANCODE_SLASH },
                    { "Semicolon", SDL_SCANCODE_SEMICOLON }, { "Apostrophe", SDL_SCANCODE_APOSTROPHE },
                    { "Minus", SDL_SCANCODE_MINUS }, { "Equals", SDL_SCANCODE_EQUALS }, { "Backslash", SDL_SCANCODE_BACKSLASH },
                    { "LeftBracket", SDL_SCANCODE_LEFTBRACKET }, { "RightBracket", SDL_SCANCODE_RIGHTBRACKET },
                    { "Grave", SDL_SCANCODE_GRAVE },
                };
                b.kind = KEY;
                b.code = SDL_GetScancodeFromName(name);
                for (size_t k = 0; k < sizeof aliases / sizeof *aliases; k++)
                    if (!strcasecmp(name, aliases[k].alias)) b.code = aliases[k].code;
                if (b.code == SDL_SCANCODE_UNKNOWN) b.code = -1;
            }
            if (b.code < 0) {
                fprintf(stderr, "%s:%d: unknown key/button '%s' (SDL key names, or pad:a, pad:dpup, pad:leftx- ...)\n",
                        origin, line_no, name);
                errors++;
                continue;
            }
            if (nbind[action] < MAX_BINDINGS) bind[action][nbind[action]++] = b;
        }
    }
    free(copy);
    return errors;
}

/* ---- input scripts: "<frame> <action> <frames held>" per line, frames counted by SwapBuffers.
   TGM3_INPUT replays one; TGM3_RECORD writes what was pressed in the same format, so a play
   session can be replayed exactly (also how a recompiled build gets checked against this one). */

struct press { uint64_t frame, len; int action; };
static struct press *script;
static int nscript;
static FILE *rec_file;
static uint64_t pressed_at[ACTION_COUNT];
static bool was_down[ACTION_COUNT];

static const char *action_name(int a, char *buf, size_t size)
{
    if (a == A_TEST) return "test";
    if (a == A_QUIT) return "quit";
    if (a == A_FULLSCREEN) return "fullscreen";
    snprintf(buf, size, "p%d.%s", (a - A_START) / PLAYER_ACTIONS + 1, player_action_names[(a - A_START) % PLAYER_ACTIONS]);
    return buf;
}

static bool load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return false; }
    char line[256];
    int line_no = 0;
    while (fgets(line, sizeof line, f)) {
        line_no++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        unsigned long long frame, len = 1;
        char name[64];
        int n = sscanf(line, "%llu %63s %llu", &frame, name, &len);
        if (n <= 0) continue;
        int a = n >= 2 ? action_from_name(name) : -1;
        if (a < 0) { fprintf(stderr, "%s:%d: expected '<frame> <action> [frames]'\n", path, line_no); fclose(f); return false; }
        script = realloc(script, sizeof *script * (size_t)(nscript + 1));
        script[nscript++] = (struct press){ frame, len, a };
    }
    fclose(f);
    LOG("input script %s: %d presses", path, nscript);
    return true;
}

static bool scripted(int a, uint64_t frame)
{
    for (int i = 0; i < nscript; i++)
        if (script[i].action == a && frame >= script[i].frame && frame < script[i].frame + script[i].len) return true;
    return false;
}

static void record(const bool *state, uint64_t frame)
{
    if (!rec_file) return;
    for (int a = 0; a < ACTION_COUNT; a++) {
        if (a == A_QUIT || a == A_FULLSCREEN) continue;
        if (state[a] && !was_down[a]) pressed_at[a] = frame;
        if (!state[a] && was_down[a]) {
            char buf[32];
            uint64_t len = frame > pressed_at[a] ? frame - pressed_at[a] : 1;
            fprintf(rec_file, "%llu %s %llu\n", (unsigned long long)pressed_at[a], action_name(a, buf, sizeof buf),
                    (unsigned long long)len);
        }
        was_down[a] = state[a];
    }
}

bool input_init(const char *config_path)
{
    int errors = parse_config(default_config, "<default>");
    if (config_path) {
        FILE *f = fopen(config_path, "r");
        if (!f) { perror(config_path); return false; }
        char *text = NULL;
        size_t cap = 0;
        if (getdelim(&text, &cap, '\0', f) >= 0) errors += parse_config(text, config_path);
        free(text);
        fclose(f);
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (getenv("TGM3_INPUT") && !load_script(getenv("TGM3_INPUT"))) return false;
    if (getenv("TGM3_RECORD") && !(rec_file = fopen(getenv("TGM3_RECORD"), "w"))) { perror(getenv("TGM3_RECORD")); return false; }
    if (rec_file) setvbuf(rec_file, NULL, _IOLBF, 0);
    return errors == 0;
}

static void open_pads(void)
{
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i] && !SDL_GameControllerGetAttached(pads[i])) { SDL_GameControllerClose(pads[i]); pads[i] = NULL; }
    for (int j = 0; j < SDL_NumJoysticks(); j++) {
        if (!SDL_IsGameController(j)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        bool open = false;
        for (int i = 0; i < MAX_PADS; i++)
            if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) open = true;
        if (open) continue;
        for (int i = 0; i < MAX_PADS; i++)
            if (!pads[i]) {
                pads[i] = SDL_GameControllerOpen(j);
                if (pads[i]) LOG("controller %s -> player %d", SDL_GameControllerName(pads[i]), i + 1);
                break;
            }
    }
}

static bool active(int action, int player)
{
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    SDL_GameController *pad = player >= 0 ? pads[player] : pads[0];
    for (int i = 0; i < nbind[action]; i++) {
        const struct binding *b = &bind[action][i];
        if (b->kind == KEY && keys[b->code]) return true;
        if (!pad) continue;
        if (b->kind == PAD_BUTTON && SDL_GameControllerGetButton(pad, b->code)) return true;
        if (b->kind == PAD_AXIS) {
            int v = SDL_GameControllerGetAxis(pad, b->code);
            if (b->dir > 0 ? v > 16384 : v < -16384) return true;
        }
    }
    return false;
}

/* Main thread, from PeekMessageA: process SDL events and refresh the switch snapshot. */
void input_pump(void)
{
    SDL_Event ev;
    bool rescan = false;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            LOG("window closed");
            fflush(NULL);
            _exit(0);
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            rescan = true;
            break;
        case SDL_KEYDOWN:
            if (ev.key.keysym.sym == SDLK_F12 && !ev.key.repeat) {
                extern bool g_snapshot_requested;
                g_snapshot_requested = true;
            }
            if (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT) && !ev.key.repeat) {
                Uint32 fs = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(g_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
            break;
        }
    }
    if (rescan) open_pads();

    uint64_t frame = gl_frame_count();
    bool state[ACTION_COUNT];
    for (int a = 0; a < ACTION_COUNT; a++)
        state[a] = active(a, a >= A_START ? (a - A_START) / PLAYER_ACTIONS : -1) || scripted(a, frame);
    record(state, frame);

    struct jvs_inputs in = { 0 };
    in.test = state[A_TEST];
    for (int p = 0; p < JVS_PLAYERS; p++) {
        const bool *s = state + A_START + p * PLAYER_ACTIONS;
        struct jvs_player *pl = &in.p[p];
        pl->start = s[A_START - A_START];
        pl->service = s[A_SERVICE - A_START];
        pl->coin = s[A_COIN - A_START];
        pl->up = s[A_UP - A_START];
        pl->down = s[A_DOWN - A_START];
        pl->left = s[A_LEFT - A_START];
        pl->right = s[A_RIGHT - A_START];
        for (int k = 0; k < JVS_BUTTONS; k++) pl->button[k] = s[A_B1 + k - A_START];
    }
    pthread_mutex_lock(&snap_lock);
    snapshot = in;
    pthread_mutex_unlock(&snap_lock);

    if (state[A_QUIT]) {
        LOG("quit pressed");
        if (rec_file) fflush(rec_file);
        fflush(NULL);
        _exit(0);
    }
    if (state[A_FULLSCREEN] && !prev_fullscreen) {
        Uint32 fs = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(g_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    prev_fullscreen = state[A_FULLSCREEN];
}

void input_snapshot(struct jvs_inputs *out)
{
    pthread_mutex_lock(&snap_lock);
    *out = snapshot;
    pthread_mutex_unlock(&snap_lock);
}
