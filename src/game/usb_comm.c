#include "usb_comm.h"

#include <PR/ultratypes.h>
#include <PR/os_cont.h>

#include "game_init.h"
#include "macros.h"

#ifdef TARGET_PS2
#include <kernel.h>
#endif

extern void adjust_analog_stick(struct Controller *controller);

/* how long (in frames) a remote player's last packet stays valid. */
#ifndef SM64_USB_STALE_FRAMES
#define SM64_USB_STALE_FRAMES 30
#endif

typedef struct {
    u8  valid;
    u16 buttons;
    s8  stick_x;
    s8  stick_y;
    s32 x;
    s32 y;
    s32 z;
    u32 last_seen_frame;
} Sm64UsbRemoteState;

static Sm64UsbRemoteState sRemoteStates[SM64_USB_MAX_PLAYERS];
static u8  sParseBuf[SM64_USB_PACKET_SIZE];
static u32 sParseIndex = 0;

static void sm64usb_memcpy(void *dst, const void *src, u32 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}
static void usb_comm_update_controller(struct Controller *controller, u16 buttons, s8 stick_x, s8 stick_y) {
    controller->rawStickX = stick_x;
    controller->rawStickY = stick_y;
    controller->buttonPressed = buttons & (buttons ^ controller->buttonDown);
    controller->buttonDown = buttons;
    adjust_analog_stick(controller);
}

/* Store already-decoded fields (host-endian), so the rest of the game never worries about byte order. */
static void usb_comm_store_decoded(u8 player_id, u16 buttons, s8 stick_x, s8 stick_y, s32 x, s32 y, s32 z) {
    if (player_id >= SM64_USB_MAX_PLAYERS) {
        return;
    }
    sRemoteStates[player_id].valid = 1;
    sRemoteStates[player_id].buttons = buttons;
    sRemoteStates[player_id].stick_x = stick_x;
    sRemoteStates[player_id].stick_y = stick_y;
    sRemoteStates[player_id].x = x;
    sRemoteStates[player_id].y = y;
    sRemoteStates[player_id].z = z;
    sRemoteStates[player_id].last_seen_frame = gGlobalTimer;
}

void usb_comm_consume_bytes(const u8 *data, u32 len) {
    u32 i;

    if (!data || len == 0) return;

    for (i = 0; i < len; i++) {
        u8 byte = data[i];

        if (sParseIndex == 0) {
            if (byte != SM64_USB_SYNC0) continue;
            sParseBuf[sParseIndex++] = byte;
            continue;
        }

        if (sParseIndex == 1) {
            if (byte != SM64_USB_SYNC1) { sParseIndex = 0; continue; }
            sParseBuf[sParseIndex++] = byte;
            continue;
        }

        sParseBuf[sParseIndex++] = byte;

        if (sParseIndex >= SM64_USB_PACKET_SIZE) {
            if (sParseBuf[SM64_USB_O_SYNC0] == SM64_USB_SYNC0 &&
                sParseBuf[SM64_USB_O_SYNC1] == SM64_USB_SYNC1 &&
                sParseBuf[SM64_USB_O_VERSION] == SM64_USB_VERSION) {

                Sm64UsbPacket pkt;
                u8  pid;
                s32 x, y, z;
                u16 buttons;
                s8  stick_x, stick_y;

                sm64usb_memcpy(pkt.b, sParseBuf, (u32)SM64_USB_PACKET_SIZE);

                pid     = pkt.b[SM64_USB_O_PLAYER_ID];
                x       = sm64usb_read_be32(&pkt.b[SM64_USB_O_X]);
                y       = sm64usb_read_be32(&pkt.b[SM64_USB_O_Y]);
                z       = sm64usb_read_be32(&pkt.b[SM64_USB_O_Z]);
                buttons = sm64usb_read_be16(&pkt.b[SM64_USB_O_BUTTONS]);
                stick_x = (s8)pkt.b[SM64_USB_O_STICK_X];
                stick_y = (s8)pkt.b[SM64_USB_O_STICK_Y];

                usb_comm_store_decoded(pid, buttons, stick_x, stick_y, x, y, z);
            }

            sParseIndex = 0;
        }
    }
}

void usb_comm_apply_remote_inputs(void) {
    u8 player_id;
    const u32 now = gGlobalTimer;

    for (player_id = 0; player_id < SM64_USB_MAX_PLAYERS; player_id++) {
        u8 slot = (u8)(player_id + 1);

        if (!sRemoteStates[player_id].valid) {
            continue;
        }

        /* Drop stale controllers so they don't "stick" forever if the sender disappears. */
        if ((u32)(now - sRemoteStates[player_id].last_seen_frame) > (u32)SM64_USB_STALE_FRAMES) {
            sRemoteStates[player_id].valid = 0;
            continue;
        }

        if (slot < ARRAY_COUNT(gControllers)) {
            usb_comm_update_controller(&gControllers[slot],
                                       sRemoteStates[player_id].buttons,
                                       sRemoteStates[player_id].stick_x,
                                       sRemoteStates[player_id].stick_y);
        }
    }

}
