/*
 * Runtime interface for C generated from game.exe by tools/recomp.py.
 *
 * Guest registers live in a CPU struct per thread; generated functions copy them into locals
 * (REGS_DECL) and write them back around calls (SYNC_OUT / SYNC_IN). Guest memory is the
 * 4 GiB window at M: on a 32-bit host M is 0 and guest addresses are host addresses, on a
 * 64-bit host M is a reserved region.
 */
#ifndef RT_H
#define RT_H

#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;          /* direction flag */
    uint32_t fs_base;     /* TEB */
    uint32_t ret_to;      /* address the last `ret` popped */
    uint32_t eip;         /* last block entered (diagnostics) */
    uint32_t top, fcw, fsw;
    double st[8];         /* x87 registers, physical order; ST(i) = st[(top + i) & 7] */
} CPU;

typedef void (*rc_fn)(CPU *c);
struct rc_func { uint32_t va; rc_fn fn; };
extern const struct rc_func rc_funcs[];
extern const unsigned rc_nfuncs;
extern const uint32_t rc_entry;

extern uint8_t *M;

/* ---- guest memory ---- */
#define GM(a) (M + (uint32_t)(a))
static inline uint32_t RD8(uint32_t a) { return *GM(a); }
static inline uint32_t RD16(uint32_t a) { uint16_t v; memcpy(&v, GM(a), 2); return v; }
static inline uint32_t RD32(uint32_t a) { uint32_t v; memcpy(&v, GM(a), 4); return v; }
static inline uint64_t RD64(uint32_t a) { uint64_t v; memcpy(&v, GM(a), 8); return v; }
static inline void WR8(uint32_t a, uint32_t v) { *GM(a) = (uint8_t)v; }
static inline void WR16(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(GM(a), &x, 2); }
static inline void WR32(uint32_t a, uint32_t v) { memcpy(GM(a), &v, 4); }
static inline void WR64(uint32_t a, uint64_t v) { memcpy(GM(a), &v, 8); }
static inline double RDF32(uint32_t a) { float f; memcpy(&f, GM(a), 4); return f; }
static inline double RDF64(uint32_t a) { double d; memcpy(&d, GM(a), 8); return d; }
static inline void WRF32(uint32_t a, double v) { float f = (float)v; memcpy(GM(a), &f, 4); }
static inline void WRF64(uint32_t a, double v) { memcpy(GM(a), &v, 8); }
double RDF80(uint32_t a);
void WRF80(uint32_t a, double v);

/* xchg with memory is atomic on x86 */
static inline uint32_t XCHG32(uint32_t a, uint32_t v)
{
    if (!(a & 3)) return __atomic_exchange_n((uint32_t *)(void *)GM(a), v, __ATOMIC_SEQ_CST);
    uint32_t o = RD32(a); WR32(a, v); return o;
}
static inline uint32_t XCHG16(uint32_t a, uint32_t v) { uint32_t o = RD16(a); WR16(a, v); return o; }
static inline uint32_t XCHG8(uint32_t a, uint32_t v) { uint32_t o = RD8(a); WR8(a, v); return o; }

#define PARITY(v) ((uint32_t)(~__builtin_parity((uint32_t)(v) & 0xFFu) & 1u))

/* ---- registers ---- */
#define REGS_DECL \
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, \
             esp = c->esp, ebp = c->ebp, esi = c->esi, edi = c->edi; \
    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0; \
    (void)cf; (void)zf; (void)sf; (void)of; (void)pf
#define SYNC_OUT do { c->eax = eax; c->ecx = ecx; c->edx = edx; c->ebx = ebx; \
                      c->esp = esp; c->ebp = ebp; c->esi = esi; c->edi = edi; } while (0)
#define SYNC_IN  do { eax = c->eax; ecx = c->ecx; edx = c->edx; ebx = c->ebx; \
                      esp = c->esp; ebp = c->ebp; esi = c->esi; edi = c->edi; } while (0)

#ifdef RC_NO_BLK
#define BLK(va) ((void)0)
#else
#define BLK(va) (c->eip = (va))
#endif
#define CHECK_RET(va) do { if (__builtin_expect(c->ret_to != (va), 0)) rt_badret(c, (va)); } while (0)

/* ---- runtime entry points ---- */
void rt_call(CPU *c, uint32_t target);           /* indirect call (return address already pushed) */
void rt_jmp(CPU *c, uint32_t target);            /* indirect jump leaving the function */
void rt_trap(CPU *c, uint32_t va, const char *why) __attribute__((noreturn, cold));
void rt_div0(CPU *c, uint32_t va) __attribute__((noreturn, cold));
void rt_badret(CPU *c, uint32_t expected) __attribute__((cold));
void rt_rep_movs(CPU *c, int size);
void rt_rep_stos(CPU *c, int size);

/* ---- x87 ---- */
#define ST(i) (c->st[(c->top + (i)) & 7])
#define FPUSH(v) do { double v_ = (v); c->top = (c->top - 1) & 7; c->st[c->top] = v_; } while (0)
#define FPOP() (c->top = (c->top + 1) & 7)
#define FCOM(a, b) rt_fcom(c, (a), (b))
#define FSW() ((c->fsw & ~0x3800u) | ((c->top & 7) << 11))
#define FIST16(c, v) rt_fist(c, v, 16)
#define FIST32(c, v) rt_fist(c, v, 32)
#define FIST64(c, v) rt_fist(c, v, 64)
#define FROUND(c, v) rt_fround(c, v)
#define FXAM(c) rt_fxam(c)
#define FNINIT(c) rt_fninit(c)

static inline void rt_fcom(CPU *c, double a, double b)
{
    uint32_t f = c->fsw & ~0x4500u;       /* C3 C2 C0 */
    if (a != a || b != b) f |= 0x4500u;
    else if (a < b) f |= 0x0100u;
    else if (a == b) f |= 0x4000u;
    c->fsw = f;
}

static inline double rt_fround(CPU *c, double v)
{
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static inline int64_t rt_fist(CPU *c, double v, int bits)
{
    double r = rt_fround(c, v);
    double lim = bits == 64 ? 9223372036854775808.0 : bits == 32 ? 2147483648.0 : 32768.0;
    if (!(r >= -lim && r < lim)) return bits == 64 ? INT64_MIN : bits == 32 ? INT32_MIN : INT16_MIN;
    return (int64_t)r;
}

void rt_fxam(CPU *c);
void rt_fninit(CPU *c);

#endif
