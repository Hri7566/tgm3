/* KERNEL32 subset used by game.exe (and its statically linked MSVC C runtime). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "w32.h"

#define A(i)      (a[i])
#define S(i)      G2H(const char *, a[i])
#define INVALID_HANDLE_VALUE 0xFFFFFFFFu
#define CURRENT_PROCESS      0xFFFFFFFFu
#define CURRENT_THREAD       0xFFFFFFFEu
#define IMAGE_BASE           0x400000u

/* ======================================================================== paths */

/* Every drive maps to the game folder; the working directory is a Windows path. */
static char cwd[1024] = "C:\\";
static pthread_mutex_t cwd_lock = PTHREAD_MUTEX_INITIALIZER;

/* Make an absolute, normalised Windows path ("C:\a\b"). */
static void win_absolute(const char *in, char *out, size_t size)
{
    char tmp[2048];
    pthread_mutex_lock(&cwd_lock);
    if (isalpha((unsigned char)in[0]) && in[1] == ':' && (in[2] == '\\' || in[2] == '/'))
        snprintf(tmp, sizeof tmp, "%s", in);
    else if (in[0] == '\\' || in[0] == '/')
        snprintf(tmp, sizeof tmp, "%c:%s", cwd[0], in);
    else if (isalpha((unsigned char)in[0]) && in[1] == ':')
        snprintf(tmp, sizeof tmp, "%s\\%s", cwd, in + 2);
    else
        snprintf(tmp, sizeof tmp, "%s\\%s", cwd, in);
    pthread_mutex_unlock(&cwd_lock);

    /* Split on separators, resolve . and .., rejoin with backslashes. */
    char *parts[256];
    int n = 0;
    char *save = NULL;
    char drive = (char)toupper((unsigned char)tmp[0]);
    for (char *tok = strtok_r(tmp + 2, "\\/", &save); tok; tok = strtok_r(NULL, "\\/", &save)) {
        if (!strcmp(tok, ".") || !*tok) continue;
        if (!strcmp(tok, "..")) { if (n) n--; continue; }
        if (n < 256) parts[n++] = tok;
    }
    size_t o = (size_t)snprintf(out, size, "%c:\\", drive);
    for (int i = 0; i < n && o < size; i++)
        o += (size_t)snprintf(out + o, size - o, "%s%s", i ? "\\" : "", parts[i]);
}

/* Find an existing entry in dir matching name case-insensitively. */
static bool match_ci(const char *dir, const char *name, char *found, size_t size)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    bool ok = false;
    for (struct dirent *e; (e = readdir(d));)
        if (!strcasecmp(e->d_name, name)) { snprintf(found, size, "%s", e->d_name); ok = true; break; }
    closedir(d);
    return ok;
}

const char *path_to_host(const char *win_path, char *out, size_t out_size, bool for_create)
{
    (void)for_create;
    char abs[2048];
    win_absolute(win_path, abs, sizeof abs);
    size_t o = (size_t)snprintf(out, out_size, "%s", g_root);
    char *save = NULL;
    for (char *tok = strtok_r(abs + 3, "\\", &save); tok; tok = strtok_r(NULL, "\\", &save)) {
        char probe[4096], found[256];
        snprintf(probe, sizeof probe, "%s/%s", out, tok);
        struct stat st;
        if (stat(probe, &st) != 0 && match_ci(out, tok, found, sizeof found)) tok = found;
        o += (size_t)snprintf(out + o, out_size - o, "/%s", tok);
    }
    return out;
}

static uint32_t errno_to_win(int e)
{
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EEXIST: return ERROR_FILE_EXISTS;
    case EACCES: case EPERM: case EISDIR: case EROFS: return ERROR_ACCESS_DENIED;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    default: return ERROR_INVALID_PARAMETER;
    }
}

/* ======================================================================== files */

struct file { int fd; bool own; };

static void file_destroy(struct object *o)
{
    struct file *f = o->data;
    if (f->own) close(f->fd);
    free(f);
}

static uint32_t std_handles[3];

static uint32_t new_file(int fd, bool own)
{
    struct file *f = malloc(sizeof *f);
    *f = (struct file){ fd, own };
    return handle_new(OBJ_FILE, f, file_destroy);
}

static struct file *get_file(uint32_t h)
{
    struct object *o = handle_get(h, OBJ_FILE);
    return o ? o->data : NULL;
}

/* COM2 is provided by comm.c */
uint32_t comm_read(uint32_t h, uint8_t *buf, uint32_t n);
uint32_t comm_write(uint32_t h, const uint8_t *buf, uint32_t n);
uint32_t comm_SetupComm(uint32_t *a), comm_GetCommState(uint32_t *a), comm_SetCommState(uint32_t *a),
    comm_SetCommMask(uint32_t *a), comm_GetCommTimeouts(uint32_t *a), comm_SetCommTimeouts(uint32_t *a),
    comm_ClearCommError(uint32_t *a), comm_GetCommModemStatus(uint32_t *a), comm_EscapeCommFunction(uint32_t *a);

static bool is_com_name(const char *name)
{
    if (!strncmp(name, "\\\\.\\", 4)) name += 4;
    return !strcasecmp(name, "COM2") || !strcasecmp(name, "COM2:");
}

static uint32_t k_CreateFileA(uint32_t *a)
{
    const char *name = S(0);
    uint32_t access = A(1), disposition = A(4);
    if (is_com_name(name)) return comm_open();

    char host[4096];
    path_to_host(name, host, sizeof host, disposition != 3);
    bool rd = access & (0x80000000u | 0x10000000u | 1), wrt = access & (0x40000000u | 0x10000000u | 2 | 4);
    int flags = (rd && wrt) ? O_RDWR : wrt ? O_WRONLY : O_RDONLY;
    struct stat st;
    bool existed = stat(host, &st) == 0;
    switch (disposition) {
    case 1: flags |= O_CREAT | O_EXCL; break;           /* CREATE_NEW */
    case 2: flags |= O_CREAT | O_TRUNC; break;          /* CREATE_ALWAYS */
    case 3: break;                                      /* OPEN_EXISTING */
    case 4: flags |= O_CREAT; break;                    /* OPEN_ALWAYS */
    case 5: flags |= O_TRUNC; break;                    /* TRUNCATE_EXISTING */
    }
    if (existed && S_ISDIR(st.st_mode)) flags = O_RDONLY | O_DIRECTORY;
    int fd = open(host, flags | O_CLOEXEC, 0644);
    LOG("CreateFileA(%s) -> %s%s", name, host, fd < 0 ? " FAILED" : "");
    if (fd < 0) {
        set_last_error(errno_to_win(errno));
        return INVALID_HANDLE_VALUE;
    }
    set_last_error(existed && (disposition == 2 || disposition == 4) ? ERROR_ALREADY_EXISTS : 0);
    return new_file(fd, true);
}

