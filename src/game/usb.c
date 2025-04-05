#include <string.h>
#include <usb.h>
#include <ultra64.h>
#include <PR/rcp.h>
#include <PR/os.h>
#include <stdio.h>
#include "object_list_processor.h"
#include "sm64.h"

f32 read_usb_posX = 0;
f32 read_usb_posY = 0;
f32 read_usb_posZ = 0;

ALIGNED8 u8 gThread7Stack[STACKSIZE];

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

void thread7_usb_loop(UNUSED void *arg) {
    u32 posX_f32_binary_cast = 0;
    u32 posY_f32_binary_cast = 0;
    u32 posZ_f32_binary_cast = 0;

    OSTimer timer;
    OSMesgQueue timerQueue;
    OSMesg timerMsg;
    OSMesgQueue *mq = &timerQueue;

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
            IO_WRITE(USB_Y_ADDR, posY_f32_binary_cast);

            WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
            IO_WRITE(USB_X_ADDR, posX_f32_binary_cast); // the macro takes care of the offsets

            WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
            IO_WRITE(USB_Z_ADDR, posZ_f32_binary_cast);

            // read other (crash bandicoot) pos
            WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
            osPiRawReadIo(READ_USB_X_ADDR, &posX_f32_binary_cast);
            __osAtomicWriteF32(&read_usb_posX, &(*(f32 *)&posX_f32_binary_cast));

            WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
            osPiRawReadIo(READ_USB_Y_ADDR, &posY_f32_binary_cast);
            __osAtomicWriteF32(&read_usb_posY, &(*(f32 *)&posY_f32_binary_cast));

            WAIT_ON_IO_BUSY(IO_READ(PI_STATUS_REG));
            osPiRawReadIo(READ_USB_Z_ADDR, &posZ_f32_binary_cast);
            __osAtomicWriteF32(&read_usb_posZ, &(*(f32 *)&posZ_f32_binary_cast));

            __osPiRelAccess();

            osSetTimer(&timer, OS_USEC_TO_CYCLES(5000), 0, mq, NULL);
            osRecvMesg(mq, &timerMsg, OS_MESG_BLOCK);
        }
    }
}
