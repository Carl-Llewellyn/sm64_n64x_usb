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
#include "usb_comm.h"

static char gBuf[32];

#define SM64_USB_DEBUG_PRINT 1

static void usb_print_incoming(const u8 *data) {
    int i = 0;
    int y = 12;
    u16 buttons = sm64usb_read_be16(&data[SM64_USB_O_BUTTONS]);
    s8 stick_x = (s8)data[SM64_USB_O_STICK_X];
    s8 stick_y = (s8)data[SM64_USB_O_STICK_Y];

    sprintf(gBuf, "rx %02X %02X %02X %02X",
            data[0], data[1], data[2], data[3]);
    print_text(0, y, gBuf);
    y += 20;

    for (i = 4; i < 20; i += 4) {
        sprintf(gBuf, "%02d: %02X %02X %02X %02X",
                i, data[i + 0], data[i + 1], data[i + 2], data[i + 3]);
        print_text(0, y, gBuf);
        y += 20;
    }

    sprintf(gBuf, "20: %02X %02X",
            data[20], data[21]);
    print_text(0, y, gBuf);
    y += 20;

    sprintf(gBuf, "pid=%d btn=%04X sx=%d sy=%d",
            (int)data[SM64_USB_O_PLAYER_ID],
            (unsigned int)buttons,
            (int)stick_x, (int)stick_y);
    print_text(0, y, gBuf);
}

//split whatever comes in to write 32bit
void send_32_bit(int len, u8 *data){
    u32 *curr_write_add = (u32 *)CART_SRAM_START;
    int i = 0;
    u32 word = 0;
    int n = 0;

    if(len != SM64_USB_PACKET_SIZE){
        return;
    }

    //32/8 = 4 - so we can send 4 u8s at a time
    for(i = 0; i < len; i+=4){
        word = 0;
        n = (len - i < 4) ? (len - i) : 4; // last one is 2 bytes for len=22
        memcpy(&word, data+i, n);//cp 4 u8s into the word - use n to only cp 2 at end
        WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
        IO_WRITE((u32)curr_write_add, word);//write word
        curr_write_add++;
    }
}

//collect all the data then send it to the send func
void usb_send_state(struct Controller *c, struct MarioState *mstate) {
    u8 raw[SM64_USB_PACKET_SIZE] = {0};
    u16 btn = (u16)c->buttonDown;

    raw[0] = SM64_USB_SYNC0;
    raw[1] = SM64_USB_SYNC1;
    raw[2] = SM64_USB_VERSION;
    raw[3] = 0xFF;

    memcpy(raw+4, &mstate->pos[0], 4);//not being used atm but meant for sync
    memcpy(raw+8, &mstate->pos[1], 4);
    memcpy(raw+12, &mstate->pos[2], 4);

    memcpy(raw + 16, &btn, 2);//2 bytes
    raw[18] = (u8)c->rawStickX;
    raw[19] = (u8)c->rawStickY;

    send_32_bit(SM64_USB_PACKET_SIZE, raw);
}

void read_incoming(u8 *data){
    const int len = 22;
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
    u8 incomingState[SM64_USB_PACKET_SIZE] = {0};

    if (gMarioObject != NULL) {
        __osPiGetAccess();
       // prevInt = __osDisableInt();//disable interrupts

        // write mario pos
        usb_send_state(&gControllers[0], &gMarioStates[0]);

        //read incoming data
        read_incoming(incomingState);
        __osPiRelAccess();
        usb_comm_consume_bytes(incomingState, SM64_USB_PACKET_SIZE);
        usb_comm_apply_remote_inputs();
#if SM64_USB_DEBUG_PRINT
        usb_print_incoming(incomingState);
#endif
        //__osRestoreInt(prevInt);//END DISABLE INTERRUPTS
    }   
}
