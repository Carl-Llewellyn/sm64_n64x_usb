#include <string.h>
#include <usb.h>
#include <ultra64.h>
#include <PR/rcp.h>
#include <PR/os.h>
#include <stdio.h>
#include "object_list_processor.h"
#include "sm64.h"
#include "print.h"
#include "sm64_usb_protocol.h"
#include "game_init.h"
#include "level_update.h"
#include "PR/os_pi.h"
#include "sync_object.h"
#include "usb_comm.h"

static char gBuf[64];

#define SM64_USB_DEBUG_PRINT 1
#define SM64_USB_FRAME_PAYLOAD_SIZE (SM64_USB_FIXED_PLAYER_BLOCK_SIZE + SYNC_OBJECT_PACKET_SIZE)
#define SM64_USB_FRAME_SIZE (SM64_USB_TRANSPORT_HEADER_SIZE + SM64_USB_FRAME_PAYLOAD_SIZE)

void __osPiGetAccess(void);
void __osPiRelAccess(void);

static u32 float_to_u32(f32 f) {
    union {
        f32 f;
        u32 u;
    } u;
    u.f = f;
    return u.u;
}

static void sm64usb_zero_bytes(u8 *dst, int len) {
    int i;
    for (i = 0; i < len; i++) {
        dst[i] = 0;
    }
}

static void usb_print_incoming(const u8 *data) {
    int i = 0;
    int y = 12;
    struct SyncObjectDebugState syncDebug;
    u16 buttons = sm64usb_read_be16(&data[SM64_USB_FP_O_BUTTONS]);
    s8 stick_x = (s8)data[SM64_USB_FP_O_STICK_X];
    s8 stick_y = (s8)data[SM64_USB_FP_O_STICK_Y];

    sync_object_get_debug_state(&syncDebug);

    /* sprintf(gBuf, "rx %02X %02X %02X %02X",
            data[0], data[1], data[2], data[3]);
    print_text(0, y, gBuf);
    y += 20; */

    for (i = 4; i < 20; i += 4) {
        /* sprintf(gBuf, "%02d: %02X %02X %02X %02X",
                i, data[i + 0], data[i + 1], data[i + 2], data[i + 3]);
        print_text(0, y, gBuf);
        y += 20; */
    }

    /* sprintf(gBuf, "20: %02X %02X",
            data[20], data[21]);
    print_text(0, y, gBuf);
    y += 20; */

    sprintf(gBuf, "sp%lu ap%lu del%lu act%lu",
            (unsigned long)syncDebug.remoteSpawnCount,
            (unsigned long)syncDebug.remoteApplyCount,
            (unsigned long)syncDebug.remoteDeleteCount,
            (unsigned long)syncDebug.remoteActiveCount);
    print_text(0, y, gBuf);
    y += 20;

    sprintf(gBuf, "id%lu fr%lu",
            (unsigned long)syncDebug.lastRemoteSyncId,
            (unsigned long)syncDebug.lastRemoteFrame);
    print_text(0, y, gBuf);
    y += 20;

    sprintf(gBuf, "x%d y%d z%d",
            (int)syncDebug.lastRemotePosX,
            (int)syncDebug.lastRemotePosY,
            (int)syncDebug.lastRemotePosZ);
    print_text(0, y, gBuf);
}

//split whatever comes in to write 32bit
void send_32_bit(int len, const u8 *data){
    u32 *curr_write_add = (u32 *)CART_SRAM_START;
    int i = 0;
    u32 word = 0;
    int n = 0;

    if (len <= 0 || len > SM64_USB_IO_BUFFER_SIZE) {
        return;
    }

    //32/8 = 4 - so we can send 4 u8s at a time
    for(i = 0; i < len; i+=4){
        word = 0;
        n = (len - i < 4) ? (len - i) : 4; // last one is 2 bytes for len=30
        memcpy(&word, data+i, n);//cp 4 u8s into the word - use n to only cp 2 at end
        WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
        IO_WRITE((u32)curr_write_add, word);//write word
        curr_write_add++;
    }
}