static uint32_t k_ReadFile(uint32_t *a)
{
    uint8_t *buf = G2H(uint8_t *, A(1));
    uint32_t n = A(2), done = 0;
    if (handle_get(A(0), OBJ_COMM)) {
        done = comm_read(A(0), buf, n);
        wr32(A(3), done);
        return W32_TRUE;
    }
    struct file *f = get_file(A(0));
    if (!f) { set_last_error(ERROR_INVALID_HANDLE); return W32_FALSE; }
    while (done < n) {
        ssize_t r = read(f->fd, buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { set_last_error(errno_to_win(errno)); wr32(A(3), done); return W32_FALSE; }
        if (r == 0) break;
        done += (uint32_t)r;
    }
    wr32(A(3), done);
    return W32_TRUE;
}

static uint32_t k_WriteFile(uint32_t *a)
{
    const uint8_t *buf = G2H(const uint8_t *, A(1));
    uint32_t n = A(2), done = 0;
    if (handle_get(A(0), OBJ_COMM)) {
        done = comm_write(A(0), buf, n);
        wr32(A(3), done);
        return W32_TRUE;
    }
    struct file *f = get_file(A(0));
    if (!f) { set_last_error(ERROR_INVALID_HANDLE); return W32_FALSE; }
    while (done < n) {
        ssize_t w = write(f->fd, buf + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { set_last_error(errno_to_win(errno)); wr32(A(3), done); return W32_FALSE; }
        done += (uint32_t)w;
    }
    wr32(A(3), done);
    return W32_TRUE;
}

static uint32_t k_SetFilePointer(uint32_t *a)
{
    struct file *f = get_file(A(0));
    if (!f) { set_last_error(ERROR_INVALID_HANDLE); return 0xFFFFFFFF; }
    int64_t off = (int32_t)A(1);
    if (A(2)) off = (int64_t)((uint64_t)rd32(A(2)) << 32 | A(1));
    int whence = A(3) == 0 ? SEEK_SET : A(3) == 1 ? SEEK_CUR : SEEK_END;
    off_t r = lseek(f->fd, (off_t)off, whence);
    if (r < 0) { set_last_error(ERROR_INVALID_PARAMETER); return 0xFFFFFFFF; }
    if (A(2)) wr32(A(2), (uint32_t)((uint64_t)r >> 32));
    set_last_error(0);
    return (uint32_t)r;
}

static uint32_t k_SetEndOfFile(uint32_t *a)
{
    struct file *f = get_file(A(0));
    if (!f) return W32_FALSE;
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    return ftruncate(f->fd, pos) == 0;
}

static uint32_t k_FlushFileBuffers(uint32_t *a)
{
    struct file *f = get_file(A(0));
    if (f) fsync(f->fd);
    return W32_TRUE;
}

static uint32_t k_GetFileType(uint32_t *a)
{
    struct file *f = get_file(A(0));
    if (!f) return handle_get(A(0), OBJ_COMM) ? 2 : 0;
    struct stat st;
    if (fstat(f->fd, &st) != 0) return 0;
    return S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) ? 1 : S_ISFIFO(st.st_mode) ? 3 : 2;
}

static uint32_t k_CloseHandle(uint32_t *a)
{
    for (int i = 0; i < 3; i++)
        if (A(0) == std_handles[i]) return W32_TRUE;
    if (A(0) == CURRENT_PROCESS || A(0) == CURRENT_THREAD) return W32_TRUE;
    handle_close(A(0));
    return W32_TRUE;
}

static uint32_t k_CreateDirectoryA(uint32_t *a)
{
    char host[4096];
    path_to_host(S(0), host, sizeof host, true);
    if (mkdir(host, 0755) == 0) return W32_TRUE;
    set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win(errno));
    return W32_FALSE;
}

static uint32_t k_GetCurrentDirectoryA(uint32_t *a)
{
    pthread_mutex_lock(&cwd_lock);
    uint32_t len = (uint32_t)strlen(cwd);
    if (len + 1 <= A(0)) memcpy(G2H(char *, A(1)), cwd, len + 1);
    pthread_mutex_unlock(&cwd_lock);
    return len + 1 <= A(0) ? len : len + 1;
}

static uint32_t k_SetCurrentDirectoryA(uint32_t *a)
{
    char abs[1024], host[4096];
    win_absolute(S(0), abs, sizeof abs);
    path_to_host(abs, host, sizeof host, false);
    struct stat st;
    LOG("SetCurrentDirectoryA(%s) -> %s", S(0), host);
    if (stat(host, &st) != 0 || !S_ISDIR(st.st_mode)) { set_last_error(ERROR_PATH_NOT_FOUND); return W32_FALSE; }
    pthread_mutex_lock(&cwd_lock);
    snprintf(cwd, sizeof cwd, "%s", abs);
    pthread_mutex_unlock(&cwd_lock);
    return W32_TRUE;
}

static uint32_t k_GetStdHandle(uint32_t *a)
{
    int32_t which = (int32_t)A(0);
    if (which >= -12 && which <= -10) return std_handles[-10 - which];
    return INVALID_HANDLE_VALUE;
}

static uint32_t k_SetStdHandle(uint32_t *a) { (void)a; return W32_TRUE; }
static uint32_t k_SetHandleCount(uint32_t *a) { return A(0); }

/* ======================================================================== heap */

static uint32_t k_HeapCreate(uint32_t *a) { (void)a; return handle_new(OBJ_HEAP, NULL, NULL); }
static uint32_t k_HeapDestroy(uint32_t *a) { handle_close(A(0)); return W32_TRUE; }

