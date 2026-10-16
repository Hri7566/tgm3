/* Glue between the recompiled-code runtime (rt.c) and the Win32 layer (host.c). */
#ifndef RT_HOST_H
#define RT_HOST_H

#include "rt.h"

extern __thread CPU *g_cpu;

void rt_init_dispatch(void);
/* Runs the HLE function at entry address t, popping its stdcall frame; false if t isn't one. */
int hle_call(CPU *c, uint32_t t);
void rt_fatal(CPU *c, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void rt_dump(CPU *c);

#endif
