#pragma once

#include <stddef.h>
#include <stdint.h>

#define KEEPER_VERSION "1.0.0"

/* Wire protocol (device -> host), mirrors fw/main/proto.h:
 *   [SOF 0x5A][type u8][len u16 LE][payload...]
 * Host -> device: raw byte stream; lines starting with '@' are commands
 * ("@@" passes a literal '@' to the target console). */
#define PKT_SOF 0x5A
#define PKT_TYPE_LOG 0x01
#define PKT_TYPE_CTRL 0x02
#define PKT_LEN_MAX 4096

/* --- proto.c: incremental frame parser with resync --- */

typedef struct {
    uint8_t buf[2 * PKT_LEN_MAX + 8];
    size_t len;
} parser_t;

void parser_init(parser_t *p);

/* Append bytes and invoke cb() for every complete frame. */
void parser_feed(parser_t *p, const uint8_t *data, size_t n,
                 void (*cb)(uint8_t type, const uint8_t *payload, size_t len,
                            void *user),
                 void *user);

/* --- tty.c --- */

typedef struct {
    char dev[280];
    uint16_t vid;
    uint16_t pid;
} tty_port_t;

/* Espressif USB IDs (USB-Serial-JTAG peripheral: 303a:1001). */
#define ESPRESSIF_VID 0x303A
#define ESPRESSIF_USJ_PID 0x1001

/* Scan /sys/class/tty for Espressif USB-serial ports. Returns the count
 * found (may exceed max; only max entries are filled in). */
int tty_scan_espressif(tty_port_t *out, int max);

/* Open a serial port in raw, non-blocking mode; deasserts DTR/RTS.
 * Returns fd or -1 (errno set). */
int tty_open(const char *path);

/* --- device.c --- */

typedef struct device device_t;

/* port == NULL: auto-detect. capture_path == NULL: no tee file.
 * On failure returns NULL and puts a human-readable reason into err. */
device_t *device_open(const char *port, const char *capture_path, char *err,
                      size_t err_sz);
void device_close(device_t *d);

/* Last I/O-level error message of this device ("" if none). */
const char *device_err(const device_t *d);

/* Send "@cmd" and wait for the CTRL reply.
 * Returns 0 and fills reply ("OK ...") on success;
 * 1 if the dongle replied "ERR ..." (text in reply);
 * -1 on timeout or I/O error (reason in reply). */
int device_command(device_t *d, const char *cmd, long timeout_ms, char *reply,
                   size_t reply_sz);

/* Raw bytes straight to the target console (must not start with '@'). */
int device_send_console(device_t *d, const uint8_t *data, size_t n);

/* Invoke cb() for every LOG frame until cb returns nonzero, *stop is set,
 * or timeout_ms elapses (timeout_ms <= 0: no deadline).
 * Returns 0 on normal end, 2 on timeout, -1 on I/O error. */
int device_log_loop(device_t *d,
                    int (*cb)(const uint8_t *data, size_t len, void *user),
                    void *user, const volatile int *stop, long timeout_ms);

/* --- usb.c: detection of Rockchip download modes on the host (sysfs only) --- */

#define USB_VID_ROCKCHIP 0x2207
#define USB_PID_MASKROM_RK3308 0x330E  /* RK3308 BootROM */
#define USB_PID_MASKROM_GENERIC 0x0011 /* classic Rockchip BootROM */
#define USB_PID_LOADER_CLASSIC 0x0010  /* miniloader/loader mode */

/* List PIDs of all connected 2207:* devices (deduplicated). Returns count. */
int usb_list_rockchip(unsigned *pids, int max);

/* Wait up to timeout_ms for any of `want` PIDs to appear on the bus.
 * Returns 1 and fills *found_pid on success, 0 on timeout. */
int usb_wait_rockchip(const unsigned *want, int nwant, int timeout_ms,
                      unsigned *found_pid);
