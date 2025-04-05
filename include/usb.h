#ifndef UNFL_USB_H
#define UNFL_USB_H

#define CART_DOM2_ADDR2_START 0x08000000
#define CART_SRAM_START CART_DOM2_ADDR2_START

#define USB_X_ADDR CART_SRAM_START
#define USB_Y_ADDR (USB_X_ADDR + 4)
#define USB_Z_ADDR (USB_Y_ADDR + 4)

#define READ_USB_X_ADDR (USB_Z_ADDR + 4)
#define READ_USB_Y_ADDR (READ_USB_X_ADDR + 4)
#define READ_USB_Z_ADDR (READ_USB_Y_ADDR + 4)

#define WAIT_ON_IO_BUSY(stat)                                                                          \
    stat = IO_READ(PI_STATUS_REG);                                                                     \
    while (stat & (PI_STATUS_IO_BUSY | PI_STATUS_DMA_BUSY))                                            \
        stat = IO_READ(PI_STATUS_REG);

// Align to 8-byte boundary for DMA requirements
#ifdef __GNUC__
#define ALIGNED8 __attribute__((aligned(8)))
#else
#define ALIGNED8
#endif

#ifdef __GNUC__
#define UNUSED __attribute__((unused))
#else
#define UNUSED
#endif

#define STACKSIZE 0x2000


extern f32 read_usb_posX;
extern f32 read_usb_posY;
extern f32 read_usb_posZ;

extern ALIGNED8 u8 gThread7Stack[STACKSIZE];

extern f32 __osAtomicReadF32(f32 *src);
extern f32 __osAtomicWriteF32(f32 *src, f32 *dest);

extern void thread7_usb_loop(UNUSED void *arg);


#endif
