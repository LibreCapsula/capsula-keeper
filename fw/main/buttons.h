#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BTN_RESET = 0,
    BTN_RECOVERY = 1,
    BTN_MASKROM = 2,
    BTN_COUNT,
} btn_t;

// Configure button pins as open-drain outputs, released.
void buttons_init(void);

// Drive the line low ("button held down").
void btn_press(btn_t b);

// Release the line ("button up", open-drain high-Z).
void btn_release(btn_t b);

bool btn_is_pressed(btn_t b);

// Map "reset"/"rst" and "recovery"/"rec"/"maskrom" to btn_t. 0 on success.
int btn_from_name(const char *name, btn_t *out);

// Schedule a timed press. Returns 0 on success, -1 if a sequence is running.
int buttons_timed_press(btn_t b, uint32_t ms);

// Schedule the RK3308 loader entry sequence:
//   recovery down -> pre_hold_ms -> reset down -> reset_hold_ms ->
//   reset up -> post_hold_ms -> recovery up.
// Returns 0 on success, -1 if a sequence is running.
int buttons_run_loader(uint32_t pre_hold_ms, uint32_t reset_hold_ms,
                       uint32_t post_hold_ms);

// Schedule the guaranteed MaskROM entry sequence (short FLASH_D0 to GND
// during boot so the BootROM fails to load the bootloader):
//   maskrom down -> pre_hold_ms -> reset down -> reset_hold_ms ->
//   reset up -> post_hold_ms -> maskrom up.
int buttons_run_maskrom(uint32_t pre_hold_ms, uint32_t reset_hold_ms,
                        uint32_t post_hold_ms);

// Block until the scheduled sequence finishes. Returns true when done.
bool buttons_wait_done(uint32_t timeout_ms);