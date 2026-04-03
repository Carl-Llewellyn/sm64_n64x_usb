#ifndef SM64_USB_PROTOCOL_H
#define SM64_USB_PROTOCOL_H

#include <PR/ultratypes.h>

#include "config.h"

#define SM64_USB_SYNC0        0x53 // 'S'
#define SM64_USB_SYNC1        0x4D // 'M'
#define SM64_USB_VERSION      1
#define SM64_USB_MAX_PLAYERS  MAX_PLAYERS

#define SM64_USB_PLAYER_PACKET_SIZE 30
#define SM64_USB_PACKET_SIZE        SM64_USB_PLAYER_PACKET_SIZE /* compatibility alias */

#define SM64_USB_TRANSPORT_SYNC0      SM64_USB_SYNC0
#define SM64_USB_TRANSPORT_SYNC1      SM64_USB_SYNC1
#define SM64_USB_TRANSPORT_VERSION    SM64_USB_VERSION
#define SM64_USB_TRANSPORT_HEADER_SIZE 6
#define SM64_USB_IO_BUFFER_SIZE       256
#define SM64_USB_TRANSPORT_MAX_PAYLOAD (SM64_USB_IO_BUFFER_SIZE - SM64_USB_TRANSPORT_HEADER_SIZE)

#define SM64_USB_T_O_SYNC0      0
#define SM64_USB_T_O_SYNC1      1
#define SM64_USB_T_O_VERSION    2
#define SM64_USB_T_O_FLAGS      3
#define SM64_USB_T_O_LEN        4 /* big-endian u16 payload length */

/* Fixed player block inside transport payload (no inner header). */
#define SM64_USB_FIXED_PLAYER_BLOCK_SIZE 30
#define SM64_USB_FP_O_PLAYER_ID   0
#define SM64_USB_FP_O_X           1
#define SM64_USB_FP_O_Y           5
#define SM64_USB_FP_O_Z           9
#define SM64_USB_FP_O_PITCH       13
#define SM64_USB_FP_O_YAW         15
#define SM64_USB_FP_O_ROLL        17
#define SM64_USB_FP_O_CAM_YAW     19
#define SM64_USB_FP_O_BUTTONS     21
#define SM64_USB_FP_O_STICK_X     23
#define SM64_USB_FP_O_STICK_Y     24
#define SM64_USB_FP_O_LEVEL       25

// byte offsets in the wire packet (big-endian for multi-byte fields)
#define SM64_USB_O_SYNC0       0   // S
#define SM64_USB_O_SYNC1       1   // M
#define SM64_USB_O_VERSION     2   // version is put here for sanity
#define SM64_USB_O_PLAYER_ID   3   // P1, P2, P3, etc.
#define SM64_USB_O_X           4   // 4 bytes (BE)
#define SM64_USB_O_Y           8   // 4 bytes (BE)
#define SM64_USB_O_Z           12  // 4 bytes (BE)
#define SM64_USB_O_PITCH       16  // 2 bytes (BE, s16)
#define SM64_USB_O_YAW         18  // 2 bytes (BE, s16)
#define SM64_USB_O_ROLL        20  // 2 bytes (BE, s16)
#define SM64_USB_O_CAM_YAW     22  // 2 bytes (BE, s16)
#define SM64_USB_O_BUTTONS     24  // 2 bytes (BE)
#define SM64_USB_O_STICK_X     26  // 1 byte (s8)
#define SM64_USB_O_STICK_Y     27  // 1 byte (s8)
#define SM64_USB_O_LEVEL       28  // 1 byte (u8)
#define SM64_USB_O_RESERVED    29  // 1 byte (29)

/* ---- Compatibility helpers for IDO (C89-ish) ---- */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define SM64USB_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
/* msg is ignored in this mode, but keeps callsites readable */
#define SM64USB_STATIC_ASSERT(cond, msg) \
    typedef char sm64usb_static_assert_##__LINE__[(cond) ? 1 : -1]
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
#define SM64USB_INLINE static inline
#else
#define SM64USB_INLINE static
#endif
/* ----------------------------------------------- */

typedef struct {
    u8 b[SM64_USB_PLAYER_PACKET_SIZE];
} Sm64UsbPacket;

SM64USB_STATIC_ASSERT(sizeof(Sm64UsbPacket) == SM64_USB_PLAYER_PACKET_SIZE,
                      "Sm64UsbPacket must be 30 bytes");

SM64USB_INLINE u16 sm64usb_read_be16(const u8 *p) {
    return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}

SM64USB_INLINE s32 sm64usb_read_be32(const u8 *p) {
    u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    return (s32)v;
}

SM64USB_INLINE void sm64usb_write_be16(u8 *p, u16 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v);
}

SM64USB_INLINE void sm64usb_write_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)(v);
}

#endif