static uint32_t k_HeapAlloc(uint32_t *a)
{
    gptr p = guest_alloc(A(2));                  /* always zeroed, which covers HEAP_ZERO_MEMORY */
    if (!p) set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

static uint32_t k_HeapFree(uint32_t *a) { guest_free(A(2)); return W32_TRUE; }
static uint32_t k_HeapSize(uint32_t *a) { return guest_size(A(2)); }

static uint32_t k_HeapReAlloc(uint32_t *a)
{
    uint32_t flags = A(1), size = A(3);
    if (flags & 0x10) {                          /* HEAP_REALLOC_IN_PLACE_ONLY */
        if (size > guest_size(A(2))) return 0;
        return guest_realloc(A(2), size);
    }
    gptr q = guest_realloc(A(2), size);          /* growth is zeroed, which covers HEAP_ZERO_MEMORY */
    if (!q) set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return q;
}

/* ======================================================================== virtual memory */

#define PAGE 4096u
#define MEM_COMMIT  0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_FREE    0x10000u
#define MEM_PRIVATE 0x20000u
#define MEM_IMAGE   0x1000000u

struct region { uint32_t base, size; uint8_t *committed; };
static struct region regions[512];
static int nregions;
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;

static int prot_from_win(uint32_t p)
{
    if (p & 0x01) return PROT_NONE;
    if (p & (0x02)) return PROT_READ;
    if (p & (0x10 | 0x20)) return PROT_READ | PROT_EXEC;
    if (p & 0x40) return PROT_READ | PROT_WRITE | PROT_EXEC;
    return PROT_READ | PROT_WRITE;
}

static struct region *find_region(uint32_t addr)
{
    for (int i = 0; i < nregions; i++)
        if (addr >= regions[i].base && addr - regions[i].base < regions[i].size) return &regions[i];
    return NULL;
}

static uint32_t k_VirtualAlloc(uint32_t *a)
{
    uint32_t addr = A(0), size = (A(1) + PAGE - 1) & ~(PAGE - 1), type = A(2);
    int prot = prot_from_win(A(3));
    pthread_mutex_lock(&vm_lock);
    struct region *r = addr ? find_region(addr) : NULL;
    if (!r) {
        if (!(type & (MEM_RESERVE | MEM_COMMIT)) || nregions == (int)(sizeof regions / sizeof *regions)) goto fail;
        /* Reserve on a 64K boundary like Windows. */
        uint32_t base = addr & ~0xFFFFu, rsize = (addr ? addr - base : 0) + size;
        rsize = (rsize + 0xFFFFu) & ~0xFFFFu;
        base = gmem_reserve(base, rsize);
        if (!base) goto fail;
        if (!addr) addr = base;
        r = &regions[nregions++];
        *r = (struct region){ base, rsize, calloc(rsize / PAGE, 1) };
    }
    if (!addr) addr = r->base;
    if (type & MEM_COMMIT) {
        uint32_t start = addr & ~(PAGE - 1), end = (addr + size + PAGE - 1) & ~(PAGE - 1);
        if (end > r->base + r->size) goto fail;
        gmem_protect(start, end - start, prot);
        memset(r->committed + (start - r->base) / PAGE, 1, (end - start) / PAGE);
        addr = start;
    }
    pthread_mutex_unlock(&vm_lock);
    return addr;
fail:
    pthread_mutex_unlock(&vm_lock);
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

static uint32_t k_VirtualFree(uint32_t *a)
{
    uint32_t addr = A(0), size = A(1), type = A(2);
    pthread_mutex_lock(&vm_lock);
    struct region *r = find_region(addr);
    if (!r) { pthread_mutex_unlock(&vm_lock); set_last_error(ERROR_INVALID_PARAMETER); return W32_FALSE; }
    if (type & MEM_RELEASE) {
        gmem_release(r->base, r->size);
        free(r->committed);
        *r = regions[--nregions];
    } else if (type & MEM_DECOMMIT) {
        uint32_t start = addr & ~(PAGE - 1), end = size ? (addr + size + PAGE - 1) & ~(PAGE - 1) : r->base + r->size;
        gmem_decommit(start, end - start);
        memset(r->committed + (start - r->base) / PAGE, 0, (end - start) / PAGE);
    }
    pthread_mutex_unlock(&vm_lock);
    return W32_TRUE;
}

static uint32_t k_VirtualQuery(uint32_t *a)
{
    uint32_t addr = A(0) & ~(PAGE - 1), mbi = A(1);
    if (A(2) < 28) return 0;
    uint32_t base = addr, alloc_base = addr, size = PAGE, state = MEM_COMMIT, prot = 4, type = MEM_PRIVATE;
    pthread_mutex_lock(&vm_lock);
    struct region *r = find_region(addr);
    if (r) {
        uint32_t i = (addr - r->base) / PAGE, n = r->size / PAGE, j = i;
        while (j < n && r->committed[j] == r->committed[i]) j++;
        alloc_base = r->base;
        size = (j - i) * PAGE;
        state = r->committed[i] ? MEM_COMMIT : MEM_RESERVE;
        prot = r->committed[i] ? 4 : 0;
    } else if (addr >= IMAGE_BASE && addr < IMAGE_BASE + 0x253000) {
        alloc_base = IMAGE_BASE;
        size = IMAGE_BASE + 0x253000 - addr;
        prot = 0x40;
        type = MEM_IMAGE;
    }
    pthread_mutex_unlock(&vm_lock);
    wr32(mbi, base);
    wr32(mbi + 4, alloc_base);
    wr32(mbi + 8, prot ? prot : 4);
    wr32(mbi + 12, size);
    wr32(mbi + 16, state);
    wr32(mbi + 20, prot);
    wr32(mbi + 24, type);
    return 28;
}

static uint32_t k_VirtualProtect(uint32_t *a)
{
    uint32_t start = A(0) & ~(PAGE - 1), end = (A(0) + A(1) + PAGE - 1) & ~(PAGE - 1);
    bool in_image = start >= IMAGE_BASE && end <= IMAGE_BASE + 0x253000;
    if (!in_image) gmem_protect(start, end - start, prot_from_win(A(2)));
    wr32(A(3), in_image ? 0x40 : 4);
    return W32_TRUE;
}

/* ======================================================================== threads */

struct thread {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int suspend;
    bool done, terminate;
    uint32_t exit_code, tid;
    gptr start;
    uint32_t param;
    jmp_buf exit_jmp;
};

static __thread struct thread *cur_thread;
static uint32_t main_thread_handle;

static void cond_init(pthread_cond_t *cv)
{
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(cv, &ca);
    pthread_condattr_destroy(&ca);
}

static struct thread *thread_new(void)
{
    struct thread *t = calloc(1, sizeof *t);
    pthread_mutex_init(&t->lock, NULL);
    cond_init(&t->cv);
    return t;
}

static struct thread *get_thread(uint32_t h)
{
    if (h == CURRENT_THREAD) return cur_thread;
    struct object *o = handle_get(h, OBJ_THREAD);
    return o ? o->data : NULL;
}

/* Absolute CLOCK_MONOTONIC deadline for a Win32 timeout, NULL for INFINITE. */
static struct timespec *deadline(uint32_t ms, struct timespec *ts)
{
    if (ms == 0xFFFFFFFF) return NULL;
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
    return ts;
}

static void *thread_main(void *p)
{
    struct thread *t = p;
    cur_thread = t;
    thread_setup();
    pthread_mutex_lock(&t->lock);
    t->tid = (uint32_t)syscall(SYS_gettid);
    while (t->suspend > 0 && !t->terminate) pthread_cond_wait(&t->cv, &t->lock);
    bool skip = t->terminate;
    pthread_mutex_unlock(&t->lock);

    uint32_t code = t->exit_code;
    if (!skip && setjmp(t->exit_jmp) == 0)
        code = guest_call(t->start, 1, &t->param);
    else
        code = t->exit_code;
    pthread_mutex_lock(&t->lock);
    t->done = true;
    t->exit_code = code;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->lock);
    LOG("thread %u exited (%u)", t->tid, code);
    return NULL;
}

static uint32_t k_CreateThread(uint32_t *a)
{
    struct thread *t = thread_new();
    t->start = A(2);
    t->param = A(3);
    t->suspend = (A(4) & 4) ? 1 : 0;          /* CREATE_SUSPENDED */
    uint32_t h = handle_new(OBJ_THREAD, t, NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, A(1) > (8u << 20) ? A(1) : (8u << 20));
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t pt;
    if (pthread_create(&pt, &attr, thread_main, t) != 0) {
        pthread_attr_destroy(&attr);
        handle_close(h);
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    pthread_attr_destroy(&attr);
    while (!__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE)) sched_yield();
    wr32(A(5), t->tid);
    LOG("CreateThread(start=%08x param=%08x flags=%x) -> tid %u", A(2), A(3), A(4), t->tid);
    return h;
}

static void exit_current_thread(uint32_t code) __attribute__((noreturn));
static void exit_current_thread(uint32_t code)
{
    struct thread *t = cur_thread;
    if (!t || t->tid == 0) { LOG("ExitThread on main thread"); exit((int)code); }
    t->exit_code = code;
    longjmp(t->exit_jmp, 1);
}

static uint32_t k_ExitThread(uint32_t *a) { exit_current_thread(A(0)); }

static uint32_t k_SuspendThread(uint32_t *a)
{
    struct thread *t = get_thread(A(0));
    if (!t) return 0xFFFFFFFF;
    pthread_mutex_lock(&t->lock);
    uint32_t prev = (uint32_t)t->suspend++;
    if (t == cur_thread) {
        while (t->suspend > 0 && !t->terminate) pthread_cond_wait(&t->cv, &t->lock);
        bool die = t->terminate;
        pthread_mutex_unlock(&t->lock);
        if (die) exit_current_thread(t->exit_code);
        return prev;
    }
    static bool warned;
    if (!warned) { WARN("SuspendThread on another thread is only counted, not enforced"); warned = true; }
    pthread_mutex_unlock(&t->lock);
    return prev;
}

static uint32_t k_ResumeThread(uint32_t *a)
{
    struct thread *t = get_thread(A(0));
    if (!t) return 0xFFFFFFFF;
    pthread_mutex_lock(&t->lock);
    uint32_t prev = (uint32_t)t->suspend;
    if (t->suspend > 0) t->suspend--;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->lock);
    return prev;
}

