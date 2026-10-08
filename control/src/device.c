#include "keeper.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CTRL_MAX 8
#define CTRL_LINE_MAX 192
/* LOG payload received while nobody streams (e.g. while a command reply is
 * awaited) is kept here so 'reset --log' can show the very first boot lines. */
#define PENDING_MAX 65536

struct device {
    int fd;
    parser_t parser;
    FILE *capture;

    char ctrl[CTRL_MAX][CTRL_LINE_MAX];
    size_t ctrl_count;

    uint8_t pending[PENDING_MAX];
    size_t pending_len;

    int (*log_cb)(const uint8_t *data, size_t len, void *user);
    void *log_user;
    int logging;

    char err[256];
};

static void set_deadline(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static int past_deadline(const struct timespec *ts)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > ts->tv_sec ||
           (now.tv_sec == ts->tv_sec && now.tv_nsec >= ts->tv_nsec);
}

static void push_ctrl(device_t *d, const uint8_t *payload, size_t len)
{
    while (len > 0 && (payload[len - 1] == '\r' || payload[len - 1] == '\n')) {
        len--;
    }
    if (d->ctrl_count >= CTRL_MAX) {
        return; // stale replies only accumulate if nobody reads them
    }
    size_t n = len < CTRL_LINE_MAX - 1 ? len : CTRL_LINE_MAX - 1;
    char *dst = d->ctrl[d->ctrl_count++];
    memcpy(dst, payload, n);
    dst[n] = '\0';
}

static void on_frame(uint8_t type, const uint8_t *payload, size_t len,
                     void *user)
{
    device_t *d = user;
    if (type == PKT_TYPE_CTRL) {
        push_ctrl(d, payload, len);
    } else if (type == PKT_TYPE_LOG) {
        if (d->capture != NULL) {
            fwrite(payload, 1, len, d->capture);
            fflush(d->capture);
        }
        if (d->logging && d->log_cb != NULL) {
            if (d->log_cb(payload, len, d->log_user)) {
                d->logging = 0;
            }
        } else if (d->pending_len < PENDING_MAX) {
            memcpy(d->pending + d->pending_len, payload, len);
            d->pending_len += len;
        }
    }
}

/* Returns >0 when bytes were consumed, 0 on idle/timeout, -1 on error. */
static int pump(device_t *d, int timeout_ms)
{
    struct pollfd pfd = { .fd = d->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) {
            return 0;
        }
        snprintf(d->err, sizeof(d->err), "poll: %s", strerror(errno));
        return -1;
    }
    if (pr == 0) {
        return 0;
    }
    uint8_t buf[1024];
    ssize_t n = read(d->fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        snprintf(d->err, sizeof(d->err), "read: %s", strerror(errno));
        return -1;
    }
    if (n == 0) {
        if (pfd.revents & POLLHUP) {
            snprintf(d->err, sizeof(d->err), "device lost (EOF)");
            return -1;
        }
        return 0;
    }
    parser_feed(&d->parser, buf, (size_t)n, on_frame, d);
    return 1;
}

device_t *device_open(const char *port, const char *capture_path, char *err,
                      size_t err_sz)
{
    char chosen[280];
    if (port == NULL) {
        tty_port_t found[16];
        int n = tty_scan_espressif(found, 16);
        if (n == 0) {
            snprintf(err, err_sz,
                     "keeper dongle not found (ESP32-S3 USB-JTAG/serial "
                     "303a:1001); use --port");
            return NULL;
        }
        snprintf(chosen, sizeof(chosen), "%s", found[0].dev);
        for (int i = 0; i < n && i < 16; i++) {
            if (found[i].pid == ESPRESSIF_USJ_PID) {
                snprintf(chosen, sizeof(chosen), "%s", found[i].dev);
                break;
            }
        }
    } else {
        snprintf(chosen, sizeof(chosen), "%s", port);
    }

    int fd = tty_open(chosen);
    if (fd < 0) {
        if (errno == EBUSY) {
            snprintf(err, err_sz,
                     "%s is busy (held by another capsula-keeper process)",
                     chosen);
        } else {
            snprintf(err, err_sz, "cannot open %s: %s", chosen,
                     strerror(errno));
        }
        return NULL;
    }
    device_t *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        close(fd);
        snprintf(err, err_sz, "out of memory");
        return NULL;
    }
    d->fd = fd;
    parser_init(&d->parser);
    if (capture_path != NULL) {
        d->capture = fopen(capture_path, "ab");
        if (d->capture == NULL) {
            snprintf(err, err_sz, "cannot open capture file %s: %s",
                     capture_path, strerror(errno));
            close(fd);
            free(d);
            return NULL;
        }
    }
    return d;
}

