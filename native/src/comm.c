/* COM2: the JVS I/O board, emulated in-process (what typex_io.dll + jvsd do elsewhere). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "jvs.h"
#include "w32.h"

void input_snapshot(struct jvs_inputs *out);

static struct jvs_board board;
static uint8_t rxq[8192];
static size_t rxlen;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t dcb[28];
static uint32_t timeouts[5];

static void jvs_log(const char *fmt, ...)
{
    if (!g_verbose) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    w32_log("jvs: %s", buf);
}

void comm_init(void)
{
    jvs_init(&board, jvs_log);
    memset(dcb, 0, sizeof dcb);
    uint32_t len = sizeof dcb, baud = 115200;
    memcpy(dcb, &len, 4);
    memcpy(dcb + 4, &baud, 4);
}

uint32_t comm_open(void)
{
    pthread_mutex_lock(&lock);
    jvs_init(&board, jvs_log);
    rxlen = 0;
    pthread_mutex_unlock(&lock);
    LOG("opened COM2 (JVS board)");
    return handle_new(OBJ_COMM, NULL, NULL);
}

uint32_t comm_write(uint32_t h, const uint8_t *buf, uint32_t n)
{
    (void)h;
    struct jvs_inputs in;
    input_snapshot(&in);
    pthread_mutex_lock(&lock);
    jvs_set_inputs(&board, &in);
    rxlen += jvs_feed(&board, buf, n, rxq + rxlen, sizeof rxq - rxlen);
    pthread_mutex_unlock(&lock);
    return n;
}

uint32_t comm_read(uint32_t h, uint8_t *buf, uint32_t n)
{
    (void)h;
    pthread_mutex_lock(&lock);
    if (!rxlen) {
        /* Nothing will arrive (replies are produced synchronously by writes): behave like the
           read timeout expiring on the real port, capped so a missing reply can't stall. */
        pthread_mutex_unlock(&lock);
        uint32_t ms = timeouts[2] ? timeouts[2] : 1;
        struct timespec ts = { 0, (long)(ms > 50 ? 50 : ms) * 1000000L };
        nanosleep(&ts, NULL);
        return 0;
    }
    uint32_t k = n < rxlen ? n : (uint32_t)rxlen;
    memcpy(buf, rxq, k);
    memmove(rxq, rxq + k, rxlen - k);
    rxlen -= k;
    pthread_mutex_unlock(&lock);
    return k;
}

#define A(i) (a[i])

uint32_t comm_SetupComm(uint32_t *a) { (void)a; return W32_TRUE; }
uint32_t comm_SetCommMask(uint32_t *a) { (void)a; return W32_TRUE; }
uint32_t comm_EscapeCommFunction(uint32_t *a) { (void)a; return W32_TRUE; }

uint32_t comm_GetCommState(uint32_t *a) { memcpy(G2H(void *, A(1)), dcb, sizeof dcb); return W32_TRUE; }
uint32_t comm_SetCommState(uint32_t *a) { memcpy(dcb, G2H(void *, A(1)), sizeof dcb); return W32_TRUE; }
uint32_t comm_GetCommTimeouts(uint32_t *a) { memcpy(G2H(void *, A(1)), timeouts, sizeof timeouts); return W32_TRUE; }
uint32_t comm_SetCommTimeouts(uint32_t *a) { memcpy(timeouts, G2H(void *, A(1)), sizeof timeouts); return W32_TRUE; }

uint32_t comm_ClearCommError(uint32_t *a)
{
    wr32(A(1), 0);
    if (A(2)) {
        pthread_mutex_lock(&lock);
        wr32(A(2), 0);
        wr32(A(2) + 4, (uint32_t)rxlen);   /* cbInQue */
        wr32(A(2) + 8, 0);
        pthread_mutex_unlock(&lock);
    }
    return W32_TRUE;
}

/* The JVS sense line, read by the game as CTS: high once the board has an address. */
uint32_t comm_GetCommModemStatus(uint32_t *a)
{
    pthread_mutex_lock(&lock);
    wr32(A(1), 0x20 | (board.address ? 0x10 : 0));     /* MS_DSR_ON | MS_CTS_ON */
    pthread_mutex_unlock(&lock);
    return W32_TRUE;
}
