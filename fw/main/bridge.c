#include "bridge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "buttons.h"
#include "pins.h"
#include "proto.h"

#define LOG_CHUNK 512
#define CONSOLE_BATCH 128
#define CMD_LINE_MAX 96

#define LOG_TASK_STACK 4096
#define CMD_TASK_STACK 4096

#define USJ_RX_BUF 4096
#define USJ_TX_BUF 8192
#define UART_RX_BUF 8192
#define UART_TX_BUF 1024

#define DEFAULT_RESET_MS 1000
// The download key is checked by U-BOOT (not the BootROM) on rk3308-evb
// boards, after TPL/SPL and DDR init — so recovery must stay held well
// after reset is released, see boot_mode.c: rockchip_dnl_mode_check().
#define LOADER_PRE_MS 200
#define LOADER_RESET_MS 1000
#define LOADER_POST_MS 3000

// MaskROM forcing: FLASH_D0 shorted while the BootROM attempts to read the
// bootloader; release shortly after — before the kernel touches the NAND.
#define MASKROM_PRE_MS 100
#define MASKROM_RESET_MS 1000
#define MASKROM_POST_MS 1500

static volatile uint32_t s_drops = 0;
static volatile uint32_t s_rx_total = 0;
static volatile uint32_t s_baud = TARGET_UART_BAUD;

// CTRL replies must get through even when the LOG stream saturates the USJ
// TX buffer: wait much longer than a log chunk ever would.
static void send_ctrl(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    uint8_t hdr[4] = {PKT_SOF, PKT_TYPE_CTRL, (uint8_t)(n & 0xFF),
                      (uint8_t)((n >> 8) & 0xFF)};
    int w1 = usb_serial_jtag_write_bytes(hdr, sizeof(hdr), pdMS_TO_TICKS(2000));
    int w2 = (w1 == (int)sizeof(hdr))
                 ? usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(2000))
                 : 0;
    if (w1 != (int)sizeof(hdr) || w2 != n) {
        s_drops++;
    }
}

// Logs are best-effort: never block long on a full TX buffer, that starves
// the command task and delays CTRL replies (the host resyncs on the next
// SOF). Undelivered chunks are counted as drops.
static void send_log(const uint8_t *data, size_t len)
{
    uint8_t hdr[4] = {PKT_SOF, PKT_TYPE_LOG, (uint8_t)(len & 0xFF),
                      (uint8_t)(len >> 8)};
    int w1 = usb_serial_jtag_write_bytes(hdr, sizeof(hdr), pdMS_TO_TICKS(10));
    int w2 = (w1 == (int)sizeof(hdr))
                 ? usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(20))
                 : 0;
    if (w1 != (int)sizeof(hdr) || w2 != (int)len) {
        s_drops++;
    }
}

// Target UART -> host
static void log_task(void *arg)
{
    (void)arg;
    uint8_t buf[LOG_CHUNK];
    for (;;) {
        int n = uart_read_bytes(TARGET_UART_NUM, buf, sizeof(buf),
                                pdMS_TO_TICKS(20));
        if (n > 0) {
            s_rx_total += n;
            send_log(buf, n);
        }
    }
}

static uint32_t parse_ms_or(const char *s, uint32_t def)
{
    if (s == NULL || *s == '\0') {
        return def;
    }
    long v = atol(s);
    if (v <= 0 || v > 60000) {
        return def;
    }
    return (uint32_t)v;
}

static void finish_seq(const char *what, uint32_t budget_ms)
{
    if (!buttons_wait_done(budget_ms + 2000)) {
        send_ctrl("ERR timeout waiting for %s sequence", what);
        return;
    }
    send_ctrl("OK %s done", what);
}

