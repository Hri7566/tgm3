#include "input.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <libevdev/libevdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define MAX_DEVICES  32
#define MAX_BINDINGS 8

enum action {
    A_TEST, A_QUIT,
    A_START, A_SERVICE, A_COIN, A_UP, A_DOWN, A_LEFT, A_RIGHT,
    A_B1, A_B2, A_B3, A_B4, A_B5, A_B6,
    PLAYER_ACTIONS = A_B6 - A_START + 1,
    ACTION_COUNT = A_START + JVS_PLAYERS * PLAYER_ACTIONS,
};

static const char *const player_action_names[PLAYER_ACTIONS] = {
    "start", "service", "coin", "up", "down", "left", "right",
    "b1", "b2", "b3", "b4", "b5", "b6",
};

/* Defaults follow typex_io.log (coin 5/7, service 6/8, test 9, quit Esc) plus arrows/ZXCV and a gamepad on P1. */
static const char default_config[] =
    "test = KEY_9\n"
    "quit = KEY_ESC\n"
    "p1.start = KEY_1, BTN_START\n"
    "p1.service = KEY_6\n"
    "p1.coin = KEY_5, BTN_SELECT\n"
    "p1.up = KEY_UP, BTN_DPAD_UP, ABS_HAT0Y-, ABS_Y-\n"
    "p1.down = KEY_DOWN, BTN_DPAD_DOWN, ABS_HAT0Y+, ABS_Y+\n"
    "p1.left = KEY_LEFT, BTN_DPAD_LEFT, ABS_HAT0X-, ABS_X-\n"
    "p1.right = KEY_RIGHT, BTN_DPAD_RIGHT, ABS_HAT0X+, ABS_X+\n"
    "p1.b1 = KEY_Z, BTN_SOUTH\n"
    "p1.b2 = KEY_X, BTN_EAST\n"
    "p1.b3 = KEY_C, BTN_WEST\n"
    "p1.b4 = KEY_V, BTN_NORTH\n"
    "p2.start = KEY_2\n"
    "p2.service = KEY_8\n"
    "p2.coin = KEY_7\n"
    "p2.up = KEY_I\n"
    "p2.down = KEY_K\n"
    "p2.left = KEY_J\n"
    "p2.right = KEY_L\n"
    "p2.b1 = KEY_Q\n"
    "p2.b2 = KEY_W\n"
    "p2.b3 = KEY_E\n"
    "p2.b4 = KEY_R\n";

struct binding {
    unsigned type, code;
    int dir;                       /* ABS bindings: -1 or +1; keys: 0 */
};

struct device {
    int fd;
    struct libevdev *dev;
    char path[64];
    /* Axes only count once they've moved: some non-gamepads (e.g. RGB controllers) expose
       ABS_X/ABS_Y parked at one end, which would otherwise read as a held direction. */
    bool abs_moved[ABS_CNT];
};

struct input {
    struct binding bind[ACTION_COUNT][MAX_BINDINGS];
    int nbind[ACTION_COUNT];
    struct device dev[MAX_DEVICES];
    int ndev;
    /* Irrelevant nodes, remembered so hotplug rescans don't reopen them: closing evdev fds
       can take tens of ms each, and jvsd has to answer the game within ~20 ms. */
    char ignored[64][64];
    int nignored;
    int inotify_fd;
    bool verbose;
    bool warned_access;
};

