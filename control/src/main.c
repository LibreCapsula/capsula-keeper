#include "keeper.h"

#include <errno.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile int g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* --- strict numeric option parsing ---------------------------------------- */

static int parse_long(const char *s, long *out)
{
    if (s == NULL || *s == '\0') {
        return -1;
    }
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

static int parse_double(const char *s, double *out)
{
    if (s == NULL || *s == '\0') {
        return -1;
    }
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || *end != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

static const char *usb_pid_name(unsigned pid)
{
    switch (pid) {
    case USB_PID_MASKROM_RK3308:
        return "MaskROM (RK3308 BootROM)";
    case USB_PID_MASKROM_GENERIC:
        return "MaskROM (BootROM)";
    case USB_PID_LOADER_CLASSIC:
        return "Loader (miniloader)";
    default:
        return "rockusb/unknown";
    }
}

static void usage(FILE *f)
{
    fprintf(f,
            "usage: capsula-keeper [--port DEV] <command> [options]\n"
            "\n"
            "commands:\n"
            "  ports                          list detected dongles\n"
            "  usb                            list Rockchip (2207:*) USB devices\n"
            "  ping                           check the dongle is alive\n"
            "  status                         uptime, baud, drops, button states\n"
            "  reset [--ms N] [--log [REGEX]] [--max-lines N] [--timeout SEC]\n"
            "                                 reboot, then stream target logs\n"
            "  loader [--pre-ms A] [--reset-ms B] [--post-ms C]\n"
            "                                 reboot into Rockchip LOADER/MaskROM\n"
            "  maskrom [--pre-ms A] [--reset-ms B] [--post-ms C]\n"
            "                                 force MaskROM via TP48 (FLASH_D0) short\n"
            "  press <reset|recovery|maskrom> [--ms N]\n"
            "                                 hold a button (or until 'keeper release')\n"
            "  release <reset|recovery|maskrom>\n"
            "                                 release a held button\n"
            "  baud <RATE>                    change target UART baud (default 1500000)\n"
            "  send <TEXT> [--no-newline]     send a line to the target console\n"
            "  telnet [--timeout SEC]         interactive console: logs on screen,\n"
            "                                 keystrokes go to the target (Ctrl-] quits)\n"
            "  log [--until REGEX] [--timeout SEC] [--capture FILE] [--raw]\n"
            "                                 stream target logs (Ctrl-C to stop)\n");
}

/* --- small option parser ------------------------------------------------- */

typedef struct {
    const char *name;
    int has_value;
} optdef_t;

#define MAX_POS 8

static int parse_opts(int argc, char **argv, const optdef_t *defs, int ndefs,
                      const char *values[], const char *pos[], int *npos,
                      char *err, size_t errsz)
{
    for (int i = 0; i < ndefs; i++) {
        values[i] = NULL;
    }
    *npos = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        int matched = 0;
        for (int k = 0; k < ndefs; k++) {
            if (strcmp(a, defs[k].name) != 0) {
                continue;
            }
            matched = 1;
            if (defs[k].has_value) {
                if (i + 1 >= argc || strncmp(argv[i + 1], "--", 2) == 0) {
                    /* optional-value style: "--flag --other" leaves it empty */
                    values[k] = "";
                } else {
                    values[k] = argv[++i];
                }
            } else {
                values[k] = "";
            }
            break;
        }
        if (!matched) {
            if (*npos >= MAX_POS) {
                snprintf(err, errsz, "unexpected argument: %s", a);
                return -1;
            }
            pos[(*npos)++] = a;
        }
    }
    return 0;
}

/* --- commands ------------------------------------------------------------ */

static int run_cmd_retry(const char *port, const char *cmd, long timeout_ms,
                         int retries);

static int run_cmd(const char *port, const char *cmd, long timeout_ms)
{
    return run_cmd_retry(port, cmd, timeout_ms, 0);
}

/* The target UART stream can saturate the dongle and a host command can be
 * dropped in the flood; read-only idempotent commands (PING/STAT/...) are
 * simply retried in that case. State-changing sequences get retries=0. */
static int run_cmd_retry(const char *port, const char *cmd, long timeout_ms,
                         int retries)
{
    char err[256], reply[256];
    for (int attempt = 0;; attempt++) {
        device_t *d = device_open(port, NULL, err, sizeof(err));
        if (d == NULL) {
            fprintf(stderr, "capsula-keeper: %s\n", err);
            return 1;
        }
        int rc = device_command(d, cmd, timeout_ms, reply, sizeof(reply));
        device_close(d);
        if (rc == 0) {
            printf("%s\n", reply);
            return 0;
        }
        if (rc < 0 && attempt < retries) {
            usleep(500 * 1000);
            continue;
        }
        fprintf(stderr, "capsula-keeper: %s\n", reply);
        return 1;
    }
}

static int confirm_download_mode(const unsigned *want, int nwant,
                                 long timeout_ms);

static int cmd_ports(void)
{
    tty_port_t found[16];
    int n = tty_scan_espressif(found, 16);
    if (n == 0) {
        printf("no keeper dongles found (ESP32-S3 USB-JTAG/serial "
               "303a:1001)\n");
        return 1;
    }
    for (int i = 0; i < n && i < 16; i++) {
        printf("%s (%04x:%04x)\n", found[i].dev, found[i].vid, found[i].pid);
    }
    return 0;
}

static int run_log_stream(device_t *d, const char *until, const char *timeout_s,
                          const char *max_lines_s, int raw);

static int cmd_reset(int argc, char **argv, const char *port)
{
    const optdef_t defs[] = {
        { "--ms", 1 },
        { "--log", 1 },     /* fall through to the log stream after reset */
        { "--until", 1 },   /* passed to the log stream */
        { "--max-lines", 1 },
        { "--timeout", 1 },
    };
    enum { OPT_MS, OPT_LOG, OPT_UNTIL, OPT_MAXL, OPT_TMOUT };
    const char *vals[5], *pos[MAX_POS];
    int npos;
    char err[128], cmd[32];
    if (parse_opts(argc, argv, defs, 5, vals, pos, &npos, err, sizeof(err)) !=
        0) {
        fprintf(stderr, "capsula-keeper: %s\n", err);
        return 2;
    }
    if (npos > 0) {
        fprintf(stderr, "capsula-keeper: unexpected argument '%s'\n", pos[0]);
        return 2;
    }
    long ms = 1000;
    if (vals[OPT_MS] != NULL && parse_long(vals[OPT_MS], &ms) != 0) {
        fprintf(stderr, "capsula-keeper: bad --ms value\n");
        return 2;
    }
    if (ms <= 0 || ms > 60000) {
        fprintf(stderr, "capsula-keeper: bad --ms value\n");
        return 2;
    }
    snprintf(cmd, sizeof(cmd), "RESET %ld", ms);
    if (vals[OPT_LOG] == NULL) {
        return run_cmd(port, cmd, ms + 3000);
    }

    /* Keep the port open across the reset: with close/reopen every byte the
     * dongle streams between the two opens (i.e. the very start of the boot)
     * would be lost. LOG frames arriving while we wait for the RESET reply
     * are buffered by the device and drained by the log loop. */
    device_t *d = device_open(port, NULL, err, sizeof(err));
    if (d == NULL) {
        fprintf(stderr, "capsula-keeper: %s\n", err);
        return 1;
    }
    char reply[256];
    int rc = device_command(d, cmd, ms + 3000, reply, sizeof(reply));
    if (rc != 0) {
        fprintf(stderr, "capsula-keeper: %s\n", reply);
        device_close(d);
        return 1;
    }
    printf("%s\n", reply);
    fflush(stdout);
    /* --log's value (if given) acts as the --until pattern; a separate
     * --until also passes through. Plain --log just streams. */
    const char *until = vals[OPT_LOG][0] != '\0' ? vals[OPT_LOG] :
                        vals[OPT_UNTIL];
    rc = run_log_stream(d, until, vals[OPT_TMOUT], vals[OPT_MAXL], 0);
    device_close(d);
    return rc;
}

static int cmd_loader(int argc, char **argv, const char *port)
{
    const optdef_t defs[] = { { "--pre-ms", 1 },
                              { "--reset-ms", 1 },
                              { "--post-ms", 1 },
                              { "--no-usb-check", 0 } };
    const char *vals[4], *pos[MAX_POS];
    int npos;
    char err[128], cmd[64];
    if (parse_opts(argc, argv, defs, 4, vals, pos, &npos, err, sizeof(err)) !=
        0) {
        fprintf(stderr, "capsula-keeper: %s\n", err);
        return 2;
    }
    if (npos > 0) {
        fprintf(stderr, "capsula-keeper: unexpected argument '%s'\n", pos[0]);
        return 2;
    }
    long pre = 200, rst = 1000, post = 3000;
    if ((vals[0] != NULL && parse_long(vals[0], &pre) != 0) ||
        (vals[1] != NULL && parse_long(vals[1], &rst) != 0) ||
        (vals[2] != NULL && parse_long(vals[2], &post) != 0)) {
        fprintf(stderr, "capsula-keeper: bad timings\n");
        return 2;
    }
    if (pre <= 0 || pre > 60000 || rst <= 0 || rst > 60000 || post <= 0 ||
        post > 60000) {
        fprintf(stderr, "capsula-keeper: bad timings (1..60000 ms)\n");
        return 2;
    }
    snprintf(cmd, sizeof(cmd), "LOADER %ld %ld %ld", pre, rst, post);
    int rc = run_cmd(port, cmd, pre + rst + post + 3000);
    if (rc != 0) {
        return rc;
    }
    if (vals[3] != NULL) {
        printf("target should be in LOADER mode now (--no-usb-check: "
               "verify with rkdeveloptool ld)\n");
        return 0;
    }
    const unsigned want[] = { USB_PID_MASKROM_RK3308, USB_PID_LOADER_CLASSIC,
                              USB_PID_MASKROM_GENERIC };
    rc = confirm_download_mode(want, 3, 10000);
    if (rc != 0) {
        fprintf(stderr,
                "  note: the download key is only honored by U-Boot when\n"
                "  CONFIG_DM_KEY/CONFIG_ADC_KEY are enabled in its defconfig\n"
                "  (currently disabled in capsula_rk3308_defconfig);\n"
                "  use 'keeper maskrom' instead.\n");
    }
    return rc;
}

static int cmd_usb(void)
{
    unsigned pids[16];
    int n = usb_list_rockchip(pids, 16);
    if (n == 0) {
        printf("no Rockchip (2207:*) USB devices\n");
        return 1;
    }
    for (int i = 0; i < n && i < 16; i++) {
        printf("2207:%04x  %s\n", pids[i], usb_pid_name(pids[i]));
    }
    return 0;
}

/* After a download-entry sequence, confirm the target actually showed up
 * in a Rockchip download mode on USB. Returns 0 (confirmed), 4 (not seen). */
static int confirm_download_mode(const unsigned *want, int nwant,
                                 long timeout_ms)
{
    unsigned found = 0;
    if (usb_wait_rockchip(want, nwant, timeout_ms, &found)) {
        printf("confirmed: target in download mode (2207:%04x, %s)\n", found,
               usb_pid_name(found));
        return 0;
    }
    unsigned seen[16];
    int n = usb_list_rockchip(seen, 16);
    fprintf(stderr,
            "capsula-keeper: no download-mode USB device appeared within %ldms\n",
            timeout_ms);
    if (n > 0) {
        fprintf(stderr, "  Rockchip USB devices present (not download modes):");
        for (int i = 0; i < n; i++) {
            fprintf(stderr, " 2207:%04x", seen[i]);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr,
            "  checklist:\n"
            "  - board's OTG cable plugged into this PC? ('keeper usb' to list)\n"
            "  - MOSFET drain connected to TP48? TP48 must drop to ~0.2V\n"
            "    during 'keeper press maskrom'\n"
            "  - longer hold: keeper maskrom --post-ms 3000\n");
    return 4;
}

static int cmd_maskrom(int argc, char **argv, const char *port)
{
    const optdef_t defs[] = { { "--pre-ms", 1 },
                              { "--reset-ms", 1 },
                              { "--post-ms", 1 },
                              { "--no-usb-check", 0 } };
    const char *vals[4], *pos[MAX_POS];
    int npos;
    char err[128], cmd[64];
    if (parse_opts(argc, argv, defs, 4, vals, pos, &npos, err, sizeof(err)) !=
        0) {
        fprintf(stderr, "capsula-keeper: %s\n", err);
        return 2;
    }
    if (npos > 0) {
        fprintf(stderr, "capsula-keeper: unexpected argument '%s'\n", pos[0]);
        return 2;
    }
    long pre = 100, rst = 1000, post = 1500;
    if ((vals[0] != NULL && parse_long(vals[0], &pre) != 0) ||
        (vals[1] != NULL && parse_long(vals[1], &rst) != 0) ||
        (vals[2] != NULL && parse_long(vals[2], &post) != 0)) {
        fprintf(stderr, "capsula-keeper: bad timings\n");
        return 2;
    }
    if (pre <= 0 || pre > 60000 || rst <= 0 || rst > 60000 || post <= 0 ||
        post > 60000) {
        fprintf(stderr, "capsula-keeper: bad timings (1..60000 ms)\n");
        return 2;
    }
    snprintf(cmd, sizeof(cmd), "MASKROM %ld %ld %ld", pre, rst, post);
    int rc = run_cmd(port, cmd, pre + rst + post + 3000);
    if (rc != 0) {
        return rc;
    }
    if (vals[3] != NULL) {
        printf("target should be in MaskROM mode now (--no-usb-check: "
               "verify with lsusb | grep 2207)\n");
        return 0;
    }
    const unsigned want[] = { USB_PID_MASKROM_RK3308,
                              USB_PID_MASKROM_GENERIC };
    return confirm_download_mode(want, 2, 6000);
}

static int valid_button(const char *b)
{
    return strcmp(b, "reset") == 0 || strcmp(b, "rst") == 0 ||
           strcmp(b, "recovery") == 0 || strcmp(b, "rec") == 0 ||
           strcmp(b, "maskrom") == 0 || strcmp(b, "tp48") == 0 ||
           strcmp(b, "flash_d0") == 0;
}

static int cmd_press(int argc, char **argv, const char *port)
{
    const optdef_t defs[] = { { "--ms", 1 } };
    const char *vals[1], *pos[MAX_POS];
    int npos;
    char err[128], cmd[64];
    if (parse_opts(argc, argv, defs, 1, vals, pos, &npos, err, sizeof(err)) !=
        0) {
        fprintf(stderr, "capsula-keeper: %s\n", err);
        return 2;
    }
    if (npos != 1 || !valid_button(pos[0])) {
        fprintf(stderr, "capsula-keeper: usage: capsula-keeper press <reset|recovery> [--ms N]\n");
        return 2;
    }
    long ms = 0;
    if (vals[0] != NULL && parse_long(vals[0], &ms) != 0) {
        fprintf(stderr, "capsula-keeper: bad --ms value\n");
        return 2;
    }
    if (ms < 0 || ms > 60000) {
        fprintf(stderr, "capsula-keeper: bad --ms value\n");
        return 2;
    }
    int rc;
    if (ms > 0) {
        snprintf(cmd, sizeof(cmd), "PRESS %s %ld", pos[0], ms);
        rc = run_cmd(port, cmd, ms + 3000);
    } else {
        snprintf(cmd, sizeof(cmd), "PRESS %s", pos[0]);
        rc = run_cmd(port, cmd, 5000);
    }
    return rc;
}

static int cmd_release(int argc, char **argv, const char *port)
{
    const char *pos[MAX_POS];
    int npos;
    char err[128], cmd[64];
    if (parse_opts(argc, argv, NULL, 0, NULL, pos, &npos, err, sizeof(err)) !=
        0) {
        fprintf(stderr, "capsula-keeper: %s\n", err);
        return 2;
    }
    if (npos != 1 || !valid_button(pos[0])) {
        fprintf(stderr, "capsula-keeper: usage: capsula-keeper release <reset|recovery>\n");
        return 2;
    }
    snprintf(cmd, sizeof(cmd), "RELEASE %s", pos[0]);
    return run_cmd_retry(port, cmd, 5000, 2);
}

static int cmd_baud(int argc, char **argv, const char *port)
{
    const char *pos[MAX_POS];
    int npos;
    char err[128], cmd[32];
    if (parse_opts(argc, argv, NULL, 0, NULL, pos, &npos, err, sizeof(err)) !=
        0) {
        fprintf(stderr, "capsula-keeper: %s\n", err);
        return 2;
    }
    if (npos != 1) {
        fprintf(stderr, "capsula-keeper: usage: capsula-keeper baud <RATE>\n");
        return 2;
    }
    long rate = 0;
    if (parse_long(pos[0], &rate) != 0 || rate < 1200 || rate > 5000000) {
        fprintf(stderr, "capsula-keeper: baud out of range (1200..5000000)\n");
        return 2;
    }
    snprintf(cmd, sizeof(cmd), "BAUD %ld", rate);
    return run_cmd_retry(port, cmd, 5000, 2);
}

static int cmd_send(int argc, char **argv, const char *port)
{
    const optdef_t defs[] = { { "--no-newline", 0 } };
    const char *vals[1], *pos[MAX_POS];
    int npos;
    char err[128];
    if (parse_opts(argc, argv, defs, 1, vals, pos, &npos, err, sizeof(err)) !=
        0) {
        fprintf(stderr, "capsula-keeper: %s\n", err);
        return 2;
    }
    if (npos < 1) {
        fprintf(stderr, "capsula-keeper: usage: capsula-keeper send <TEXT> [--no-newline]\n");
        return 2;
    }
    char text[1024];
    size_t off = 0;
    for (int i = 0; i < npos; i++) {
        size_t l = strlen(pos[i]);
        if (off + l + 2 >= sizeof(text)) {
            fprintf(stderr, "capsula-keeper: text too long\n");
            return 2;
        }
        if (i > 0) {
            text[off++] = ' ';
        }
        memcpy(text + off, pos[i], l);
        off += l;
    }
    text[off] = '\0';
    if (text[0] == '@') {
        fprintf(stderr,
                "capsula-keeper: text starting with '@' is a command; "
                "send '@@...' to pass a literal '@'\n");
        return 1;
    }
    if (vals[0] == NULL) {
        text[off++] = '\n';
        text[off] = '\0';
    }

    char open_err[256];
    device_t *d = device_open(port, NULL, open_err, sizeof(open_err));
    if (d == NULL) {
        fprintf(stderr, "capsula-keeper: %s\n", open_err);
        return 1;
    }
    int rc = device_send_console(d, (const uint8_t *)text, off);
    device_close(d);
    if (rc != 0) {
        fprintf(stderr, "capsula-keeper: write failed\n");
        return 1;
    }
    return 0;
}

/* --- telnet: interactive console to the target UART --- */

#define TELNET_QUIT_KEY 0x1D /* Ctrl-], like telnet's escape */

static int telnet_print_cb(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    fwrite(data, 1, len, stdout);
    fflush(stdout);
    return 0;
}

static int cmd_telnet(int argc, char **argv, const char *port)
{
    const optdef_t defs[] = { { "--timeout", 1 } };
    const char *vals[1], *pos[MAX_POS];
    int npos;
    char err[128];
    if (parse_opts(argc, argv, defs, 1, vals, pos, &npos, err, sizeof(err)) !=
        0 || npos > 0) {
        fprintf(stderr, "capsula-keeper: %s\n", npos > 0 ? pos[0] : err);
        return 2;
    }
    double timeout = 0;
    if (vals[0] != NULL && parse_double(vals[0], &timeout) != 0) {
        fprintf(stderr, "capsula-keeper: bad --timeout value\n");
        return 2;
    }
    long timeout_ms = (long)(timeout * 1000.0);

    char open_err[256];
    device_t *d = device_open(port, NULL, open_err, sizeof(open_err));
    if (d == NULL) {
        fprintf(stderr, "capsula-keeper: %s\n", open_err);
        return 1;
    }

    struct termios tio_backup;
    int stdin_tty = isatty(STDIN_FILENO);
    if (stdin_tty) {
        tcgetattr(STDIN_FILENO, &tio_backup);
        struct termios raw = tio_backup;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    fprintf(stderr,
            "[telnet] console open; Ctrl-] quits, '@' is escaped "
            "automatically%s\n",
            timeout > 0 ? "" : " (no timeout)");
    g_stop = 0;
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    struct timespec dl;
    int have_dl = timeout_ms > 0;
    if (have_dl) {
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec += timeout_ms / 1000;
        dl.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) {
            dl.tv_sec += 1;
            dl.tv_nsec -= 1000000000L;
        }
    }

    int rc = 0;
    char inbuf[256];
    uint8_t out[512];
    for (;;) {
        /* 50 ms slices of the log stream, stdin in between */
        int lrc = device_log_loop(d, telnet_print_cb, NULL, &g_stop, 50);
        if (g_stop) {
            break;
        }
        if (lrc < 0) {
            fprintf(stderr, "\n[telnet] %s\n", device_err(d));
            rc = 1;
            break;
        }
        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0) {
            ssize_t n = read(STDIN_FILENO, inbuf, sizeof(inbuf));
            if (n == 0) { /* EOF (piped input done) */
                break;
            }
            size_t o = 0;
            int quit = 0;
            for (ssize_t i = 0; i < n; i++) {
                unsigned char ch = (unsigned char)inbuf[i];
                if (ch == TELNET_QUIT_KEY) {
                    quit = 1;
                    break;
                }
                if (ch == '@') {
                    out[o++] = '@'; /* '@@' passes a literal '@' through */
                }
                out[o++] = ch;
            }
            if (o > 0) {
                device_send_console(d, out, o);
            }
            if (quit) {
                g_stop = 1;
                break;
            }
        }
        if (have_dl) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > dl.tv_sec ||
                (now.tv_sec == dl.tv_sec && now.tv_nsec >= dl.tv_nsec)) {
                fprintf(stderr, "\n[telnet] --timeout %gs reached\n", timeout);
                rc = 2;
                break;
            }
        }
    }

    if (stdin_tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &tio_backup);
    }
    fprintf(stderr, "[telnet] closed\n");
    device_close(d);
    return rc;
}