static uint32_t k_TerminateThread(uint32_t *a)
{
    struct thread *t = get_thread(A(0));
    if (!t) return W32_FALSE;
    if (t == cur_thread) exit_current_thread(A(1));
    pthread_mutex_lock(&t->lock);
    t->terminate = true;
    t->exit_code = A(1);
    /* A thread parked in SuspendThread exits as soon as it wakes; a running one can't be
       stopped safely, so it is reported as finished and left to run out. */
    t->done = true;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->lock);
    return W32_TRUE;
}

static uint32_t k_GetExitCodeThread(uint32_t *a)
{
    struct thread *t = get_thread(A(0));
    if (!t) return W32_FALSE;
    pthread_mutex_lock(&t->lock);
    wr32(A(1), t->done ? t->exit_code : 259);       /* STILL_ACTIVE */
    pthread_mutex_unlock(&t->lock);
    return W32_TRUE;
}

static uint32_t k_SetThreadPriority(uint32_t *a) { (void)a; return W32_TRUE; }
static uint32_t k_GetCurrentThreadId(uint32_t *a) { (void)a; return (uint32_t)syscall(SYS_gettid); }
static uint32_t k_GetCurrentProcessId(uint32_t *a) { (void)a; return (uint32_t)getpid(); }
static uint32_t k_GetCurrentProcess(uint32_t *a) { (void)a; return CURRENT_PROCESS; }

static uint32_t k_ExitProcess(uint32_t *a)
{
    LOG("ExitProcess(%u)", A(0));
    fflush(NULL);
    _exit((int)A(0));
}

static uint32_t k_TerminateProcess(uint32_t *a)
{
    LOG("TerminateProcess(%u)", A(1));
    fflush(NULL);
    _exit((int)A(1));
}

