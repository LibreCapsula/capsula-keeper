#pragma once

#define FW_VERSION "keeper-fw 0.2.0"

// Wire protocol (device -> host): binary frames
//   [SOF 0x5A][type u8][len u16 LE][payload...]
#define PKT_SOF 0x5A
#define PKT_TYPE_LOG 0x01  // payload: raw bytes from the target UART
#define PKT_TYPE_CTRL 0x02 // payload: ASCII reply to a host command

#define PKT_LEN_MAX 4096