/* --- log --- */

typedef struct {
    regex_t *rx;
    char line[8192];
    size_t llen;
    int matched;
    long max_lines; /* 0 = unlimited */
    long lines;     /* completed lines so far */
    int limit_hit;
    int raw; /* write bytes through immediately, even while --until matches */
} logctx_t;

static int log_cb(const uint8_t *data, size_t len, void *user)
{
    logctx_t *c = user;
    if (c->raw) {
        fwrite(data, 1, len, stdout);
        fflush(stdout);
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = data[i];
        if (ch != '\n') {
            if (c->rx != NULL && c->llen < sizeof(c->line) - 1) {
                c->line[c->llen++] = (char)ch;
            }
            continue;
        }
        /* a complete line ends here */
        c->lines++;
        int stop = 0;
        if (c->rx != NULL) {
            if (c->llen >= sizeof(c->line)) {
                c->llen = sizeof(c->line) - 1;
            }
            c->line[c->llen] = '\0';
            if (regexec(c->rx, c->line, 0, NULL, 0) == 0) {
                c->matched = 1;
                stop = 1;
            }
            c->llen = 0;
        }
        if (!stop && c->max_lines > 0 && c->lines >= c->max_lines) {
            c->limit_hit = 1;
            stop = 1;
        }
        if (stop) {
            if (!c->raw) {
                fwrite(data, 1, i + 1, stdout); /* through the newline */
                fflush(stdout);
            }
            return 1;
        }
    }
    if (!c->raw) {
        fwrite(data, 1, len, stdout);
        fflush(stdout);
    }
    return 0;
}

