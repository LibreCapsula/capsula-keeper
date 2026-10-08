#pragma once

// ESP32-S3 SuperMini pin assignment.
// All pins are routable via GPIO matrix; remap freely if your wiring differs.
// Avoid S3 strapping pins (0, 3, 45, 46) and USB pins (19, 20).

// UART bridge to the target. On the rk3308-evb ("capsula") board the debug
// console is UART2 (GPIO4_D2/D3 of the SoC, 3.3V domain), brought out at
// test points TP50 (UART2_RX_M1) / TP51 (UART2_TX_M1) and the J3 header.
#define PIN_TARGET_UART_TX 5 // ESP32 -> target UART2_RX (TP50)
#define PIN_TARGET_UART_RX 4 // target UART2_TX (TP51) -> ESP32

// Reset/Recovery lines. Driven open-drain ONLY (idle = floating, "press" =
// pull low). Never configure these as push-pull and never drive them high:
// the recovery line is a SARADC input (1.8V domain).
#define PIN_BTN_RESET 6    // series resistor 330R -> Reset button signal pad
#define PIN_BTN_RECOVERY 7 // series resistor 330R -> Recovery button signal pad

// Maskrom: PUSH-PULL gate driver for an external N-MOSFET (2N7002/AO3400/
// KP505A). "press" = gate HIGH = transistor shorts TP48 (FLASH_D0) to GND,
// BootROM fails to load the bootloader and enters MaskROM (schematic p.20).
// A 10K gate->GND pull-down keeps it closed while the dongle is off/reset.
#define PIN_BTN_MASKROM 8

#define TARGET_UART_NUM UART_NUM_1
#define TARGET_UART_BAUD 1500000
