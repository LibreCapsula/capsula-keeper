#include "buttons.h"

#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pins.h"

typedef enum {
    SEQ_NONE = 0,
    SEQ_TIMED,
    SEQ_LOADER,
    SEQ_MASKROM,
} seq_kind_t;

static const gpio_num_t s_pins[BTN_COUNT] = {PIN_BTN_RESET, PIN_BTN_RECOVERY,
                                             PIN_BTN_MASKROM};
static volatile bool s_pressed[BTN_COUNT];

static SemaphoreHandle_t s_start;
static SemaphoreHandle_t s_done;
static volatile bool s_busy;

static struct {
    seq_kind_t kind;
    btn_t btn;
    uint32_t a, b, c; // SEQ_TIMED: hold ms; SEQ_LOADER: pre/reset/post ms
} s_req;

void btn_press(btn_t b)
{
    if ((int)b >= BTN_COUNT) {
        return;
    }
    s_pressed[b] = true;
    // Button channels: open-drain, "press" = pull low.
    // The maskrom channel drives a MOSFET gate (push-pull): "press" = gate
    // HIGH, which shorts FLASH_D0 to GND through the transistor.
    gpio_set_level(s_pins[b], b == BTN_MASKROM ? 1 : 0);
}

void btn_release(btn_t b)
{
    if ((int)b >= BTN_COUNT) {
        return;
    }
    s_pressed[b] = false;
    gpio_set_level(s_pins[b], b == BTN_MASKROM ? 0 : 1); // OD high = high-Z
}

bool btn_is_pressed(btn_t b)
{
    return (int)b < BTN_COUNT && s_pressed[b];
}

int btn_from_name(const char *name, btn_t *out)
{
    if (name == NULL) {
        return -1;
    }
    if (!strcasecmp(name, "reset") || !strcasecmp(name, "rst")) {
        *out = BTN_RESET;
        return 0;
    }
    if (!strcasecmp(name, "recovery") || !strcasecmp(name, "rec")) {
        *out = BTN_RECOVERY;
        return 0;
    }
    if (!strcasecmp(name, "maskrom") || !strcasecmp(name, "tp48") ||
        !strcasecmp(name, "flash_d0")) {
        *out = BTN_MASKROM;
        return 0;
    }
    return -1;
}

static void seq_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_start, portMAX_DELAY);
        switch (s_req.kind) {
        case SEQ_TIMED:
            btn_press(s_req.btn);
            vTaskDelay(pdMS_TO_TICKS(s_req.a));
            btn_release(s_req.btn);
            break;
        case SEQ_LOADER:
            btn_press(BTN_RECOVERY);
            vTaskDelay(pdMS_TO_TICKS(s_req.a));
            btn_press(BTN_RESET);
            vTaskDelay(pdMS_TO_TICKS(s_req.b));
            btn_release(BTN_RESET);
            vTaskDelay(pdMS_TO_TICKS(s_req.c));
            btn_release(BTN_RECOVERY);
            break;
        case SEQ_MASKROM:
            btn_press(BTN_MASKROM);
            vTaskDelay(pdMS_TO_TICKS(s_req.a));
            btn_press(BTN_RESET);
            vTaskDelay(pdMS_TO_TICKS(s_req.b));
            btn_release(BTN_RESET);
            vTaskDelay(pdMS_TO_TICKS(s_req.c));
            btn_release(BTN_MASKROM);
            break;
        default:
            break;
        }
        s_busy = false;
        xSemaphoreGive(s_done);
    }
}

static int schedule(seq_kind_t kind, btn_t b, uint32_t a, uint32_t b_ms,
                    uint32_t c)
{
    if (s_busy) {
        return -1;
    }
    xSemaphoreTake(s_done, 0); // drain stale token
    s_busy = true;
    s_req.kind = kind;
    s_req.btn = b;
    s_req.a = a;
    s_req.b = b_ms;
    s_req.c = c;
    xSemaphoreGive(s_start);
    return 0;
}

int buttons_timed_press(btn_t b, uint32_t ms)
{
    return schedule(SEQ_TIMED, b, ms, 0, 0);
}

int buttons_run_loader(uint32_t pre_hold_ms, uint32_t reset_hold_ms,
                       uint32_t post_hold_ms)
{
    return schedule(SEQ_LOADER, BTN_RESET, pre_hold_ms, reset_hold_ms,
                    post_hold_ms);
}

int buttons_run_maskrom(uint32_t pre_hold_ms, uint32_t reset_hold_ms,
                        uint32_t post_hold_ms)
{
    return schedule(SEQ_MASKROM, BTN_RESET, pre_hold_ms, reset_hold_ms,
                    post_hold_ms);
}

bool buttons_wait_done(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void buttons_init(void)
{
    // Reset/Recovery: open-drain (never source voltage onto board lines).
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BTN_RESET) | (1ULL << PIN_BTN_RECOVERY),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE, // no bias on the SARADC line
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    btn_release(BTN_RESET);
    btn_release(BTN_RECOVERY);

    // Maskrom: push-pull gate driver for the external MOSFET (fail-safe
    // state is LOW = transistor closed; a 10K gate->GND pulldown covers
    // dongle-off / download-mode when the pin floats).
    gpio_config_t io2 = {
        .pin_bit_mask = 1ULL << PIN_BTN_MASKROM,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io2);
    s_pressed[BTN_MASKROM] = false;
    gpio_set_level(PIN_BTN_MASKROM, 0);

    s_start = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    xTaskCreate(seq_task, "btnseq", 3072, NULL, 5, NULL);
}
