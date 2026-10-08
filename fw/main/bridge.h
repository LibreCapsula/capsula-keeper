#pragma once

// USB <-> target UART bridge: forwards target logs to the host over USB
// (framed), parses host commands ("@CMD ..." lines) and forwards everything
// else to the target console.
void bridge_init(void);
void bridge_start(void);
