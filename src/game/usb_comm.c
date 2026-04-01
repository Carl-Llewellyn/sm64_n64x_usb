#include "usb_comm.h"

#include <PR/ultratypes.h>
#include <PR/os_cont.h>

#include "game_init.h"
#include "level_update.h"
#include "object_fields.h"
#include "object_list_processor.h"
#include "macros.h"
#include "engine/graph_node.h"

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
    s16 pitch;
    s16 yaw;
    s16 roll;
    s16 cam_yaw;
    u8  level;
    u32 last_seen_frame;
} Sm64UsbRemoteState;

static Sm64UsbRemoteState sRemoteStates[SM64_USB_MAX_PLAYERS];
static u8  sParseBuf[SM64_USB_PACKET_SIZE];
static u32 sParseIndex = 0;

#ifndef SM64_USB_POS_SNAP_THRESHOLD
#define SM64_USB_POS_SNAP_THRESHOLD 600.0f
#endif

#ifndef SM64_USB_POS_MAX_ABS
#define SM64_USB_POS_MAX_ABS 30000.0f
#endif

#ifndef SM64_USB_ROT_SNAP_THRESHOLD
#define SM64_USB_ROT_SNAP_THRESHOLD 0x200
#endif

static f32 sm64usb_s32_to_f32(s32 v) {
    union {
        s32 i;
        f32 f;
    } u;
    u.i = v;
    return u.f;
}

static s32 sm64usb_abs_s16(s16 v) {
    return (v < 0) ? -(s32)v : (s32)v;
}

static int sm64usb_pos_valid(f32 x, f32 y, f32 z) {
    if (x != x || y != y || z != z) {
        return 0;
    }
    if (x < -SM64_USB_POS_MAX_ABS || x > SM64_USB_POS_MAX_ABS) return 0;
    if (y < -SM64_USB_POS_MAX_ABS || y > SM64_USB_POS_MAX_ABS) return 0;
    if (z < -SM64_USB_POS_MAX_ABS || z > SM64_USB_POS_MAX_ABS) return 0;
    return 1;
}

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

static void usb_comm_apply_remote_position(u8 slot, const Sm64UsbRemoteState *state) {
    struct MarioState *m = &gMarioStates[slot];
    struct Object *obj = gMarioObjects[slot];
    f32 x = sm64usb_s32_to_f32(state->x);
    f32 y = sm64usb_s32_to_f32(state->y);
    f32 z = sm64usb_s32_to_f32(state->z);
    f32 dx = x - m->pos[0];
    f32 dy = y - m->pos[1];
    f32 dz = z - m->pos[2];
    f32 dist2 = dx * dx + dy * dy + dz * dz;
    f32 thresh = SM64_USB_POS_SNAP_THRESHOLD;
    s32 rotThresh = SM64_USB_ROT_SNAP_THRESHOLD;

    if (state->level != (u8)gCurrLevelNum) {
        return;
    }
    if (obj == NULL) {
        return;
    }
    if (!sm64usb_pos_valid(x, y, z)) {
        return;
    }
    if (dist2 > (thresh * thresh)) {
        m->pos[0] = x;
        m->pos[1] = y;
        m->pos[2] = z;

        obj->oPosX = x;
        obj->oPosY = y;
        obj->oPosZ = z;
        obj->header.gfx.pos[0] = x;
        obj->header.gfx.pos[1] = y;
        obj->header.gfx.pos[2] = z;
    }

    if (sm64usb_abs_s16((s16)(state->pitch - m->faceAngle[0])) > rotThresh) {
        m->faceAngle[0] = state->pitch;
        obj->oMoveAnglePitch = state->pitch;
        obj->header.gfx.angle[0] = state->pitch;
    }
    if (sm64usb_abs_s16((s16)(state->yaw - m->faceAngle[1])) > rotThresh) {
        m->faceAngle[1] = state->yaw;
        obj->oMoveAngleYaw = state->yaw;
        obj->header.gfx.angle[1] = state->yaw;
    }
    if (sm64usb_abs_s16((s16)(state->roll - m->faceAngle[2])) > rotThresh) {
        m->faceAngle[2] = state->roll;
        obj->oMoveAngleRoll = state->roll;
        obj->header.gfx.angle[2] = state->roll;
    }
}

