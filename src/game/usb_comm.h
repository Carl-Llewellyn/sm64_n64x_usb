#ifndef USB_COMM_H
#define USB_COMM_H

#include <PR/ultratypes.h>

#include "types.h"

// USB controller injection protocol for P2/P3 inputs.
//
// Usage:
// 1) Receive a full packet from USB serial into a byte buffer.
// 2) Call usb_comm_parse_and_apply(buffer, length) once per frame,
//    after read_controller_inputs() runs so USB inputs override pads.
// 3) The parser writes to gControllers[1] (P2) and gControllers[2] (P3).
//
// If you already have inputs decoded, call usb_comm_apply_player_input()
// directly per player index (0->P2, 1->P3).
//
// Test hook:
// - See usb_comm_apply_player_input(): holding L+R+Z for P3 forces
//   a leftward stick input to verify the USB path is wired correctly.
#define USB_COMM_SYNC0 0xA5
#define USB_COMM_SYNC1 0x5A
#define USB_COMM_VERSION 1

// Packet layout:
// [0]  sync0
// [1]  sync1
// [2]  version
// [3]  sequence
// [4]  player_count (records for P2..)
// [5]  flags
// [6..] player records (u16 buttons_be, s8 stick_x, s8 stick_y)
// [end-2..end-1] crc16 (big endian, CRC-16/CCITT over bytes [2..end-3])
#define USB_COMM_HEADER_SIZE 6
#define USB_COMM_RECORD_SIZE 4
#define USB_COMM_CRC_SIZE 2

enum UsbCommResult {
    USB_COMM_OK = 0,
    USB_COMM_ERR_SHORT = -1,
    USB_COMM_ERR_SYNC = -2,
    USB_COMM_ERR_VERSION = -3,
    USB_COMM_ERR_LEN = -4,
    USB_COMM_ERR_CRC = -5,
};

void usb_comm_reset(void);

// Parses and applies a packet to controller slots 1.. (P2/P3).
// Call after read_controller_inputs() so serial inputs override pads.
s32 usb_comm_parse_and_apply(const u8 *data, u16 len);

// Directly injects one player's inputs (player_index: 0->P2, 1->P3).
void usb_comm_apply_player_input(u8 player_index, u16 buttons, s8 stick_x, s8 stick_y);

// Test helper: builds a valid packet (P2 neutral, P3 stick left) and feeds it
// through usb_comm_parse_and_apply(). Use this to verify the packet parser path.
void usb_comm_test_inject_p3_left(void);

#endif // USB_COMM_H
