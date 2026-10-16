/* evdev input (replaces typex_io.dll's DirectInput8 polling + typex_bindings.bin). */
#ifndef INPUT_H
#define INPUT_H

#include <poll.h>
#include <stdbool.h>

#include "jvs.h"

struct input;

/* config_path may be NULL to use the built-in default bindings. */
struct input *input_open(const char *config_path, bool verbose);
void input_close(struct input *in);

/* Fill pollfds with device + hotplug fds; returns count (at most max). */
int input_pollfds(struct input *in, struct pollfd *fds, int max);

/* Drain pending events and compute the current state. Returns true if "quit" is held. */
bool input_update(struct input *in, struct jvs_inputs *out);

#endif
