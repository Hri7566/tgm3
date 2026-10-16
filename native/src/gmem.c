/*
 * Guest memory: address-space reservation and a heap, both in guest addresses.
 *
 * With GUEST_WINDOW the guest's 4 GiB address space is a reserved window at M and every
 * guest address is an offset into it (64-bit and non-x86 hosts). Without it, guest addresses
 * are host addresses (the native loader and the i386 recompiled build) and reservations are
 * ordinary mmaps. The heap on top is the same either way, so both builds hand the game
 * memory the same way.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

#include "w32.h"

#define GRANULE 0x10000u            /* Windows' allocation granularity */
#define PAGE    0x1000u

uint8_t *M;

#ifdef GUEST_WINDOW

/* One byte per 64 KiB granule of the window: nonzero = reserved. */
static uint8_t granules[0x10000];
static pthread_mutex_t gran_lock = PTHREAD_MUTEX_INITIALIZER;

void gmem_init(void)
{
    if (M) return;
    void *p = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) w32_fatal("cannot reserve the 4 GiB guest address space: %s", strerror(errno));
    M = p;
    /* Keep the null page region and everything above 0xF0000000 (Win32 entry addresses) unused. */
    granules[0] = 1;
    memset(granules + (0xF0000000u / GRANULE), 1, 0x10000 - 0xF0000000u / GRANULE);
}

gptr gmem_reserve(gptr addr, uint32_t size)
{
    uint32_t n = (size + GRANULE - 1) / GRANULE;
    pthread_mutex_lock(&gran_lock);
    uint32_t first = 0;
    if (addr) {
        first = addr / GRANULE;
        for (uint32_t i = first; i < first + n; i++)
            if (i >= 0x10000 || granules[i]) { pthread_mutex_unlock(&gran_lock); return 0; }
    } else {
        /* First fit from 256 MiB up, leaving the low area for fixed-address requests. */
        uint32_t run = 0;
        for (uint32_t i = 0x10000000u / GRANULE; i < 0x10000 && run < n; i++) {
            run = granules[i] ? 0 : run + 1;
            if (run == n) first = i + 1 - n;
        }
        if (run < n) { pthread_mutex_unlock(&gran_lock); return 0; }
    }
    memset(granules + first, 1, n);
    pthread_mutex_unlock(&gran_lock);
    return first * GRANULE;
}

