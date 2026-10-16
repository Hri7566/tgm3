/* JVS I/O board emulation (the protocol half of what typex_io.dll does on Windows). */
#ifndef JVS_H
#define JVS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JVS_SYNC      0xE0
#define JVS_MARK      0xD0
#define JVS_BROADCAST 0xFF
#define JVS_MASTER    0x00

#define JVS_PLAYERS   2
#define JVS_BUTTONS   6
#define JVS_SLOTS     2

/* Identity reported by the original typex_io.dll (see typex_io.log); TGM3 accepts it. */
#define JVS_IDENTITY "NAMCO LTD.;I/O PCB-1000;ver1.0;for domestic only,no analog input"

struct jvs_player {
    bool start, service, coin;
    bool up, down, left, right;
    bool button[JVS_BUTTONS];
};

struct jvs_inputs {
    bool test;
    struct jvs_player p[JVS_PLAYERS];
};

struct jvs_board {
    uint8_t address;                 /* 0 = not yet assigned */
    uint16_t coins[JVS_SLOTS];
    struct jvs_inputs in;

    /* receive state */
    int rx_pos;                      /* -1 = waiting for sync, 0 = dest, 1 = len, 2.. = data */
    bool rx_escape;
    uint8_t rx_dest, rx_len;
    uint8_t rx_data[256];

    /* last encoded reply, for the 0x2F retransmit command */
    uint8_t last_tx[2 * 260 + 1];
    size_t last_tx_len;

    void (*log)(const char *fmt, ...);
};

void jvs_init(struct jvs_board *b, void (*log)(const char *fmt, ...));

/* Latch the current inputs; rising edges on coin buttons add credits. */
void jvs_set_inputs(struct jvs_board *b, const struct jvs_inputs *in);

/* Feed bytes from the host. Replies are appended to out; returns the number of bytes written. */
size_t jvs_feed(struct jvs_board *b, const uint8_t *data, size_t n, uint8_t *out, size_t out_cap);

#endif