int usb_comm_get_remote_cam_yaw(u8 slot, s16 *outYaw) {
    u32 now = gGlobalTimer;
    u8 player_id;

    if (outYaw == NULL) {
        return 0;
    }
    if (slot == 0) {
        return 0;
    }

    player_id = (u8)(slot - 1);
    if (player_id >= SM64_USB_MAX_PLAYERS) {
        return 0;
    }
    if (!sRemoteStates[player_id].valid) {
        return 0;
    }
    if ((u32)(now - sRemoteStates[player_id].last_seen_frame) > (u32)SM64_USB_STALE_FRAMES) {
        return 0;
    }

    *outYaw = sRemoteStates[player_id].cam_yaw;
    return 1;
}

/* Store already-decoded fields (host-endian), so the rest of the game never worries about byte order. */
static void usb_comm_store_decoded(u8 player_id, u16 buttons, s8 stick_x, s8 stick_y, s32 x, s32 y, s32 z,
                                   s16 pitch, s16 yaw, s16 roll, s16 cam_yaw, u8 level) {
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
    sRemoteStates[player_id].pitch = pitch;
    sRemoteStates[player_id].yaw = yaw;
    sRemoteStates[player_id].roll = roll;
    sRemoteStates[player_id].cam_yaw = cam_yaw;
    sRemoteStates[player_id].level = level;
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
                s16 pitch;
                s16 yaw;
                s16 roll;
                s16 cam_yaw;
                s8  stick_x, stick_y;
                u8  level;

                sm64usb_memcpy(pkt.b, sParseBuf, (u32)SM64_USB_PACKET_SIZE);

                pid     = pkt.b[SM64_USB_O_PLAYER_ID];
                x       = sm64usb_read_be32(&pkt.b[SM64_USB_O_X]);
                y       = sm64usb_read_be32(&pkt.b[SM64_USB_O_Y]);
                z       = sm64usb_read_be32(&pkt.b[SM64_USB_O_Z]);
                pitch   = (s16)sm64usb_read_be16(&pkt.b[SM64_USB_O_PITCH]);
                yaw     = (s16)sm64usb_read_be16(&pkt.b[SM64_USB_O_YAW]);
                roll    = (s16)sm64usb_read_be16(&pkt.b[SM64_USB_O_ROLL]);
                cam_yaw = (s16)sm64usb_read_be16(&pkt.b[SM64_USB_O_CAM_YAW]);
                buttons = sm64usb_read_be16(&pkt.b[SM64_USB_O_BUTTONS]);
                stick_x = (s8)pkt.b[SM64_USB_O_STICK_X];
                stick_y = (s8)pkt.b[SM64_USB_O_STICK_Y];
                level   = pkt.b[SM64_USB_O_LEVEL];

                usb_comm_store_decoded(pid, buttons, stick_x, stick_y, x, y, z, pitch, yaw, roll, cam_yaw, level);
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
        struct Object *obj = NULL;

        if (!sRemoteStates[player_id].valid) {
            continue;
        }

        /* Drop stale controllers so they don't "stick" forever if the sender disappears. */
        if ((u32)(now - sRemoteStates[player_id].last_seen_frame) > (u32)SM64_USB_STALE_FRAMES) {
            sRemoteStates[player_id].valid = 0;
            continue;
        }
        obj = gMarioObjects[slot];
        if (sRemoteStates[player_id].level != (u8)gCurrLevelNum) {
            if (obj != NULL && obj != gMarioObjects[0]) {
                obj->header.gfx.node.flags |= GRAPH_RENDER_INVISIBLE;
            }
            continue;
        }
        if (obj != NULL && obj != gMarioObjects[0]) {
            obj->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
        }

        if (slot < ARRAY_COUNT(gControllers)) {
            usb_comm_update_controller(&gControllers[slot],
                                       sRemoteStates[player_id].buttons,
                                       sRemoteStates[player_id].stick_x,
                                       sRemoteStates[player_id].stick_y);
#if SM64_USB_APPLY_REMOTE_POS
            usb_comm_apply_remote_position(slot, &sRemoteStates[player_id]);
#endif
        }
    }

}
