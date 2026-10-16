/* Runtime support for recompiled code: dispatch, traps, string ops, x87 helpers. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "rt.h"
#include "rt_host.h"

/* ---- dispatch ---- */

#define TEXT_LO 0x401000u
#define TEXT_HI 0x467000u
static rc_fn *ftab;          /* by (va - TEXT_LO) */

void rt_init_dispatch(void)
{
    ftab = calloc(TEXT_HI - TEXT_LO, sizeof *ftab);
    for (unsigned i = 0; i < rc_nfuncs; i++)
        if (rc_funcs[i].va >= TEXT_LO && rc_funcs[i].va < TEXT_HI) ftab[rc_funcs[i].va - TEXT_LO] = rc_funcs[i].fn;
}

void rt_call(CPU *c, uint32_t t)
{
    if (t - TEXT_LO < TEXT_HI - TEXT_LO && ftab[t - TEXT_LO]) {
        ftab[t - TEXT_LO](c);
        return;
    }
    if (hle_call(c, t)) return;
    rt_fatal(c, "call/jump to %08x, which is not a known function", t);
}

void rt_jmp(CPU *c, uint32_t t) { rt_call(c, t); }

/* ---- faults ---- */

void rt_fatal(CPU *c, const char *fmt, ...)
{
    va_list ap;
    fputs("tgm3-recomp: fatal: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (c) rt_dump(c);
    _exit(1);
}

void rt_dump(CPU *c)
{
    fprintf(stderr, "  last block %08x\n", c->eip);
    fprintf(stderr, "  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
            c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->esp);
    fprintf(stderr, "  stack game.exe addrs:");
    for (int i = 0, n = 0; i < 512 && n < 16; i++) {
        uint32_t v = RD32(c->esp + 4u * (uint32_t)i);
        if (v >= TEXT_LO && v < TEXT_HI) { fprintf(stderr, " %08x", v); n++; }
    }
    fputc('\n', stderr);
}

void rt_trap(CPU *c, uint32_t va, const char *why)
{
    rt_fatal(c, "reached %s at %08x", why, va);
}

void rt_div0(CPU *c, uint32_t va)
{
    rt_fatal(c, "integer divide error at %08x", va);
}

void rt_badret(CPU *c, uint32_t expected)
{
    static int count;
    if (count++ < 20)
        fprintf(stderr, "tgm3-recomp: warning: return to %08x, expected %08x (block %08x)\n", c->ret_to, expected, c->eip);
}

/* ---- string ops (rep prefix) ---- */

void rt_rep_movs(CPU *c, int size)
{
    uint32_t n = c->ecx;
    if (!n) return;
    uint32_t bytes = n * (uint32_t)size;
    if (!c->df && c->edi - c->esi >= bytes) {
        memmove(GM(c->edi), GM(c->esi), bytes);   /* no forward overlap: same as element copy */
        c->esi += bytes;
        c->edi += bytes;
    } else {
        int32_t step = c->df ? -size : size;
        for (; n; n--) {
            if (size == 4) WR32(c->edi, RD32(c->esi));
            else if (size == 2) WR16(c->edi, RD16(c->esi));
            else WR8(c->edi, RD8(c->esi));
            c->esi += (uint32_t)step;
            c->edi += (uint32_t)step;
        }
    }
    c->ecx = 0;
}

void rt_rep_stos(CPU *c, int size)
{
    uint32_t n = c->ecx;
    if (!n) return;
    if (!c->df && size == 1) {
        memset(GM(c->edi), (int)(c->eax & 0xFF), n);
        c->edi += n;
    } else if (!c->df && size == 4 && c->eax == 0) {
        memset(GM(c->edi), 0, (size_t)n * 4);
        c->edi += n * 4;
    } else {
        int32_t step = c->df ? -size : size;
        for (; n; n--) {
            if (size == 4) WR32(c->edi, c->eax);
            else if (size == 2) WR16(c->edi, c->eax);
            else WR8(c->edi, c->eax);
            c->edi += (uint32_t)step;
        }
    }
    c->ecx = 0;
}

/* ---- x87 ---- */

double RDF80(uint32_t a)
{
    uint64_t mant = RD64(a);
    uint32_t se = RD16(a + 8);
    int sign = se >> 15, exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void WRF80(uint32_t a, double v)
{
    uint64_t mant = 0;
    uint32_t se = signbit(v) ? 0x8000u : 0;
    if (isnan(v)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e;
        double f = frexp(fabs(v), &e);          /* f in [0.5, 1) */
        mant = (uint64_t)ldexp(f, 64);
        se |= (uint32_t)(e - 1 + 16383);
    }
    WR64(a, mant);
    WR16(a + 8, se);
}

void rt_fxam(CPU *c)
{
    double v = ST(0);
    uint32_t f = c->fsw & ~0x4700u;
    if (signbit(v)) f |= 0x0200;                       /* C1 = sign */
    if (isnan(v)) f |= 0x0100;                         /* C0 */
    else if (isinf(v)) f |= 0x0500;                    /* C2 C0 */
    else if (v == 0) f |= 0x4000;                      /* C3 */
    else if (fpclassify(v) == FP_SUBNORMAL) f |= 0x4400;
    else f |= 0x0400;                                  /* C2: normal */
    c->fsw = f;
}

void rt_fninit(CPU *c)
{
    c->fcw = 0x37F;
    c->fsw = 0;
    c->top = 0;
}