static void handle_cmd(char *line)
{
    char *save = NULL;
    const char *cmd = strtok_r(line, " \t", &save);
    if (cmd == NULL) {
        send_ctrl("ERR empty command");
        return;
    }

    if (!strcasecmp(cmd, "PING")) {
        send_ctrl("OK pong");
        return;
    }
    if (!strcasecmp(cmd, "VER")) {
        send_ctrl("OK %s", FW_VERSION);
        return;
    }
    if (!strcasecmp(cmd, "STAT")) {
        send_ctrl("OK up_s=%lu baud=%lu drops=%lu rx_total=%lu rst=%s rec=%s "
                  "msk=%s",
                  (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ),
                  (unsigned long)s_baud, (unsigned long)s_drops,
                  (unsigned long)s_rx_total,
                  btn_is_pressed(BTN_RESET) ? "press" : "release",
                  btn_is_pressed(BTN_RECOVERY) ? "press" : "release",
                  btn_is_pressed(BTN_MASKROM) ? "press" : "release");
        return;
    }
    if (!strcasecmp(cmd, "BAUD")) {
        const char *arg = strtok_r(NULL, " \t", &save);
        if (arg == NULL) {
            send_ctrl("ERR baud: missing value");
            return;
        }
        uint32_t b = (uint32_t)atol(arg);
        if (b < 1200 || b > 5000000) {
            send_ctrl("ERR baud out of range");
            return;
        }
        uart_set_baudrate(TARGET_UART_NUM, b);
        s_baud = b;
        send_ctrl("OK baud=%lu", (unsigned long)b);
        return;
    }
    if (!strcasecmp(cmd, "RESET")) {
        uint32_t ms = parse_ms_or(strtok_r(NULL, " \t", &save),
                                  DEFAULT_RESET_MS);
        if (buttons_timed_press(BTN_RESET, ms) != 0) {
            send_ctrl("ERR busy");
            return;
        }
        finish_seq("reset", ms);
        return;
    }
    if (!strcasecmp(cmd, "PRESS") || !strcasecmp(cmd, "RELEASE")) {
        btn_t b;
        if (btn_from_name(strtok_r(NULL, " \t", &save), &b) != 0) {
            send_ctrl("ERR unknown button (reset|recovery|maskrom)");
            return;
        }
        if (!strcasecmp(cmd, "RELEASE")) {
            btn_release(b);
            send_ctrl("OK released");
            return;
        }
        const char *ms_s = strtok_r(NULL, " \t", &save);
        if (ms_s != NULL) {
            uint32_t ms = parse_ms_or(ms_s, DEFAULT_RESET_MS);
            if (buttons_timed_press(b, ms) != 0) {
                send_ctrl("ERR busy");
                return;
            }
            finish_seq("press", ms);
        } else {
            btn_press(b);
            send_ctrl("OK pressed");
        }
        return;
    }
    if (!strcasecmp(cmd, "LOADER")) {
        uint32_t pre = parse_ms_or(strtok_r(NULL, " \t", &save), LOADER_PRE_MS);
        uint32_t rst =
            parse_ms_or(strtok_r(NULL, " \t", &save), LOADER_RESET_MS);
        uint32_t post =
            parse_ms_or(strtok_r(NULL, " \t", &save), LOADER_POST_MS);
        if (buttons_run_loader(pre, rst, post) != 0) {
            send_ctrl("ERR busy");
            return;
        }
        finish_seq("loader", pre + rst + post);
        return;
    }
    if (!strcasecmp(cmd, "MASKROM")) {
        uint32_t pre = parse_ms_or(strtok_r(NULL, " \t", &save), MASKROM_PRE_MS);
        uint32_t rst =
            parse_ms_or(strtok_r(NULL, " \t", &save), MASKROM_RESET_MS);
        uint32_t post =
            parse_ms_or(strtok_r(NULL, " \t", &save), MASKROM_POST_MS);
        if (buttons_run_maskrom(pre, rst, post) != 0) {
            send_ctrl("ERR busy");
            return;
        }
        finish_seq("maskrom", pre + rst + post);
        return;
    }
    send_ctrl("ERR unknown command: %s", cmd);
}

// Host -> target: forward console bytes, intercept "@CMD" lines ("@@" passes
// a literal '@').
static void cmd_task(void *arg)
{
    (void)arg;
    uint8_t buf[128];
    uint8_t out[CONSOLE_BATCH];
    size_t outn = 0;
    char line[CMD_LINE_MAX];
    size_t len = 0;
    bool in_cmd = false;

    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf),
                                           pdMS_TO_TICKS(50));
        if (n <= 0) {
            if (outn > 0) {
                uart_write_bytes(TARGET_UART_NUM, out, outn);
                outn = 0;
            }
            continue;
        }
        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (in_cmd) {
                if (b == '\n' || b == '\r') {
                    line[len] = '\0';
                    handle_cmd(line);
                    in_cmd = false;
                    len = 0;
                } else if (len + 1 < sizeof(line)) {
                    line[len++] = (char)b;
                } else {
                    line[len] = '\0';
                    handle_cmd(line);
                    in_cmd = false;
                    len = 0;
                }
            } else if (b == '@') {
                if (i + 1 < n && buf[i + 1] == '@') {
                    out[outn++] = '@';
                    i++;
                } else {
                    in_cmd = true;
                    len = 0;
                }
            } else {
                out[outn++] = b;
                if (outn == sizeof(out)) {
                    uart_write_bytes(TARGET_UART_NUM, out, outn);
                    outn = 0;
                }
            }
        }
        if (outn > 0) {
            uart_write_bytes(TARGET_UART_NUM, out, outn);
            outn = 0;
        }
    }
}

void bridge_init(void)
{
    uart_config_t ucfg = {
        .baud_rate = TARGET_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(TARGET_UART_NUM, &ucfg);
    uart_set_pin(TARGET_UART_NUM, PIN_TARGET_UART_TX, PIN_TARGET_UART_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // Idle-high pull-up on RX: silences noise while the target is unplugged
    // (UART idle level is high).
    gpio_pullup_en((gpio_num_t)PIN_TARGET_UART_RX);
    uart_driver_install(TARGET_UART_NUM, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0);

    usb_serial_jtag_driver_config_t scfg = {
        .rx_buffer_size = USJ_RX_BUF,
        .tx_buffer_size = USJ_TX_BUF,
    };
    usb_serial_jtag_driver_install(&scfg);
}

void bridge_start(void)
{
    // The command task runs ABOVE the log forwarder: when the target floods
    // the UART the logfwd task would otherwise monopolize the CPU and the
    // USJ TX buffer, delaying @command handling and dropping replies.
    xTaskCreate(cmd_task, "cmd", CMD_TASK_STACK, NULL, 6, NULL);
    xTaskCreate(log_task, "logfwd", LOG_TASK_STACK, NULL, 5, NULL);
}
