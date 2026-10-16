#include "jvs.h"

#include <string.h>

enum { STATUS_OK = 1, STATUS_UNKNOWN_CMD = 2, STATUS_SUM_ERROR = 3 };
enum { REPORT_OK = 1, REPORT_PARAM_ERROR = 2 };

static void nolog(const char *fmt, ...) { (void)fmt; }

void jvs_init(struct jvs_board *b, void (*log)(const char *fmt, ...))
{
    memset(b, 0, sizeof *b);
    b->rx_pos = -1;
    b->log = log ? log : nolog;
}

void jvs_set_inputs(struct jvs_board *b, const struct jvs_inputs *in)
{
    for (int s = 0; s < JVS_SLOTS; s++)
        if (in->p[s].coin && !b->in.p[s].coin && b->coins[s] < 0x3FFF)
            b->coins[s]++;
    b->in = *in;
}

static uint8_t switch_byte(const struct jvs_player *p, int index)
{
    if (index == 0)
        return (p->start   ? 0x80 : 0) | (p->service ? 0x40 : 0) |
               (p->up      ? 0x20 : 0) | (p->down    ? 0x10 : 0) |
               (p->left    ? 0x08 : 0) | (p->right   ? 0x04 : 0) |
               (p->button[0] ? 0x02 : 0) | (p->button[1] ? 0x01 : 0);
    if (index == 1)
        return (p->button[2] ? 0x80 : 0) | (p->button[3] ? 0x40 : 0) |
               (p->button[4] ? 0x20 : 0) | (p->button[5] ? 0x10 : 0);
    return 0;
}

/* Run the commands of one packet; body receives status + reports. Returns body length, 0 = no reply. */
static size_t run_commands(struct jvs_board *b, const uint8_t *d, size_t n, uint8_t *body)
{
    size_t o = 1, i = 0;
    body[0] = STATUS_OK;

    while (i < n) {
        uint8_t cmd = d[i];
        switch (cmd) {
        case 0xF0: /* reset */
            if (i + 1 >= n) return 0;
            b->log("reset (%02X)", d[i + 1]);
            if (d[i + 1] == 0xD9)
                b->address = 0;
            return 0;
        case 0xF1: /* set address */
            if (i + 1 >= n) { body[o++] = REPORT_PARAM_ERROR; return o; }
            b->address = d[i + 1];
            b->log("address (%02X)", b->address);
            body[o++] = REPORT_OK;
            i += 2;
            break;
        case 0x10: /* identify */
            b->log("send identify (%s)", JVS_IDENTITY);
            body[o++] = REPORT_OK;
            memcpy(body + o, JVS_IDENTITY, sizeof JVS_IDENTITY);
            o += sizeof JVS_IDENTITY;
            i += 1;
            break;
        case 0x11: /* command format revision 1.3 */
            body[o++] = REPORT_OK; body[o++] = 0x13; i += 1;
            break;
        case 0x12: /* JVS revision 3.0 */
            body[o++] = REPORT_OK; body[o++] = 0x30; i += 1;
            break;
        case 0x13: /* communications version 1.0 */
            body[o++] = REPORT_OK; body[o++] = 0x10; i += 1;
            break;
        case 0x14: { /* feature report: 2 players x 14 switches, 2 coin slots */
            static const uint8_t features[] = { 0x01, 2, 14, 0, 0x02, 2, 0, 0, 0x00 };
            body[o++] = REPORT_OK;
            memcpy(body + o, features, sizeof features);
            o += sizeof features;
            i += 1;
            break;
        }
        case 0x20: { /* switch inputs */
            if (i + 2 >= n) { body[o++] = REPORT_PARAM_ERROR; return o; }
            uint8_t players = d[i + 1], bytes = d[i + 2];
            body[o++] = REPORT_OK;
            body[o++] = b->in.test ? 0x80 : 0;
            for (int p = 0; p < players; p++)
                for (int k = 0; k < bytes; k++)
                    body[o++] = p < JVS_PLAYERS ? switch_byte(&b->in.p[p], k) : 0;
            i += 3;
            break;
        }
        case 0x21: { /* coin counters */
            if (i + 1 >= n) { body[o++] = REPORT_PARAM_ERROR; return o; }
            uint8_t slots = d[i + 1];
            body[o++] = REPORT_OK;
            for (int s = 0; s < slots; s++) {
                uint16_t c = s < JVS_SLOTS ? b->coins[s] : 0;
                body[o++] = (c >> 8) & 0x3F;
                body[o++] = c & 0xFF;
            }
            i += 2;
            break;
        }
        case 0x30: { /* decrease coins */
            if (i + 3 >= n) { body[o++] = REPORT_PARAM_ERROR; return o; }
            uint8_t slot = d[i + 1];
            uint16_t amount = (uint16_t)(d[i + 2] << 8 | d[i + 3]);
            b->log("remove coin %02X %04X", slot, amount);
            if (slot >= 1 && slot <= JVS_SLOTS)
                b->coins[slot - 1] = b->coins[slot - 1] > amount ? b->coins[slot - 1] - amount : 0;
            body[o++] = REPORT_OK;
            i += 4;
            break;
        }
        default:
            b->log("### unknown command (%02X) ###", cmd);
            body[0] = STATUS_UNKNOWN_CMD;
            return 1;
        }
    }
    return o;
}

