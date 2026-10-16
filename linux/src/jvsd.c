/*
 * jvsd - native Linux JVS I/O board for TGM3 under Wine.
 *
 * The Windows port runs typex_io.dll inside game.exe to answer JVS traffic on COM2.
 * Here the board lives in this process; jvsbridge.dll (inside Wine) forwards the
 * game's COM2 bytes over a loopback TCP connection.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "input.h"
#include "jvs.h"

#define DEFAULT_PORT 26100

static int verbose;
static volatile sig_atomic_t stop;

static void logf_(const char *fmt, ...)
{
    if (verbose < 1) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("jvs: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void dump(const char *dir, const uint8_t *p, size_t n)
{
    if (verbose < 2) return;
    fprintf(stderr, "%s", dir);
    for (size_t i = 0; i < n; i++) fprintf(stderr, " %02X", p[i]);
    fputc('\n', stderr);
}

static void on_signal(int sig) { (void)sig; stop = 1; }

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port),
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-p port] [-c bindings.conf] [-v] [-v]\n", argv0);
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT, opt;
    const char *config = NULL;
    while ((opt = getopt(argc, argv, "p:c:vh")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'c': config = optarg; break;
        case 'v': verbose++; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    struct input *input = input_open(config, verbose > 0);
    if (!input) return 1;

    int lfd = listen_on(port);
    if (lfd < 0) {
        fprintf(stderr, "jvsd: cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
        return 1;
    }
    fprintf(stderr, "jvsd: listening on 127.0.0.1:%d\n", port);

    struct jvs_board board;
    jvs_init(&board, logf_);
    struct jvs_inputs state = { 0 };
    int cfd = -1, rc = 0;

    while (!stop) {
        struct pollfd fds[2 + 32];
        int n = 0;
        fds[n++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
        if (cfd >= 0) fds[n++] = (struct pollfd){ .fd = cfd, .events = POLLIN };
        int first_input = n;
        n += input_pollfds(input, fds + n, (int)(sizeof fds / sizeof *fds) - n);

        if (poll(fds, (nfds_t)n, -1) < 0 && errno != EINTR) {
            perror("poll");
            rc = 1;
            break;
        }

        /* Inputs first so a reply to this round's poll sees the freshest state. */
        bool any_input = false;
        for (int i = first_input; i < n; i++) any_input |= fds[i].revents != 0;
        if (any_input || cfd >= 0) {
            if (input_update(input, &state)) {
                fprintf(stderr, "jvsd: quit pressed\n");
                break;
            }
            jvs_set_inputs(&board, &state);
        }

        if (fds[0].revents & POLLIN) {
            int nfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (nfd >= 0) {
                int one = 1;
                setsockopt(nfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                if (cfd >= 0) close(cfd);
                cfd = nfd;
                jvs_init(&board, logf_);   /* new game session: board starts unaddressed */
                board.in = state;          /* no coin edge for a button already held */
                fprintf(stderr, "jvsd: game connected\n");
            }
        }

        if (cfd >= 0 && n > 1 && fds[1].fd == cfd && fds[1].revents) {
            uint8_t buf[512], out[4096];
            ssize_t r = read(cfd, buf, sizeof buf);
            if (r <= 0) {
                fprintf(stderr, "jvsd: game disconnected\n");
                close(cfd);
                cfd = -1;
                continue;
            }
            dump("rx", buf, (size_t)r);
            size_t len = jvs_feed(&board, buf, (size_t)r, out, sizeof out);
            dump("tx", out, len);
            if (len && !write_all(cfd, out, len)) {
                close(cfd);
                cfd = -1;
            }
        }
    }

    /* Closing the connection makes jvsbridge.dll end the game, like typex_io's quit key. */
    if (cfd >= 0) close(cfd);
    close(lfd);
    input_close(input);
    return rc;
}
