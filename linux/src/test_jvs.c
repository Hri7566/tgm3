/* Replays the host side of TGM3's JVS bring-up (as seen in typex_io.log) against the emulator. */
#include <stdio.h>
#include <string.h>

#include "jvs.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* Encode a host packet (escaping + checksum). */
static size_t packet(uint8_t *out, uint8_t dest, const uint8_t *data, size_t n)
{
    size_t o = 0;
    uint8_t len = (uint8_t)(n + 1), sum = dest + len;
    uint8_t raw[260];
    raw[0] = dest;
    raw[1] = len;
    memcpy(raw + 2, data, n);
    for (size_t i = 0; i < n; i++) sum += data[i];
    raw[2 + n] = sum;
    out[o++] = JVS_SYNC;
    for (size_t i = 0; i < n + 3; i++) {
        if (raw[i] == JVS_SYNC || raw[i] == JVS_MARK) { out[o++] = JVS_MARK; out[o++] = raw[i] - 1; }
        else out[o++] = raw[i];
    }
    return o;
}

/* Decode one reply into body (status + reports); returns body length or -1. */
static int reply(const uint8_t *in, size_t n, uint8_t *body)
{
    uint8_t raw[300];
    size_t r = 0;
    if (n < 4 || in[0] != JVS_SYNC) return -1;
    for (size_t i = 1; i < n; i++)
        raw[r++] = in[i] == JVS_MARK ? (uint8_t)(in[++i] + 1) : in[i];
    if (raw[0] != JVS_MASTER || raw[1] + 2u != r) return -1;
    uint8_t sum = 0;
    for (size_t i = 0; i < r - 1; i++) sum += raw[i];
    if (sum != raw[r - 1]) return -1;
    memcpy(body, raw + 2, r - 3);
    return (int)(r - 3);
}

static int roundtrip(struct jvs_board *b, uint8_t dest, const uint8_t *data, size_t n, uint8_t *body)
{
    uint8_t pkt[600], out[1200];
    size_t len = jvs_feed(b, pkt, packet(pkt, dest, data, n), out, sizeof out);
    return len ? reply(out, len, body) : 0;
}

int main(void)
{
    struct jvs_board b;
    uint8_t body[300];
    jvs_init(&b, NULL);

    /* E0 FF 03 F0 D9 CB: reset, no reply. */
    CHECK(roundtrip(&b, 0xFF, (const uint8_t[]){ 0xF0, 0xD9 }, 2, body) == 0);
    CHECK(b.address == 0);

    /* Address assignment. */
    CHECK(roundtrip(&b, 0xFF, (const uint8_t[]){ 0xF1, 0x01 }, 2, body) == 2);
    CHECK(body[0] == 1 && body[1] == 1 && b.address == 1);

    /* Second broadcast address is for the next board in the chain: ignore. */
    CHECK(roundtrip(&b, 0xFF, (const uint8_t[]){ 0xF1, 0x02 }, 2, body) == 0);
    CHECK(b.address == 1);

    /* Packets for another board are ignored. */
    CHECK(roundtrip(&b, 0x02, (const uint8_t[]){ 0x10 }, 1, body) == 0);

    int n = roundtrip(&b, 0x01, (const uint8_t[]){ 0x10 }, 1, body);
    CHECK(n == 2 + (int)sizeof JVS_IDENTITY);
    CHECK(!memcmp(body + 2, JVS_IDENTITY, sizeof JVS_IDENTITY));

    /* Versions in one multi-command packet. */
    n = roundtrip(&b, 0x01, (const uint8_t[]){ 0x11, 0x12, 0x13 }, 3, body);
    CHECK(n == 7 && body[1] == 1 && body[2] == 0x13 && body[4] == 0x30 && body[6] == 0x10);

    n = roundtrip(&b, 0x01, (const uint8_t[]){ 0x14 }, 1, body);
    CHECK(n == 11 && body[2] == 0x01 && body[3] == 2 && body[4] == 14 && body[6] == 0x02 && body[10] == 0x00);

    /* Switches: test + P1 start/up/b1 + P2 b3. */
    struct jvs_inputs in = { 0 };
    in.test = true;
    in.p[0].start = in.p[0].up = in.p[0].button[0] = true;
    in.p[1].button[2] = true;
    jvs_set_inputs(&b, &in);
    n = roundtrip(&b, 0x01, (const uint8_t[]){ 0x20, 2, 2 }, 3, body);
    CHECK(n == 7);
    CHECK(body[2] == 0x80);
    CHECK(body[3] == (0x80 | 0x20 | 0x02) && body[4] == 0x00);
    CHECK(body[5] == 0x00 && body[6] == 0x80);

    /* Coin edge counts once while held; release + press counts again. */
    memset(&in, 0, sizeof in);
    in.p[0].coin = true;
    jvs_set_inputs(&b, &in);
    jvs_set_inputs(&b, &in);
    in.p[0].coin = false;
    jvs_set_inputs(&b, &in);
    in.p[0].coin = true;
    jvs_set_inputs(&b, &in);
    n = roundtrip(&b, 0x01, (const uint8_t[]){ 0x21, 2 }, 2, body);
    CHECK(n == 6 && body[2] == 0 && body[3] == 2 && body[5] == 0);

    /* "remove coin 01 0001" from the log. */
    CHECK(roundtrip(&b, 0x01, (const uint8_t[]){ 0x30, 1, 0, 1 }, 4, body) == 2);
    CHECK(b.coins[0] == 1);

    /* Retransmit returns the previous reply byte-for-byte. */
    uint8_t pkt[64], out1[64], out2[64];
    size_t l1 = jvs_feed(&b, pkt, packet(pkt, 1, (const uint8_t[]){ 0x21, 2 }, 2), out1, sizeof out1);
    size_t l2 = jvs_feed(&b, pkt, packet(pkt, 1, (const uint8_t[]){ 0x2F }, 1), out2, sizeof out2);
    CHECK(l1 == l2 && l1 && !memcmp(out1, out2, l1));

    /* A value needing escape (0xE0 coins = 224) round-trips. */
    b.coins[1] = 0xE0;
    n = roundtrip(&b, 0x01, (const uint8_t[]){ 0x21, 2 }, 2, body);
    CHECK(n == 6 && body[5] == 0xE0);

    /* Bad checksum. */
    size_t len = packet(pkt, 1, (const uint8_t[]){ 0x10 }, 1);
    pkt[len - 1] ^= 0x55;
    l1 = jvs_feed(&b, pkt, len, out1, sizeof out1);
    CHECK(reply(out1, l1, body) == 1 && body[0] == 3);

    /* Unknown command. */
    CHECK(roundtrip(&b, 0x01, (const uint8_t[]){ 0x77 }, 1, body) == 1 && body[0] == 2);

    /* Byte-at-a-time delivery behaves the same as one chunk. */
    len = packet(pkt, 1, (const uint8_t[]){ 0x11 }, 1);
    l1 = 0;
    for (size_t i = 0; i < len; i++) l1 += jvs_feed(&b, pkt + i, 1, out1 + l1, sizeof out1 - l1);
    CHECK(reply(out1, l1, body) == 3 && body[2] == 0x13);

    /* Reset drops the address. */
    roundtrip(&b, 0xFF, (const uint8_t[]){ 0xF0, 0xD9 }, 2, body);
    CHECK(b.address == 0);
    CHECK(roundtrip(&b, 0x01, (const uint8_t[]){ 0x10 }, 1, body) == 0);

    printf(failures ? "%d failure(s)\n" : "all JVS tests passed\n", failures);
    return failures != 0;
}