void device_close(device_t *d)
{
    if (d == NULL) {
        return;
    }
    if (d->capture != NULL) {
        fclose(d->capture);
    }
    close(d->fd);
    free(d);
}

const char *device_err(const device_t *d)
{
    return d->err;
}

/* Blocking write of the whole buffer (fd is O_NONBLOCK, so wait on POLLOUT). */
static int write_all(device_t *d, const void *data, size_t len, char *reply,
                     size_t reply_sz)
{
    const uint8_t *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(d->fd, p + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = d->fd, .events = POLLOUT };
            poll(&pfd, 1, 200);
            continue;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        snprintf(reply, reply_sz, "write failed: %s",
                 w < 0 ? strerror(errno) : "short write");
        return -1;
    }
    return 0;
}

/* A command or its reply can be lost while the target's log flood saturates
 * the dongle (observed on real hardware as an occasional first-command
 * timeout after idle). Resend once before giving up; every keeper command
 * is idempotent, a lost reply only means the sequence may run twice. */
#define CMD_ATTEMPTS 2

int device_command(device_t *d, const char *cmd, long timeout_ms, char *reply,
                   size_t reply_sz)
{
    char line[160];
    int l = snprintf(line, sizeof(line), "@%s\n", cmd);
    for (int attempt = 0;; attempt++) {
        d->ctrl_count = 0; // drop stale replies
        if (write_all(d, line, (size_t)l, reply, reply_sz) != 0) {
            return -1;
        }
        struct timespec dl;
        set_deadline(&dl, timeout_ms);
        for (;;) {
            if (d->ctrl_count > 0) {
                snprintf(reply, reply_sz, "%s", d->ctrl[0]);
                for (size_t i = 1; i < d->ctrl_count; i++) {
                    memcpy(d->ctrl[i - 1], d->ctrl[i], CTRL_LINE_MAX);
                }
                d->ctrl_count--;
                return strncmp(reply, "ERR", 3) == 0 ? 1 : 0;
            }
            if (past_deadline(&dl)) {
                break; // this attempt timed out, resend below
            }
            if (pump(d, 20) < 0) {
                snprintf(reply, reply_sz, "%s", d->err);
                return -1;
            }
        }
        if (attempt + 1 >= CMD_ATTEMPTS) {
            snprintf(reply, reply_sz, "timeout waiting for reply to @%s", cmd);
            return -1;
        }
    }
}

int device_send_console(device_t *d, const uint8_t *data, size_t n)
{
    char reply[1];
    return write_all(d, data, n, reply, sizeof(reply));
}

int device_log_loop(device_t *d,
                    int (*cb)(const uint8_t *data, size_t len, void *user),
                    void *user, const volatile int *stop, long timeout_ms)
{
    d->log_cb = cb;
    d->log_user = user;
    /* First deliver whatever accumulated while nobody was streaming
     * (boot logs that arrived during a preceding command, e.g. RESET). */
    if (d->pending_len > 0 && !d->logging) {
        size_t n = d->pending_len;
        d->pending_len = 0;
        if (cb(d->pending, n, user)) {
            d->logging = 0;
            return 0;
        }
    }
    d->logging = 1;

    struct timespec dl;
    int have_dl = timeout_ms > 0;
    if (have_dl) {
        set_deadline(&dl, timeout_ms);
    }
    for (;;) {
        if (have_dl && past_deadline(&dl)) {
            return 2;
        }
        if (*stop) {
            return 0;
        }
        int rc = pump(d, 100);
        if (rc < 0) {
            return -1;
        }
        if (!d->logging) {
            return 0; // cb decided to stop
        }
    }
}