static int action_from_name(const char *name)
{
    if (!strcmp(name, "test")) return A_TEST;
    if (!strcmp(name, "quit")) return A_QUIT;
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

static int parse_config(struct input *in, const char *text, const char *origin)
{
    char *copy = strdup(text), *save = NULL;
    int line_no = 0, errors = 0;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        line_no++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) {
            if (*trim(line)) { fprintf(stderr, "%s:%d: expected 'name = CODE, ...'\n", origin, line_no); errors++; }
            continue;
        }
        *eq = 0;
        char *name = trim(line);
        int action = action_from_name(name);
        if (action < 0) { fprintf(stderr, "%s:%d: unknown action '%s'\n", origin, line_no, name); errors++; continue; }

        in->nbind[action] = 0;   /* later lines replace earlier ones */
        char *save2 = NULL;
        for (char *tok = strtok_r(eq + 1, ",", &save2); tok; tok = strtok_r(NULL, ",", &save2)) {
            char *code_name = trim(tok);
            if (!*code_name) continue;
            struct binding b = { 0 };
            size_t len = strlen(code_name);
            if (!strncmp(code_name, "ABS_", 4) && (code_name[len - 1] == '+' || code_name[len - 1] == '-')) {
                b.dir = code_name[len - 1] == '+' ? 1 : -1;
                code_name[len - 1] = 0;
            }
            int type = libevdev_event_type_from_code_name(code_name);
            int code = libevdev_event_code_from_code_name(code_name);
            if (type < 0 || code < 0 || (type != EV_KEY && type != EV_ABS) || (type == EV_ABS) != (b.dir != 0)) {
                fprintf(stderr, "%s:%d: bad code '%s' (use KEY_*, BTN_*, or ABS_*+/-)\n", origin, line_no, code_name);
                errors++;
                continue;
            }
            b.type = (unsigned)type;
            b.code = (unsigned)code;
            if (in->nbind[action] < MAX_BINDINGS)
                in->bind[action][in->nbind[action]++] = b;
        }
    }
    free(copy);
    return errors;
}

static bool device_is_relevant(const struct input *in, const struct libevdev *dev)
{
    for (int a = 0; a < ACTION_COUNT; a++)
        for (int i = 0; i < in->nbind[a]; i++)
            if (libevdev_has_event_code(dev, in->bind[a][i].type, in->bind[a][i].code))
                return true;
    return false;
}

static void remove_device(struct input *in, int i)
{
    if (in->verbose) fprintf(stderr, "input: removed %s\n", in->dev[i].path);
    libevdev_free(in->dev[i].dev);
    close(in->dev[i].fd);
    in->dev[i] = in->dev[--in->ndev];
}

static bool is_known(const struct input *in, const char *path)
{
    for (int i = 0; i < in->ndev; i++)
        if (!strcmp(in->dev[i].path, path)) return true;
    for (int i = 0; i < in->nignored; i++)
        if (!strcmp(in->ignored[i], path)) return true;
    return false;
}

static void forget_ignored(struct input *in, const char *path)
{
    for (int i = 0; i < in->nignored; i++)
        if (!strcmp(in->ignored[i], path)) {
            memcpy(in->ignored[i], in->ignored[--in->nignored], sizeof in->ignored[i]);
            return;
        }
}

static void rescan(struct input *in)
{
    glob_t g;
    if (glob("/dev/input/event*", 0, NULL, &g) != 0) return;
    for (size_t k = 0; k < g.gl_pathc && in->ndev < MAX_DEVICES; k++) {
        const char *path = g.gl_pathv[k];
        if (is_known(in, path)) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            /* Not remembered: udev may grant access moment later (we'll see IN_ATTRIB). */
            if (errno == EACCES && !in->warned_access) {
                fprintf(stderr, "input: no permission to read %s (add yourself to the 'input' group)\n", path);
                in->warned_access = true;
            }
            continue;
        }
        struct libevdev *dev = NULL;
        if (libevdev_new_from_fd(fd, &dev) < 0 || !device_is_relevant(in, dev)) {
            libevdev_free(dev);
            close(fd);
            if (in->nignored < (int)(sizeof in->ignored / sizeof *in->ignored))
                snprintf(in->ignored[in->nignored++], sizeof *in->ignored, "%s", path);
            continue;
        }
        struct device *d = &in->dev[in->ndev++];
        memset(d, 0, sizeof *d);
        d->fd = fd;
        d->dev = dev;
        snprintf(d->path, sizeof d->path, "%s", path);
        if (in->verbose) fprintf(stderr, "input: using %s (%s)\n", path, libevdev_get_name(dev));
    }
    globfree(&g);
}