//collect all the data then send it to the send func
void usb_send_state(struct Controller *c, struct MarioState *mstate) {
    u8 playerPacket[SM64_USB_FIXED_PLAYER_BLOCK_SIZE];
    u8 objectPacket[SYNC_OBJECT_PACKET_SIZE];
    u8 frame[SM64_USB_IO_BUFFER_SIZE];
    u16 btn = (u16)c->buttonDown;

    sm64usb_zero_bytes(playerPacket, SM64_USB_FIXED_PLAYER_BLOCK_SIZE);
    sm64usb_zero_bytes(objectPacket, SYNC_OBJECT_PACKET_SIZE);
    sm64usb_zero_bytes(frame, SM64_USB_IO_BUFFER_SIZE);

    playerPacket[SM64_USB_FP_O_PLAYER_ID] = 0xFF;
    sm64usb_write_be32(&playerPacket[SM64_USB_FP_O_X], float_to_u32(mstate->pos[0]));
    sm64usb_write_be32(&playerPacket[SM64_USB_FP_O_Y], float_to_u32(mstate->pos[1]));
    sm64usb_write_be32(&playerPacket[SM64_USB_FP_O_Z], float_to_u32(mstate->pos[2]));
    sm64usb_write_be16(&playerPacket[SM64_USB_FP_O_PITCH], (u16)mstate->faceAngle[0]);
    sm64usb_write_be16(&playerPacket[SM64_USB_FP_O_YAW], (u16)mstate->faceAngle[1]);
    sm64usb_write_be16(&playerPacket[SM64_USB_FP_O_ROLL], (u16)mstate->faceAngle[2]);
    sm64usb_write_be16(&playerPacket[SM64_USB_FP_O_CAM_YAW], (u16)mstate->area->camera->yaw);
    sm64usb_write_be16(&playerPacket[SM64_USB_FP_O_BUTTONS], btn);
    playerPacket[SM64_USB_FP_O_STICK_X] = (u8)c->rawStickX;
    playerPacket[SM64_USB_FP_O_STICK_Y] = (u8)c->rawStickY;
    playerPacket[SM64_USB_FP_O_LEVEL] = (u8)gCurrLevelNum;

    (void)sync_object_pop_outgoing_packet(objectPacket, SYNC_OBJECT_PACKET_SIZE);

    memcpy(&frame[SM64_USB_TRANSPORT_HEADER_SIZE], playerPacket, SM64_USB_FIXED_PLAYER_BLOCK_SIZE);
    memcpy(&frame[SM64_USB_TRANSPORT_HEADER_SIZE + SM64_USB_FIXED_PLAYER_BLOCK_SIZE], objectPacket,
           SYNC_OBJECT_PACKET_SIZE);

    frame[SM64_USB_T_O_SYNC0] = SM64_USB_TRANSPORT_SYNC0;
    frame[SM64_USB_T_O_SYNC1] = SM64_USB_TRANSPORT_SYNC1;
    frame[SM64_USB_T_O_VERSION] = SM64_USB_TRANSPORT_VERSION;
    frame[SM64_USB_T_O_FLAGS] = 0;
    sm64usb_write_be16(&frame[SM64_USB_T_O_LEN], SM64_USB_FRAME_PAYLOAD_SIZE);

    send_32_bit(SM64_USB_FRAME_SIZE, frame);
}

void read_incoming(u8 *data, int len){
    u32 *curr_read_add = (u32 *)CART_SRAM_START;
    int i = 0;
    u32 word = 0;
    int n = 0;


    for (i = 0; i < len; i += 4) {
        n = (len - i < 4) ? (len - i) : 4;
        WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
        word = IO_READ((u32)curr_read_add);//read whole word
        curr_read_add++;
        memcpy(data + i, &word, n);//copy into data arr
    }
}

void usb_update(void) {
    u8 incomingState[SM64_USB_IO_BUFFER_SIZE];
    const u8 *payload = NULL;

    sm64usb_zero_bytes(incomingState, SM64_USB_IO_BUFFER_SIZE);

    if (gMarioObject != NULL) {
        __osPiGetAccess();
       // prevInt = __osDisableInt();//disable interrupts

        // write mario pos
        usb_send_state(&gControllers[0], &gMarioStates[0]);

        //read incoming data
        read_incoming(incomingState, SM64_USB_IO_BUFFER_SIZE);
        __osPiRelAccess();

        if (incomingState[SM64_USB_T_O_SYNC0] == SM64_USB_TRANSPORT_SYNC0
            && incomingState[SM64_USB_T_O_SYNC1] == SM64_USB_TRANSPORT_SYNC1
            && incomingState[SM64_USB_T_O_VERSION] == SM64_USB_TRANSPORT_VERSION) {
            u16 payloadLen = sm64usb_read_be16(&incomingState[SM64_USB_T_O_LEN]);
            if (payloadLen >= SM64_USB_FRAME_PAYLOAD_SIZE) {
                payload = &incomingState[SM64_USB_TRANSPORT_HEADER_SIZE];
            }
        } else {
            payload = NULL;
        }

        if (payload != NULL) {
            usb_comm_consume_fixed_player_block(payload, SM64_USB_FIXED_PLAYER_BLOCK_SIZE);
            sync_object_consume_packet(payload + SM64_USB_FIXED_PLAYER_BLOCK_SIZE, SYNC_OBJECT_PACKET_SIZE);
        } else {
            usb_comm_consume_bytes(incomingState, SM64_USB_PLAYER_PACKET_SIZE);
        }
        usb_comm_apply_remote_inputs();
#if SM64_USB_DEBUG_PRINT
        if (payload != NULL) {
            usb_print_incoming(payload);
        } else {
            usb_print_incoming(incomingState);
        }
#endif
        //__osRestoreInt(prevInt);//END DISABLE INTERRUPTS
    }   
}