static size_t put_escaped(uint8_t *out, size_t o, uint8_t v)
{
    if (v == JVS_SYNC || v == JVS_MARK) {
        out[o++] = JVS_MARK;
        out[o++] = v - 1;
    } else {
        out[o++] = v;
    }
    return o;
}

static size_t encode(struct jvs_board *b, const uint8_t *body, size_t n)
{
    size_t o = 0;
    uint8_t len = (uint8_t)(n + 1), sum = JVS_MASTER + len;
    b->last_tx[o++] = JVS_SYNC;
    o = put_escaped(b->last_tx, o, JVS_MASTER);
    o = put_escaped(b->last_tx, o, len);
    for (size_t i = 0; i < n; i++) {
        sum += body[i];
        o = put_escaped(b->last_tx, o, body[i]);
    }
    o = put_escaped(b->last_tx, o, sum);
    b->last_tx_len = o;
    return o;
}

static size_t handle_packet(struct jvs_board *b, uint8_t *out, size_t out_cap)
{
    size_t n = b->rx_len - 1u;   /* data bytes, excluding checksum */
    uint8_t sum = b->rx_dest + b->rx_len;
    for (size_t i = 0; i < n; i++)
        sum += b->rx_data[i];

    bool broadcast = b->rx_dest == JVS_BROADCAST;
    if (!broadcast && (b->address == 0 || b->rx_dest != b->address))
        return 0;
    /* Once addressed, ignore broadcast F1 meant for boards further down the chain. */
    if (broadcast && b->address != 0 && n > 0 && b->rx_data[0] == 0xF1)
        return 0;

    uint8_t body[256];
    size_t body_len;
    if (sum != b->rx_data[n]) {
        b->log("checksum: %02X expect %02X", b->rx_data[n], sum);
        body[0] = STATUS_SUM_ERROR;
        body_len = 1;
    } else if (n == 1 && b->rx_data[0] == 0x2F) {
        b->log("host requested retransmit");
        if (b->last_tx_len > out_cap) return 0;
        memcpy(out, b->last_tx, b->last_tx_len);
        return b->last_tx_len;
    } else {
        body_len = run_commands(b, b->rx_data, n, body);
        if (body_len == 0) return 0;
    }

    size_t len = encode(b, body, body_len);
    if (len > out_cap) return 0;
    memcpy(out, b->last_tx, len);
    return len;
}

size_t jvs_feed(struct jvs_board *b, const uint8_t *data, size_t n, uint8_t *out, size_t out_cap)
{
    size_t produced = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t v = data[i];
        if (v == JVS_SYNC) {
            b->rx_pos = 0;
            b->rx_escape = false;
            continue;
        }
        if (b->rx_pos < 0) continue;
        if (v == JVS_MARK) {
            b->rx_escape = true;
            continue;
        }
        if (b->rx_escape) {
            v++;
            b->rx_escape = false;
        }

        if (b->rx_pos == 0) {
            b->rx_dest = v;
            b->rx_pos = 1;
        } else if (b->rx_pos == 1) {
            b->rx_len = v;
            b->rx_pos = v == 0 ? -1 : 2;
        } else {
            b->rx_data[b->rx_pos - 2] = v;
            if (++b->rx_pos - 2 == b->rx_len) {
                b->rx_pos = -1;
                produced += handle_packet(b, out + produced, out_cap - produced);
            }
        }
    }
    return produced;
}