/* Handle /dev/input hotplug notifications; rescans only when something changed. */
static void handle_hotplug(struct input *in)
{
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool changed = false;
    ssize_t len;
    if (in->inotify_fd < 0) return;
    while ((len = read(in->inotify_fd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + len;) {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            if (ev->len && !strncmp(ev->name, "event", 5)) {
                char path[64];
                snprintf(path, sizeof path, "/dev/input/%s", ev->name);
                if (ev->mask & IN_DELETE) forget_ignored(in, path);
                changed = true;
            }
            p += sizeof *ev + ev->len;
        }
    }
    if (changed) rescan(in);
}

struct input *input_open(const char *config_path, bool verbose)
{
    struct input *in = calloc(1, sizeof *in);
    in->verbose = verbose;
    int errors = parse_config(in, default_config, "<default>");
    if (config_path) {
        FILE *f = fopen(config_path, "r");
        if (!f) { perror(config_path); free(in); return NULL; }
        char *text = NULL;
        size_t cap = 0;
        ssize_t len = getdelim(&text, &cap, '\0', f);
        fclose(f);
        if (len >= 0) errors += parse_config(in, text, config_path);
        free(text);
    }
    if (errors) { free(in); return NULL; }
    in->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (in->inotify_fd >= 0 &&
        inotify_add_watch(in->inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB | IN_DELETE) < 0) {
        close(in->inotify_fd);
        in->inotify_fd = -1;
    }
    if (in->inotify_fd < 0) fprintf(stderr, "input: hotplug disabled (inotify failed)\n");
    rescan(in);
    return in;
}

void input_close(struct input *in)
{
    while (in->ndev)
        remove_device(in, 0);
    if (in->inotify_fd >= 0) close(in->inotify_fd);
    free(in);
}

int input_pollfds(struct input *in, struct pollfd *fds, int max)
{
    int n = 0;
    if (in->inotify_fd >= 0 && n < max)
        fds[n++] = (struct pollfd){ .fd = in->inotify_fd, .events = POLLIN };
    for (int i = 0; i < in->ndev && n < max; i++)
        fds[n++] = (struct pollfd){ .fd = in->dev[i].fd, .events = POLLIN };
    return n;
}

static void drain(struct input *in)
{
    for (int i = 0; i < in->ndev; i++) {
        struct device *d = &in->dev[i];
        struct input_event ev;
        int rc;
        do {
            rc = libevdev_next_event(d->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            while (rc == LIBEVDEV_READ_STATUS_SYNC) {
                if (ev.type == EV_ABS && ev.code < ABS_CNT) d->abs_moved[ev.code] = true;
                rc = libevdev_next_event(d->dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
            }
            if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS && ev.code < ABS_CNT)
                d->abs_moved[ev.code] = true;
        } while (rc == LIBEVDEV_READ_STATUS_SUCCESS);
        if (rc == -ENODEV) remove_device(in, i--);
    }
}

static bool binding_active(const struct device *d, const struct binding *b)
{
    if (!libevdev_has_event_code(d->dev, b->type, b->code)) return false;
    int v = libevdev_get_event_value(d->dev, b->type, b->code);
    if (b->type == EV_KEY) return v != 0;
    const struct input_absinfo *abs = libevdev_get_abs_info(d->dev, b->code);
    if (!d->abs_moved[b->code] || !abs || abs->maximum <= abs->minimum) return false;
    /* Past half of the travel from centre in the bound direction. */
    long centre2 = (long)abs->minimum + abs->maximum;
    long span = (long)abs->maximum - abs->minimum;
    long off2 = 2L * v - centre2;
    return b->dir > 0 ? off2 * 2 > span : -off2 * 2 > span;
}

static bool action_active(const struct input *in, int a)
{
    for (int i = 0; i < in->ndev; i++)
        for (int k = 0; k < in->nbind[a]; k++)
            if (binding_active(&in->dev[i], &in->bind[a][k]))
                return true;
    return false;
}

bool input_update(struct input *in, struct jvs_inputs *out)
{
    handle_hotplug(in);
    drain(in);
    memset(out, 0, sizeof *out);
    out->test = action_active(in, A_TEST);
    for (int p = 0; p < JVS_PLAYERS; p++) {
        int base = A_START + p * PLAYER_ACTIONS;
        struct jvs_player *pl = &out->p[p];
        pl->start   = action_active(in, base + A_START   - A_START);
        pl->service = action_active(in, base + A_SERVICE - A_START);
        pl->coin    = action_active(in, base + A_COIN    - A_START);
        pl->up      = action_active(in, base + A_UP      - A_START);
        pl->down    = action_active(in, base + A_DOWN    - A_START);
        pl->left    = action_active(in, base + A_LEFT    - A_START);
        pl->right   = action_active(in, base + A_RIGHT   - A_START);
        for (int k = 0; k < JVS_BUTTONS; k++)
            pl->button[k] = action_active(in, base + A_B1 + k - A_START);
    }
    return action_active(in, A_QUIT);
}