static uint32_t k_Sleep(uint32_t *a)
{
    if (A(0) == 0) { sched_yield(); return 0; }
    struct timespec ts = { A(0) / 1000, (long)(A(0) % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
    return 0;
}

/* ======================================================================== mutexes / waits */

struct mutex {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    uint32_t owner;
    int count;
};

static uint32_t k_CreateMutexA(uint32_t *a)
{
    struct mutex *m = calloc(1, sizeof *m);
    pthread_mutex_init(&m->lock, NULL);
    cond_init(&m->cv);
    if (A(1)) { m->owner = (uint32_t)syscall(SYS_gettid); m->count = 1; }
    set_last_error(0);
    return handle_new(OBJ_MUTEX, m, NULL);
}

static uint32_t k_ReleaseMutex(uint32_t *a)
{
    struct object *o = handle_get(A(0), OBJ_MUTEX);
    if (!o) return W32_FALSE;
    struct mutex *m = o->data;
    uint32_t me = (uint32_t)syscall(SYS_gettid);
    pthread_mutex_lock(&m->lock);
    if (m->owner != me) { pthread_mutex_unlock(&m->lock); set_last_error(288); return W32_FALSE; }
    if (--m->count == 0) { m->owner = 0; pthread_cond_signal(&m->cv); }
    pthread_mutex_unlock(&m->lock);
    return W32_TRUE;
}

#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT  0x102
#define WAIT_FAILED   0xFFFFFFFF

static uint32_t k_WaitForSingleObject(uint32_t *a)
{
    struct timespec ts, *dl = deadline(A(1), &ts);
    struct object *o;
    if ((o = handle_get(A(0), OBJ_MUTEX))) {
        struct mutex *m = o->data;
        uint32_t me = (uint32_t)syscall(SYS_gettid);
        pthread_mutex_lock(&m->lock);
        while (m->owner && m->owner != me) {
            if (!dl) pthread_cond_wait(&m->cv, &m->lock);
            else if (pthread_cond_timedwait(&m->cv, &m->lock, dl) == ETIMEDOUT) {
                pthread_mutex_unlock(&m->lock);
                return WAIT_TIMEOUT;
            }
        }
        m->owner = me;
        m->count++;
        pthread_mutex_unlock(&m->lock);
        return WAIT_OBJECT_0;
    }
    struct thread *t = get_thread(A(0));
    if (t) {
        pthread_mutex_lock(&t->lock);
        while (!t->done) {
            if (!dl) pthread_cond_wait(&t->cv, &t->lock);
            else if (pthread_cond_timedwait(&t->cv, &t->lock, dl) == ETIMEDOUT) {
                pthread_mutex_unlock(&t->lock);
                return WAIT_TIMEOUT;
            }
        }
        pthread_mutex_unlock(&t->lock);
        return WAIT_OBJECT_0;
    }
    WARN("WaitForSingleObject on unsupported handle %x", A(0));
    set_last_error(ERROR_INVALID_HANDLE);
    return WAIT_FAILED;
}

/* CRITICAL_SECTION: the guest struct's first dword holds an index into this table. */
static pthread_mutex_t *cs_table[4096];
static pthread_mutex_t cs_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *cs_get(gptr cs)
{
    uint32_t idx = rd32(cs);
    if (idx && idx < 4096 && cs_table[idx]) return cs_table[idx];
    pthread_mutex_lock(&cs_alloc_lock);
    idx = rd32(cs);
    if (!(idx && idx < 4096 && cs_table[idx])) {
        for (idx = 1; idx < 4096 && cs_table[idx]; idx++) {}
        if (idx == 4096) w32_fatal("out of critical sections");
        pthread_mutexattr_t ma;
        pthread_mutexattr_init(&ma);
        pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
        cs_table[idx] = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(cs_table[idx], &ma);
        pthread_mutexattr_destroy(&ma);
        wr32(cs, idx);
    }
    pthread_mutex_unlock(&cs_alloc_lock);
    return cs_table[idx];
}

static uint32_t k_InitializeCriticalSection(uint32_t *a)
{
    memset(G2H(void *, A(0)), 0, 24);
    cs_get(A(0));
    return 0;
}

static uint32_t k_DeleteCriticalSection(uint32_t *a)
{
    uint32_t idx = rd32(A(0));
    pthread_mutex_lock(&cs_alloc_lock);
    if (idx && idx < 4096 && cs_table[idx]) {
        pthread_mutex_destroy(cs_table[idx]);
        free(cs_table[idx]);
        cs_table[idx] = NULL;
    }
    wr32(A(0), 0);
    pthread_mutex_unlock(&cs_alloc_lock);
    return 0;
}

static uint32_t k_EnterCriticalSection(uint32_t *a) { pthread_mutex_lock(cs_get(A(0))); return 0; }
static uint32_t k_LeaveCriticalSection(uint32_t *a) { pthread_mutex_unlock(cs_get(A(0))); return 0; }

static uint32_t k_InterlockedExchange(uint32_t *a)
{
    return __atomic_exchange_n(G2H(uint32_t *, A(0)), A(1), __ATOMIC_SEQ_CST);
}

/* ======================================================================== TLS */

static bool tls_used[64];
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t k_TlsAlloc(uint32_t *a)
{
    (void)a;
    pthread_mutex_lock(&tls_lock);
    for (uint32_t i = 0; i < 64; i++)
        if (!tls_used[i]) { tls_used[i] = true; pthread_mutex_unlock(&tls_lock); return i; }
    pthread_mutex_unlock(&tls_lock);
    return 0xFFFFFFFF;
}

static uint32_t k_TlsFree(uint32_t *a) { if (A(0) < 64) tls_used[A(0)] = false; return W32_TRUE; }
static uint32_t k_TlsGetValue(uint32_t *a) { set_last_error(0); return A(0) < 64 ? teb_tls_slots()[A(0)] : 0; }
static uint32_t k_TlsSetValue(uint32_t *a) { if (A(0) >= 64) return W32_FALSE; teb_tls_slots()[A(0)] = A(1); return W32_TRUE; }

static uint32_t k_GetLastError(uint32_t *a) { (void)a; return get_last_error(); }
static uint32_t k_SetLastError(uint32_t *a) { set_last_error(A(0)); return 0; }

/* ======================================================================== time */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t k_GetTickCount(uint32_t *a) { (void)a; return (uint32_t)(mono_ns() / 1000000ull); }

static uint32_t k_QueryPerformanceCounter(uint32_t *a)
{
    uint64_t v = mono_ns() / 100;              /* 10 MHz, like current Windows */
    wr32(A(0), (uint32_t)v);
    wr32(A(0) + 4, (uint32_t)(v >> 32));
    return W32_TRUE;
}

static uint32_t k_QueryPerformanceFrequency(uint32_t *a)
{
    wr32(A(0), 10000000);
    wr32(A(0) + 4, 0);
    return W32_TRUE;
}

static uint32_t k_GetSystemTimeAsFileTime(uint32_t *a)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ft = (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100 + 116444736000000000ull;
    wr32(A(0), (uint32_t)ft);
    wr32(A(0) + 4, (uint32_t)(ft >> 32));
    return 0;
}

/* ======================================================================== process / environment */

static uint32_t k_GetCommandLineA(uint32_t *a)
{
    (void)a;
    static gptr cmdline;
    if (!cmdline) cmdline = guest_strdup("\"C:\\game.exe\"");
    return cmdline;
}

/* Empty environment blocks (two terminating NULs, as ANSI and as UTF-16). */
static gptr empty_env(void)
{
    static gptr env;
    if (!env) env = guest_alloc(8);
    return env;
}
static uint32_t k_GetEnvironmentStrings(uint32_t *a) { (void)a; return empty_env(); }
static uint32_t k_GetEnvironmentStringsW(uint32_t *a) { (void)a; return empty_env(); }
static uint32_t k_FreeEnvironmentStrings(uint32_t *a) { (void)a; return W32_TRUE; }
static uint32_t k_SetEnvironmentVariableA(uint32_t *a) { (void)a; return W32_TRUE; }

static uint32_t k_GetStartupInfoA(uint32_t *a)
{
    memset(G2H(void *, A(0)), 0, 68);
    wr32(A(0), 68);
    return 0;
}

static uint32_t k_GetVersionExA(uint32_t *a)
{
    gptr v = A(0);
    wr32(v + 4, 5);           /* Windows XP 5.1.2600 */
    wr32(v + 8, 1);
    wr32(v + 12, 2600);
    wr32(v + 16, 2);          /* VER_PLATFORM_WIN32_NT */
    memset(G2H(void *, v + 20), 0, 128);
    return W32_TRUE;
}

static uint32_t k_GetSystemInfo(uint32_t *a)
{
    gptr s = A(0);
    memset(G2H(void *, s), 0, 36);
    wr32(s + 4, PAGE);
    wr32(s + 8, 0x10000);
    wr32(s + 12, 0x7FFEFFFF);
    wr32(s + 16, 1);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    wr32(s + 20, n > 0 ? (uint32_t)n : 1);
    wr32(s + 24, 586);
    wr32(s + 28, 0x10000);
    wr16(s + 32, 6);
    return 0;
}

static const char *const module_names[] = { "kernel32.dll", "user32.dll", "gdi32.dll", "winmm.dll", "opengl32.dll", "dsound.dll" };
static uint32_t module_handles[6];

static int module_index(const char *name)
{
    for (int i = 0; i < 6; i++) {
        size_t n = strlen(module_names[i]) - 4;
        if (!strcasecmp(name, module_names[i]) || (!strncasecmp(name, module_names[i], n) && !name[n]))
            return i;
    }
    return -1;
}

static uint32_t module_handle(int i)
{
    if (!module_handles[i]) module_handles[i] = handle_new(OBJ_MODULE, (void *)module_names[i], NULL);
    return module_handles[i];
}

static uint32_t k_GetModuleHandleA(uint32_t *a)
{
    if (!A(0)) return IMAGE_BASE;
    if (!strcasecmp(S(0), "game.exe")) return IMAGE_BASE;
    int i = module_index(S(0));
    if (i < 0) { set_last_error(126); return 0; }
    return module_handle(i);
}

static uint32_t k_LoadLibraryA(uint32_t *a)
{
    int i = module_index(S(0));
    LOG("LoadLibraryA(%s)%s", S(0), i < 0 ? " -> not available" : "");
    if (i < 0) { set_last_error(126); return 0; }
    return module_handle(i);
}

static uint32_t k_GetProcAddress(uint32_t *a)
{
    struct object *o = handle_get(A(0), OBJ_MODULE);
    bool by_ordinal = A(1) < 0x10000;
    gptr fn = o ? w32_resolve(o->data, by_ordinal ? NULL : S(1), by_ordinal ? (int)A(1) : 0) : 0;
    LOG("GetProcAddress(%s, %s) -> %08x", o ? (const char *)o->data : "?", by_ordinal ? "#ordinal" : S(1), fn);
    if (!fn) set_last_error(127);
    return fn;
}

static uint32_t k_GetModuleFileNameA(uint32_t *a)
{
    const char *name = "C:\\game.exe";
    uint32_t len = (uint32_t)strlen(name);
    if (!A(2)) return 0;
    uint32_t n = len < A(2) - 1 ? len : A(2) - 1;
    memcpy(G2H(char *, A(1)), name, n);
    G2H(char *, A(1))[n] = 0;
    return n;
}

static uint32_t k_OutputDebugStringA(uint32_t *a) { WARN("debug: %s", S(0)); return 0; }

static uint32_t k_UnhandledExceptionFilter(uint32_t *a)
{
    gptr rec = rd32(A(0));
    w32_fatal("unhandled exception %08x at %08x", rd32(rec), rd32(rec + 12));
}

static gptr unhandled_filter;
static uint32_t k_SetUnhandledExceptionFilter(uint32_t *a) { gptr prev = unhandled_filter; unhandled_filter = A(0); return prev; }

static uint32_t k_RaiseException(uint32_t *a)
{
    w32_fatal("RaiseException(%08x) - SEH dispatch is not implemented", A(0));
}

/* Only reached if an exception was dispatched; pops SEH frames down to the target. */
static uint32_t k_RtlUnwind(uint32_t *a)
{
    gptr frame = teb_exception_list();
    while (frame != 0xFFFFFFFF && frame != A(0)) frame = rd32(frame);
    teb_set_exception_list(frame);
    return A(3);
}

/* ======================================================================== locale / strings (codepage 1252) */

static uint32_t k_GetACP(uint32_t *a) { (void)a; return 1252; }
static uint32_t k_GetOEMCP(uint32_t *a) { (void)a; return 437; }

static uint32_t k_GetCPInfo(uint32_t *a)
{
    gptr p = A(1);
    memset(G2H(void *, p), 0, 20);
    wr32(p, 1);                 /* MaxCharSize */
    G2H(uint8_t *, p)[4] = '?'; /* DefaultChar */
    return W32_TRUE;
}

static uint32_t k_GetLocaleInfoA(uint32_t *a)
{
    const char *v;
    switch (A(1) & 0xFFFF) {
    case 0x1004: v = "1252"; break;    /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x000B: v = "437"; break;     /* LOCALE_IDEFAULTCODEPAGE */
    case 0x0003: v = "ENU"; break;     /* LOCALE_SABBREVLANGNAME */
    case 0x0007: v = "USA"; break;     /* LOCALE_SABBREVCTRYNAME */
    default: return 0;
    }
    uint32_t len = (uint32_t)strlen(v) + 1;
    if (!A(3)) return len;
    if (A(3) < len) return 0;
    memcpy(G2H(char *, A(2)), v, len);
    return len;
}

static uint32_t k_MultiByteToWideChar(uint32_t *a)
{
    const uint8_t *src = G2H(const uint8_t *, A(2));
    int32_t n = (int32_t)A(3);
    if (n < 0) n = (int32_t)strlen((const char *)src) + 1;
    if (!A(5)) return (uint32_t)n;
    if ((uint32_t)n > A(5)) { set_last_error(122); return 0; }
    for (int32_t i = 0; i < n; i++) wr16(A(4) + 2u * (uint32_t)i, src[i]);
    return (uint32_t)n;
}

static uint32_t k_WideCharToMultiByte(uint32_t *a)
{
    gptr src = A(2);
    int32_t n = (int32_t)A(3);
    if (n < 0) { n = 0; while (rd16(src + 2u * (uint32_t)n)) n++; n++; }
    if (!A(5)) return (uint32_t)n;
    if ((uint32_t)n > A(5)) { set_last_error(122); return 0; }
    uint8_t *dst = G2H(uint8_t *, A(4));
    for (int32_t i = 0; i < n; i++) {
        uint16_t c = rd16(src + 2u * (uint32_t)i);
        dst[i] = c < 256 ? (uint8_t)c : '?';
    }
    if (A(7)) wr32(A(7), 0);
    return (uint32_t)n;
}

static uint32_t k_LCMapStringA(uint32_t *a)
{
    const uint8_t *src = G2H(const uint8_t *, A(2));
    int32_t n = (int32_t)A(3);
    if (n < 0) n = (int32_t)strlen((const char *)src) + 1;
    if (!A(5)) return (uint32_t)n;
    if ((uint32_t)n > A(5)) return 0;
    uint8_t *dst = G2H(uint8_t *, A(4));
    for (int32_t i = 0; i < n; i++)
        dst[i] = (A(1) & 0x100) ? (uint8_t)tolower(src[i]) : (A(1) & 0x200) ? (uint8_t)toupper(src[i]) : src[i];
    return (uint32_t)n;
}

static uint32_t k_LCMapStringW(uint32_t *a)
{
    gptr src = A(2);
    int32_t n = (int32_t)A(3);
    if (n < 0) { n = 0; while (rd16(src + 2u * (uint32_t)n)) n++; n++; }
    if (!A(5)) return (uint32_t)n;
    if ((uint32_t)n > A(5)) return 0;
    for (int32_t i = 0; i < n; i++) {
        uint16_t c = rd16(src + 2u * (uint32_t)i);
        if (c < 128) c = (A(1) & 0x100) ? (uint16_t)tolower(c) : (A(1) & 0x200) ? (uint16_t)toupper(c) : c;
        wr16(A(4) + 2u * (uint32_t)i, c);
    }
    return (uint32_t)n;
}

static uint16_t ctype1(unsigned c)
{
    if (c > 255) return 0x100;
    uint16_t t = 0;
    if (isupper(c)) t |= 0x001;
    if (islower(c)) t |= 0x002;
    if (isdigit(c)) t |= 0x004;
    if (isspace(c)) t |= 0x008;
    if (ispunct(c)) t |= 0x010;
    if (iscntrl(c)) t |= 0x020;
    if (c == ' ' || c == '\t') t |= 0x040;
    if (isxdigit(c)) t |= 0x080;
    if (isalpha(c)) t |= 0x100;
    return t;
}

static uint32_t k_GetStringTypeA(uint32_t *a)
{
    const uint8_t *src = G2H(const uint8_t *, A(2));
    int32_t n = (int32_t)A(3);
    if (n < 0) n = (int32_t)strlen((const char *)src) + 1;
    for (int32_t i = 0; i < n; i++) wr16(A(4) + 2u * (uint32_t)i, A(1) == 1 ? ctype1(src[i]) : 0);
    return W32_TRUE;
}

static uint32_t k_GetStringTypeW(uint32_t *a)
{
    int32_t n = (int32_t)A(2);
    if (n < 0) { n = 0; while (rd16(A(1) + 2u * (uint32_t)n)) n++; n++; }
    for (int32_t i = 0; i < n; i++) wr16(A(3) + 2u * (uint32_t)i, A(0) == 1 ? ctype1(rd16(A(1) + 2u * (uint32_t)i)) : 0);
    return W32_TRUE;
}

/* ======================================================================== table */

__attribute__((constructor)) static void kernel32_init(void)
{
    for (int i = 0; i < 3; i++) std_handles[i] = new_file(i, false);
    cur_thread = thread_new();
    main_thread_handle = handle_new(OBJ_THREAD, cur_thread, NULL);
}

const struct w32_export kernel32_exports[] = {
    { "CreateFileA", 7, k_CreateFileA, 0 },
    { "ReadFile", 5, k_ReadFile, 0 },
    { "WriteFile", 5, k_WriteFile, 0 },
    { "SetFilePointer", 4, k_SetFilePointer, 0 },
    { "SetEndOfFile", 1, k_SetEndOfFile, 0 },
    { "FlushFileBuffers", 1, k_FlushFileBuffers, 0 },
    { "GetFileType", 1, k_GetFileType, 0 },
    { "CloseHandle", 1, k_CloseHandle, 0 },
    { "CreateDirectoryA", 2, k_CreateDirectoryA, 0 },
    { "GetCurrentDirectoryA", 2, k_GetCurrentDirectoryA, 0 },
    { "SetCurrentDirectoryA", 1, k_SetCurrentDirectoryA, 0 },
    { "GetStdHandle", 1, k_GetStdHandle, 0 },
    { "SetStdHandle", 2, k_SetStdHandle, 0 },
    { "SetHandleCount", 1, k_SetHandleCount, 0 },
    { "HeapCreate", 3, k_HeapCreate, 0 },
    { "HeapDestroy", 1, k_HeapDestroy, 0 },
    { "HeapAlloc", 3, k_HeapAlloc, 0 },
    { "HeapFree", 3, k_HeapFree, 0 },
    { "HeapSize", 3, k_HeapSize, 0 },
    { "HeapReAlloc", 4, k_HeapReAlloc, 0 },
    { "VirtualAlloc", 4, k_VirtualAlloc, 0 },
    { "VirtualFree", 3, k_VirtualFree, 0 },
    { "VirtualQuery", 3, k_VirtualQuery, 0 },
    { "VirtualProtect", 4, k_VirtualProtect, 0 },
    { "CreateThread", 6, k_CreateThread, 0 },
    { "ExitThread", 1, k_ExitThread, 0 },
    { "SuspendThread", 1, k_SuspendThread, 0 },
    { "ResumeThread", 1, k_ResumeThread, 0 },
    { "TerminateThread", 2, k_TerminateThread, 0 },
    { "GetExitCodeThread", 2, k_GetExitCodeThread, 0 },
    { "SetThreadPriority", 2, k_SetThreadPriority, 0 },
    { "GetCurrentThreadId", 0, k_GetCurrentThreadId, 0 },
    { "GetCurrentProcessId", 0, k_GetCurrentProcessId, 0 },
    { "GetCurrentProcess", 0, k_GetCurrentProcess, 0 },
    { "ExitProcess", 1, k_ExitProcess, 0 },
    { "TerminateProcess", 2, k_TerminateProcess, 0 },
    { "Sleep", 1, k_Sleep, 0 },
    { "CreateMutexA", 3, k_CreateMutexA, 0 },
    { "ReleaseMutex", 1, k_ReleaseMutex, 0 },
    { "WaitForSingleObject", 2, k_WaitForSingleObject, 0 },
    { "InitializeCriticalSection", 1, k_InitializeCriticalSection, 0 },
    { "DeleteCriticalSection", 1, k_DeleteCriticalSection, 0 },
    { "EnterCriticalSection", 1, k_EnterCriticalSection, 0 },
    { "LeaveCriticalSection", 1, k_LeaveCriticalSection, 0 },
    { "InterlockedExchange", 2, k_InterlockedExchange, 0 },
    { "TlsAlloc", 0, k_TlsAlloc, 0 },
    { "TlsFree", 1, k_TlsFree, 0 },
    { "TlsGetValue", 1, k_TlsGetValue, 0 },
    { "TlsSetValue", 2, k_TlsSetValue, 0 },
    { "GetLastError", 0, k_GetLastError, 0 },
    { "SetLastError", 1, k_SetLastError, 0 },
    { "GetTickCount", 0, k_GetTickCount, 0 },
    { "QueryPerformanceCounter", 1, k_QueryPerformanceCounter, 0 },
    { "QueryPerformanceFrequency", 1, k_QueryPerformanceFrequency, 0 },
    { "GetSystemTimeAsFileTime", 1, k_GetSystemTimeAsFileTime, 0 },
    { "GetCommandLineA", 0, k_GetCommandLineA, 0 },
    { "GetEnvironmentStrings", 0, k_GetEnvironmentStrings, 0 },
    { "GetEnvironmentStringsW", 0, k_GetEnvironmentStringsW, 0 },
    { "FreeEnvironmentStringsA", 1, k_FreeEnvironmentStrings, 0 },
    { "FreeEnvironmentStringsW", 1, k_FreeEnvironmentStrings, 0 },
    { "SetEnvironmentVariableA", 2, k_SetEnvironmentVariableA, 0 },
    { "GetStartupInfoA", 1, k_GetStartupInfoA, 0 },
    { "GetVersionExA", 1, k_GetVersionExA, 0 },
    { "GetSystemInfo", 1, k_GetSystemInfo, 0 },
    { "GetModuleHandleA", 1, k_GetModuleHandleA, 0 },
    { "LoadLibraryA", 1, k_LoadLibraryA, 0 },
    { "GetProcAddress", 2, k_GetProcAddress, 0 },
    { "GetModuleFileNameA", 3, k_GetModuleFileNameA, 0 },
    { "OutputDebugStringA", 1, k_OutputDebugStringA, 0 },
    { "UnhandledExceptionFilter", 1, k_UnhandledExceptionFilter, 0 },
    { "SetUnhandledExceptionFilter", 1, k_SetUnhandledExceptionFilter, 0 },
    { "RaiseException", 4, k_RaiseException, 0 },
    { "RtlUnwind", 4, k_RtlUnwind, 0 },
    { "GetACP", 0, k_GetACP, 0 },
    { "GetOEMCP", 0, k_GetOEMCP, 0 },
    { "GetCPInfo", 2, k_GetCPInfo, 0 },
    { "GetLocaleInfoA", 4, k_GetLocaleInfoA, 0 },
    { "MultiByteToWideChar", 6, k_MultiByteToWideChar, 0 },
    { "WideCharToMultiByte", 8, k_WideCharToMultiByte, 0 },
    { "LCMapStringA", 6, k_LCMapStringA, 0 },
    { "LCMapStringW", 6, k_LCMapStringW, 0 },
    { "GetStringTypeA", 5, k_GetStringTypeA, 0 },
    { "GetStringTypeW", 4, k_GetStringTypeW, 0 },
    { "SetupComm", 3, comm_SetupComm, 0 },
    { "GetCommState", 2, comm_GetCommState, 0 },
    { "SetCommState", 2, comm_SetCommState, 0 },
    { "SetCommMask", 2, comm_SetCommMask, 0 },
    { "GetCommTimeouts", 2, comm_GetCommTimeouts, 0 },
    { "SetCommTimeouts", 2, comm_SetCommTimeouts, 0 },
    { "ClearCommError", 3, comm_ClearCommError, 0 },
    { "GetCommModemStatus", 2, comm_GetCommModemStatus, 0 },
    { "EscapeCommFunction", 2, comm_EscapeCommFunction, 0 },
    { NULL, 0, NULL, 0 },
};