void gmem_release(gptr base, uint32_t size)
{
    uint32_t n = (size + GRANULE - 1) / GRANULE;
    mmap(M + base, (size_t)n * GRANULE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    pthread_mutex_lock(&gran_lock);
    memset(granules + base / GRANULE, 0, n);
    pthread_mutex_unlock(&gran_lock);
}

#else

void gmem_init(void) {}

gptr gmem_reserve(gptr addr, uint32_t size)
{
    size = (size + GRANULE - 1) & ~(GRANULE - 1);
    void *p;
    if (addr) {
        p = mmap(G2H(void *, addr), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
        if (p != G2H(void *, addr)) { if (p != MAP_FAILED) munmap(p, size); return 0; }
        return addr;
    }
    p = mmap(NULL, size + GRANULE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return 0;
    uintptr_t lo = (uintptr_t)p, aligned = (lo + GRANULE - 1) & ~(uintptr_t)(GRANULE - 1);
    if (aligned > lo) munmap(p, aligned - lo);
    if (aligned + size < lo + size + GRANULE) munmap((void *)(aligned + size), lo + size + GRANULE - aligned - size);
    return H2G(aligned);
}

void gmem_release(gptr base, uint32_t size)
{
    munmap(G2H(void *, base), (size + GRANULE - 1) & ~(GRANULE - 1));
}

#endif

void gmem_protect(gptr addr, uint32_t size, int prot)
{
    uint32_t start = addr & ~(PAGE - 1), end = (addr + size + PAGE - 1) & ~(PAGE - 1);
    if (mprotect(G2H(void *, start), end - start, prot) != 0)
        WARN("mprotect(%08x, %x): %s", start, end - start, strerror(errno));
}

void gmem_decommit(gptr addr, uint32_t size)
{
    uint32_t start = addr & ~(PAGE - 1), end = (addr + size + PAGE - 1) & ~(PAGE - 1);
    mprotect(G2H(void *, start), end - start, PROT_NONE);
    madvise(G2H(void *, start), end - start, MADV_DONTNEED);
}

/* ---- heap ----
   Power-of-two size classes (32 B .. 64 KiB including a 16-byte header) carved from 1 MiB
   arenas, with a free list per class; anything bigger gets its own reservation. */

#define HDR 16u
#define NCLASS 12                    /* 32 << 11 == 64 KiB */
#define ARENA (1u << 20)
#define LARGE_CLASS 0xFFu
#define MAGIC 0x6D656D67u

struct hdr { uint32_t size, cls, span, magic; };

static gptr free_list[NCLASS];
static gptr arena_next, arena_end;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

static struct hdr *hdr_of(gptr p) { return G2H(struct hdr *, p - HDR); }

static int class_for(uint32_t total)
{
    for (int c = 0; c < NCLASS; c++)
        if (total <= (32u << c)) return c;
    return -1;
}

gptr guest_alloc(size_t size)
{
    if (size > 0xF0000000u) return 0;
    uint32_t total = (uint32_t)size + HDR;
    int c = class_for(total);
    gptr block;
    uint32_t span;
    if (c < 0) {
        span = (total + PAGE - 1) & ~(PAGE - 1);
        block = gmem_reserve(0, span);
        if (!block) return 0;
        gmem_protect(block, span, PROT_READ | PROT_WRITE);
    } else {
        span = 32u << c;
        pthread_mutex_lock(&heap_lock);
        block = free_list[c];
        if (block) {
            free_list[c] = rd32(block);
        } else {
            if (arena_end - arena_next < span) {
                arena_next = gmem_reserve(0, ARENA);
                if (!arena_next) { pthread_mutex_unlock(&heap_lock); return 0; }
                gmem_protect(arena_next, ARENA, PROT_READ | PROT_WRITE);
                arena_end = arena_next + ARENA;
            }
            block = arena_next;
            arena_next += span;
        }
        pthread_mutex_unlock(&heap_lock);
    }
    memset(G2H(void *, block), 0, total);
    *G2H(struct hdr *, block) = (struct hdr){ (uint32_t)size, c < 0 ? LARGE_CLASS : (uint32_t)c, span, MAGIC };
    return block + HDR;
}

void guest_free(gptr p)
{
    if (!p) return;
    struct hdr *h = hdr_of(p);
    if (h->magic != MAGIC) { WARN("guest_free(%08x): not a heap block", p); return; }
    h->magic = 0;
    gptr block = p - HDR;
    if (h->cls == LARGE_CLASS) {
        gmem_release(block, h->span);
        return;
    }
    pthread_mutex_lock(&heap_lock);
    wr32(block, free_list[h->cls]);
    free_list[h->cls] = block;
    pthread_mutex_unlock(&heap_lock);
}

uint32_t guest_size(gptr p)
{
    struct hdr *h = hdr_of(p);
    return h->magic == MAGIC ? h->size : 0xFFFFFFFFu;
}

gptr guest_realloc(gptr p, size_t size)
{
    if (!p) return guest_alloc(size);
    struct hdr *h = hdr_of(p);
    if (h->magic != MAGIC) return 0;
    if (size + HDR <= h->span) {
        if (size > h->size) memset(G2H(uint8_t *, p) + h->size, 0, size - h->size);
        h->size = (uint32_t)size;
        return p;
    }
    gptr q = guest_alloc(size);
    if (!q) return 0;
    memcpy(G2H(void *, q), G2H(void *, p), h->size);
    guest_free(p);
    return q;
}

gptr guest_strdup(const char *s)
{
    gptr p = guest_alloc(strlen(s) + 1);
    if (!p) w32_fatal("out of guest memory");
    strcpy(G2H(char *, p), s);
    return p;
}