/* Streams target logs from an already-open device until the pattern hits,
 * the line limit is reached, the timeout expires or Ctrl-C.
 * timeout_s/max_lines_s: string option values (NULL = defaults).
 * Returns the process exit code. */
static int run_log_stream(device_t *d, const char *until, const char *timeout_s,
                          const char *max_lines_s, int raw)
{
    regex_t rx;
    int have_rx = 0;
    if (until != NULL && until[0] != '\0') {
        if (regcomp(&rx, until, REG_EXTENDED | REG_NOSUB) != 0) {
            fprintf(stderr, "capsula-keeper: bad --until regex\n");
            return 2;
        }
        have_rx = 1;
    }
    double timeout = 0;
    if (timeout_s != NULL && parse_double(timeout_s, &timeout) != 0) {
        fprintf(stderr, "capsula-keeper: bad --timeout value\n");
        if (have_rx) {
            regfree(&rx);
        }
        return 2;
    }
    long timeout_ms = (long)(timeout * 1000.0);
    long max_lines = 0;
    if (max_lines_s != NULL &&
        (parse_long(max_lines_s, &max_lines) != 0 || max_lines < 0)) {
        fprintf(stderr, "capsula-keeper: bad --max-lines value\n");
        if (have_rx) {
            regfree(&rx);
        }
        return 2;
    }

    logctx_t c = { .rx = have_rx ? &rx : NULL,
                   .llen = 0,
                   .matched = 0,
                   .max_lines = max_lines,
                   .lines = 0,
                   .limit_hit = 0,
                   .raw = raw };
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    g_stop = 0;

    int rc = device_log_loop(d, log_cb, &c, &g_stop, timeout_ms);
    const char *ioerr = device_err(d);
    if (have_rx) {
        regfree(&rx);
    }
    if (c.matched || c.limit_hit) {
        if (c.limit_hit) {
            fprintf(stderr, "capsula-keeper: --max-lines %ld reached\n",
                    max_lines);
        }
        return 0;
    }
    if (rc == 2) {
        fprintf(stderr, "capsula-keeper: --timeout %gs reached\n", timeout);
        return 2;
    }
    if (rc < 0) {
        fprintf(stderr, "\ncapsula-keeper: %s\n",
                ioerr[0] ? ioerr : "i/o error");
        return 1;
    }
    return 0;
}

