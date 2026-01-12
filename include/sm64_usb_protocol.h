#ifndef SM64_USB_PROTOCOL_H
#define SM64_USB_PROTOCOL_H

#include <PR/ultratypes.h>

#define SM64_USB_SYNC0        0x53 /* 'S' */
#define SM64_USB_SYNC1        0x4D /* 'M' */
#define SM64_USB_VERSION      1
#define SM64_USB_MAX_PLAYERS  4

/* wire size is fixed and MUST NOT depend on compiler packing/alignment. */
#define SM64_USB_PACKET_SIZE  22

/* byte offsets in the wire packet (big-endian for multi-byte fields) */
#define SM64_USB_O_SYNC0       0
#define SM64_USB_O_SYNC1       1
#define SM64_USB_O_VERSION     2
#define SM64_USB_O_PLAYER_ID   3
#define SM64_USB_O_X           4   /* 4 bytes (BE) */
#define SM64_USB_O_Y           8   /* 4 bytes (BE) */
#define SM64_USB_O_Z           12  /* 4 bytes (BE) */
#define SM64_USB_O_BUTTONS     16  /* 2 bytes (BE) */
#define SM64_USB_O_STICK_X     18  /* 1 byte (s8) */
#define SM64_USB_O_STICK_Y     19  /* 1 byte (s8) */
#define SM64_USB_O_RESERVED    20  /* 2 bytes (20..21) */

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
    u8 b[SM64_USB_PACKET_SIZE];
} Sm64UsbPacket;

SM64USB_STATIC_ASSERT(sizeof(Sm64UsbPacket) == SM64_USB_PACKET_SIZE,
                      "Sm64UsbPacket must be 22 bytes");

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
