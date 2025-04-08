#include <string.h>
#include <usb.h>
#include <ultra64.h>
#include <PR/rcp.h>
#include <PR/os.h>
#include <stdio.h>
#include "object_list_processor.h"
#include "sm64.h"
#include "print.h"

f32 read_usb_posX = -1629.98;
f32 read_usb_posY = 261.04;
f32 read_usb_posZ = 3479.55;

f32 temp_read_usb_posX = -1629.98;
f32 temp_read_usb_posY = 261.04;
f32 temp_read_usb_posZ = 3479.55;

ALIGNED8 u8 gThread7Stack[STACKSIZE];

static char zPosOut[60];

s32 incomingUsbInterrupt = 0;

f32 __osAtomicReadF32(f32 *src) {
    s32 prevInt = __osDisableInt();
    f32 value = *src;
    __osRestoreInt(prevInt);
    return value;
}

f32 __osAtomicWriteF32(f32 *src, f32 *dest) {
    s32 prevInt = __osDisableInt();
    *src = *dest;
    __osRestoreInt(prevInt);
}

void string_copy(const char *src, int len, char *dest) {
    int i = 0;
    for (i = 0; i < len; i++) {
        dest[i] = src[i];
    }
}

void incoming_usb_pos(f32 *x, f32 *y, f32 *z) {
    incomingUsbInterrupt = __osDisableInt();
    *x = read_usb_posX;
    *y = read_usb_posY;
    *z = read_usb_posZ;
    __osRestoreInt(incomingUsbInterrupt);
}

void thread7_usb_loop(UNUSED void *arg) {
    u32 posX_f32_binary_cast = 0;
    u32 posY_f32_binary_cast = 0;
    u32 posZ_f32_binary_cast = 0;

    OSTimer timer;
    OSMesgQueue timerQueue;
    OSMesg timerMsg;
    OSMesgQueue *mq = &timerQueue;

    char xPosOut[20];
    char yPosOut[20];

    s32 prevInt = 0;

    osCreateMesgQueue(&timerQueue, &timerMsg, 1);

    while (TRUE) {
        if (gMarioObject != NULL) {
            // this is casting the f32 binary values into the int by telling the compiler it's actually
            // a float this means we can pass the f32 as a u32 and convert it back at the other end
            *(f32 *) &posX_f32_binary_cast = __osAtomicReadF32(&gMarioObject->oPosX);
            *(f32 *) &posY_f32_binary_cast = __osAtomicReadF32(&gMarioObject->oPosY);
            *(f32 *) &posZ_f32_binary_cast = __osAtomicReadF32(&gMarioObject->oPosZ);

            __osPiGetAccess();
            // write mario pos
            WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
            IO_WRITE(USB_X_ADDR, posX_f32_binary_cast); // the macro takes care of the offsets

            WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
            IO_WRITE(USB_Y_ADDR, posY_f32_binary_cast);

            WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
            IO_WRITE(USB_Z_ADDR, posZ_f32_binary_cast);

            // read other (crash bandicoot) pos

             prevInt = __osDisableInt();//START DISABLE INTERRUPTS

             WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
             posX_f32_binary_cast = IO_READ(READ_USB_X_ADDR);
             read_usb_posX = *(f32 *) &posX_f32_binary_cast;

             //check the pos to make sure it's in bounds and doesn't crash the game
             if (read_usb_posX > -8000 && read_usb_posX < 8000){
                temp_read_usb_posX = read_usb_posX;
             }else{
                read_usb_posX = temp_read_usb_posX;
             }

             WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
             posY_f32_binary_cast = IO_READ(READ_USB_Y_ADDR);
             read_usb_posY = *(f32 *) &posY_f32_binary_cast;

             if (read_usb_posY > -8000 && read_usb_posY < 8000){
                temp_read_usb_posY = read_usb_posY;
             }else{
                read_usb_posY = temp_read_usb_posY;
             }

             WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
             posZ_f32_binary_cast = IO_READ(READ_USB_Z_ADDR);
             read_usb_posZ = *(f32 *) &posZ_f32_binary_cast;

             if (read_usb_posZ > -8000 && read_usb_posZ < 8000){
                temp_read_usb_posZ = read_usb_posZ;
             }else{
                read_usb_posZ = temp_read_usb_posZ;
             }

            __osPiRelAccess();
 
            __osRestoreInt(prevInt);//END DISABLE INTERRUPTS

            osSetTimer(&timer, OS_USEC_TO_CYCLES(5000), 0, mq, NULL);
            osRecvMesg(mq, &timerMsg, OS_MESG_BLOCK);
        }
    }
}