static int cmd_log(int argc, char **argv, const char *port)
{
    const optdef_t defs[] = { { "--until", 1 },     { "--timeout", 1 },
                              { "--capture", 1 },   { "--max-lines", 1 },
                              { "--raw", 0 } };
    const char *vals[5], *pos[MAX_POS];
    int npos;
    char err[128];
    if (parse_opts(argc, argv, defs, 5, vals, pos, &npos, err, sizeof(err)) !=
        0) {
        fprintf(stderr, "capsula-keeper: %s\n", err);
        return 2;
    }
    if (npos > 0) {
        fprintf(stderr, "capsula-keeper: unexpected argument '%s'\n", pos[0]);
        return 2;
    }

    char open_err[256];
    device_t *d = device_open(port, vals[2], open_err, sizeof(open_err));
    if (d == NULL) {
        fprintf(stderr, "capsula-keeper: %s\n", open_err);
        return 1;
    }
    int rc = run_log_stream(d, vals[0], vals[1], vals[3], vals[4] != NULL);
    device_close(d);
    return rc;
}

/* --- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *port = getenv("KEEPER_PORT");
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("keeper-control %s\n", KEEPER_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            break;
        }
    }
    if (i >= argc) {
        usage(stderr);
        return 2;
    }
    const char *cmdname = argv[i];
    int rest_argc = argc - i - 1;
    char **rest_argv = argv + i + 1;

    if (strcmp(cmdname, "ports") == 0) {
        return cmd_ports();
    }
    if (strcmp(cmdname, "usb") == 0) {
        return cmd_usb();
    }
    if (strcmp(cmdname, "ping") == 0) {
        return run_cmd_retry(port, "PING", 5000, 2);
    }
    if (strcmp(cmdname, "status") == 0) {
        return run_cmd_retry(port, "STAT", 5000, 2);
    }
    if (strcmp(cmdname, "reset") == 0) {
        return cmd_reset(rest_argc, rest_argv, port);
    }
    if (strcmp(cmdname, "loader") == 0) {
        return cmd_loader(rest_argc, rest_argv, port);
    }
    if (strcmp(cmdname, "maskrom") == 0) {
        return cmd_maskrom(rest_argc, rest_argv, port);
    }
    if (strcmp(cmdname, "press") == 0) {
        return cmd_press(rest_argc, rest_argv, port);
    }
    if (strcmp(cmdname, "release") == 0) {
        return cmd_release(rest_argc, rest_argv, port);
    }
    if (strcmp(cmdname, "baud") == 0) {
        return cmd_baud(rest_argc, rest_argv, port);
    }
    if (strcmp(cmdname, "send") == 0) {
        return cmd_send(rest_argc, rest_argv, port);
    }
    if (strcmp(cmdname, "telnet") == 0) {
        return cmd_telnet(rest_argc, rest_argv, port);
    }
    if (strcmp(cmdname, "log") == 0) {
        return cmd_log(rest_argc, rest_argv, port);
    }
    fprintf(stderr, "capsula-keeper: unknown command '%s'\n\n", cmdname);
    usage(stderr);
    return 2;
}
