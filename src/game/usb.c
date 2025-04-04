/***************************************************************
                            usb.c

Allows USB communication between an N64 flashcart and the PC
using UNFLoader.
https://github.com/buu342/N64-UNFLoader
***************************************************************/

#include <string.h>
#include <usb.h>
#include <ultra64.h>



#define ALIGN(VAL_, ALIGNMENT_) (((VAL_) + ((ALIGNMENT_) - 1)) & ~((ALIGNMENT_) - 1))


/*********************************
           Data macros
*********************************/

// Input/Output buffer size. Always keep it at 512
#define BUFFER_SIZE 512

// USB Memory location
#define DEBUG_ADDRESS                                                                                  \
    (0x04000000                                                                                        \
     - DEBUG_ADDRESS_SIZE) // Put the debug area at the 64MB - DEBUG_ADDRESS_SIZE area in ROM space

// Data header related
#define USBHEADER_CREATE(type, left) ((((type) << 24) | ((left) & 0x00FFFFFF)))

// Protocol related
#define USBPROTOCOL_VERSION 2
#define HEARTBEAT_VERSION 1

/*********************************
            SC64 macros
*********************************/

#define SC64_WRITE_TIMEOUT 1000

#define SC64_BASE 0x10000000
#define SC64_REGS_BASE 0x1FFF0000

#define SC64_REG_SR_CMD (SC64_REGS_BASE + 0x00)
#define SC64_REG_DATA_0 (SC64_REGS_BASE + 0x04)
#define SC64_REG_DATA_1 (SC64_REGS_BASE + 0x08)
#define SC64_REG_IDENTIFIER (SC64_REGS_BASE + 0x0C)
#define SC64_REG_KEY (SC64_REGS_BASE + 0x10)

#define SC64_SR_CMD_ERROR (1 << 30)
#define SC64_SR_CMD_BUSY (1 << 31)

#define SC64_V2_IDENTIFIER 0x53437632

#define SC64_KEY_RESET 0x00000000
#define SC64_KEY_UNLOCK_1 0x5F554E4C
#define SC64_KEY_UNLOCK_2 0x4F434B5F

#define SC64_CMD_CONFIG_SET 'C'
#define SC64_CMD_USB_WRITE_STATUS 'U'
#define SC64_CMD_USB_WRITE 'M'
#define SC64_CMD_USB_READ_STATUS 'u'
#define SC64_CMD_USB_READ 'm'

#define SC64_CFG_ROM_WRITE_ENABLE 1

#define SC64_USB_WRITE_STATUS_BUSY (1 << 31)
#define SC64_USB_READ_STATUS_BUSY (1 << 31)

/*********************************
       Function Prototypes
*********************************/

static void usb_findcart(void);
static u32 usb_getaddr();
static void usb_sc64_write(int datatype, const void *data, int size);
static u32 usb_sc64_poll(void);
static void usb_sc64_read(void);

/*********************************
             Globals
*********************************/

// Function pointers
void (*funcPointer_write)(int datatype, const void *data, int size);
u32 (*funcPointer_poll)(void);
void (*funcPointer_read)(void);

// USB globals
static s8 usb_cart = CART_NONE;
static u8 usb_buffer_align[BUFFER_SIZE + 16]; // IDO doesn't support GCC's __attribute__((aligned(x))),
                                              // so this is a workaround
static u8 *usb_buffer;
static char usb_didtimeout = FALSE;
static int usb_datatype = 0;
static int usb_datasize = 0;
static int usb_dataleft = 0;
static int usb_readblock = -1;

// Cart specific globals
static u8 d64_extendedaddr = FALSE;

#ifndef LIBDRAGON
// Message globals


// osPiRaw
extern s32 osPiRawWriteIo(u32, u32);
extern s32 osPiRawReadIo(u32, u32 *);
extern s32 osPiRawStartDma(s32, u32, void *, u32);

#define osPiRawWriteIo(a, b) osPiRawWriteIo(a, b)
#define osPiRawReadIo(a, b) osPiRawReadIo(a, b)
#define osPiRawStartDma(a, b, c, d) osPiRawStartDma(a, b, c, d)


/*********************************
      I/O Wrapper Functions
*********************************/

/*==============================
    usb_io_read
    Reads a 32-bit value from a
    given address using the PI.
    @param  The address to read from
    @return The 4 byte value that was read
==============================*/

static u32 usb_io_read(u32 pi_address) {
#ifndef LIBDRAGON
    u32 value;
    osPiRawReadIo(pi_address, &value);
#endif
    return value;
#else
    return io_read(pi_address);
#endif
}

/*==============================
    usb_io_write
    Writes a 32-bit value to a
    given address using the PI.
    @param  The address to write to
    @param  The 4 byte value to write
==============================*/

static void usb_io_write(u32 pi_address, u32 value) {
#ifndef LIBDRAGON
    osPiRawWriteIo(pi_address, value);
#else
    io_write(pi_address, value);
#endif
}

/*==============================
    usb_dma_read
    Reads arbitrarily sized data from a
    given address using DMA.
    @param  The buffer to read into
    @param  The address to read from
    @param  The size of the data to read
==============================*/

static void usb_dma_read(void *ram_address, u32 pi_address, size_t size) {
#ifndef LIBDRAGON
    osWritebackDCache(ram_address, size);
    osInvalDCache(ram_address, size);

    osPiRawStartDma(OS_READ, pi_address, ram_address, size);
#else
    data_cache_hit_writeback_invalidate(ram_address, size);
    dma_read(ram_address, pi_address, size);
#endif
}

/*==============================
    usb_dma_write
    writes arbitrarily sized data to a
    given address using DMA.
    @param  The buffer to read from
    @param  The address to write to
    @param  The size of the data to write
==============================*/

static void usb_dma_write(void *ram_address, u32 pi_address, size_t size) {
#ifndef LIBDRAGON
    osWritebackDCache(ram_address, size);
    osPiRawStartDma(OS_WRITE, pi_address, ram_address, size);
#else
    data_cache_hit_writeback(ram_address, size);
    dma_write(ram_address, pi_address, size);
#endif
}

/*********************************
         Timeout helpers
*********************************/

/*==============================
    usb_timeout_start
    Returns current value of COUNT coprocessor 0 register
    @return C0_COUNT value
==============================*/

static u32 usb_timeout_start(void) {
#ifndef LIBDRAGON
    return osGetCount();
#else
    return TICKS_READ();
#endif
}

/*==============================
    usb_timeout_check
    Checks if timeout occurred
    @param Starting value obtained from usb_timeout_start
    @param Timeout duration specified in milliseconds
    @return TRUE if timeout occurred, otherwise FALSE
==============================*/

static char usb_timeout_check(u32 start_ticks, u32 duration) {

    return FALSE;
}

/*********************************
          USB functions
*********************************/

/*==============================
    usb_initialize
    Initializes the USB buffers and pointers
    @returns 1 if the USB initialization was successful, 0 if not
==============================*/

void *my_memset(void *dest, int value, size_t count) {
    unsigned char *ptr = (unsigned char *)dest;
    while (count--) {
        *ptr++ = (unsigned char)value;
    }
    return dest;
}

char usb_initialize(void) {
    // Initialize the debug related globals
    usb_buffer = (u8 *) OS_DCACHE_ROUNDUP_ADDR(usb_buffer_align);
    my_memset(usb_buffer, 0, BUFFER_SIZE);

#ifndef LIBDRAGON

#endif

    // Find the flashcart
    usb_findcart();

    // Set the function pointers based on the flashcart
    switch (usb_cart) {
        case CART_SC64:
            funcPointer_write = usb_sc64_write;
            funcPointer_poll = usb_sc64_poll;
            funcPointer_read = usb_sc64_read;
            break;
        default:
            return 0;
    }

    // Send a heartbeat
    usb_sendheartbeat();
    return 1;
}

/*==============================
    usb_findcart
SC64.
==============================*/

static void usb_findcart(void) {
// Before we do anything, check that we are using an emulator
#if CHECK_EMULATOR
    // Check the RDP clock register.
    // Always zero on emulators
    if (IO_READ(0xA4100010) == 0) // DPC_CLOCK_REG in Libultra
        return;

    // Fallback, harder emulator check.
    // The VI has an interesting quirk where its values are mirrored every 0x40 bytes
    // It's unlikely that emulators handle this, so we'll write to the VI_TEST_ADDR register and
    // readback 0x40 bytes from its address If they don't match, we probably have an emulator
    buff = (*(u32 *) 0xA4400038);
    (*(u32 *) 0xA4400038) = 0x6ABCDEF9;
    if ((*(u32 *) 0xA4400038) != (*(u32 *) 0xA4400078)) {
        (*(u32 *) 0xA4400038) = buff;
        return;
    }
    (*(u32 *) 0xA4400038) = buff;
#endif

    // let's assume we have a SC64
    // Write the key sequence to unlock the registers, then read the identifier register
    usb_io_write(SC64_REG_KEY, SC64_KEY_RESET);
    usb_io_write(SC64_REG_KEY, SC64_KEY_UNLOCK_1);
    usb_io_write(SC64_REG_KEY, SC64_KEY_UNLOCK_2);

    // Check if we have a SC64
    if (usb_io_read(SC64_REG_IDENTIFIER) == SC64_V2_IDENTIFIER) {
        // Set the cart to SC64
        usb_cart = CART_SC64;
        return;
    }
}

/*==============================
    usb_getcart
    Returns which flashcart is currently connected
    @return The CART macro that corresponds to the identified flashcart
==============================*/

char usb_getcart(void) {
    return usb_cart;
}

/*==============================
    usb_getaddr
    Gets the base address for the USB data to be stored in
    @return The base data address
==============================*/

u32 usb_getaddr() {
    if (usb_cart == CART_64DRIVE && d64_extendedaddr)
        return 0x10000000 - DEBUG_ADDRESS_SIZE;
    else
        return DEBUG_ADDRESS;
}

/*==============================
    usb_write
    Writes data to the USB.
    Will not write if there is data to read from USB
    @param The DATATYPE that is being sent
    @param A buffer with the data to send
    @param The size of the data being sent
==============================*/
char usb_write(int datatype, const void *data, int size) {
    // If no debug cart exists, stop
   // if (usb_cart == CART_NONE)
     //   return;

    // If there's data to read first, stop
    if (usb_dataleft != 0)
        return;

    usb_sc64_write(datatype, data, size);
}

/*==============================
    usb_poll
    Returns the header of data being received via USB
    The first byte contains the data type, the next 3 the number of bytes left to read
    @return The data header, or 0
==============================*/

u32 usb_poll(void) {
    // If no debug cart exists, stop
    if (usb_cart == CART_NONE)
        return 0;

    // If we're out of USB data to read, we don't need the header info anymore
    if (usb_dataleft <= 0) {
        usb_dataleft = 0;
        usb_datatype = 0;
        usb_datasize = 0;
        usb_readblock = -1;
    }

    // If there's still data that needs to be read, return the header with the data left
    if (usb_dataleft != 0)
        return USBHEADER_CREATE(usb_datatype, usb_dataleft);

    // Call the correct read function
    return funcPointer_poll();
}

/*==============================
    usb_read
    Reads bytes from USB into the provided buffer
    @param The buffer to put the read data in
    @param The number of bytes to read
==============================*/

void usb_read(void *buffer, int nbytes) {
    int read = 0;
    int left = nbytes;
    int offset = usb_datasize - usb_dataleft;
    int copystart = offset % BUFFER_SIZE;
    int block = BUFFER_SIZE - copystart;
    int blockoffset = (offset / BUFFER_SIZE) * BUFFER_SIZE;

    // If no debug cart exists, stop
    if (usb_cart == CART_NONE)
        return;

    // If there's no data to read, stop
    if (usb_dataleft == 0)
        return;

    // Read chunks from ROM
    while (left > 0) {
        // Ensure we don't read too much data
        if (left > usb_dataleft)
            left = usb_dataleft;
        if (block > left)
            block = left;

        // Call the read function if we're reading a new block
        if (usb_readblock != blockoffset) {
            usb_readblock = blockoffset;
            funcPointer_read();
        }

        // Copy from the USB buffer to the supplied buffer
        memcpy((void *) (((u32) buffer) + read), usb_buffer + copystart, block);

        // Increment/decrement all our counters
        read += block;
        left -= block;
        usb_dataleft -= block;
        blockoffset += BUFFER_SIZE;
        block = BUFFER_SIZE;
        copystart = 0;
    }
}

/*==============================
    usb_skip
    Skips a USB read by the specified amount of bytes
    @param The number of bytes to skip
==============================*/

void usb_skip(int nbytes) {
    // Subtract the amount of bytes to skip to the data pointers
    usb_dataleft -= nbytes;
    if (usb_dataleft < 0)
        usb_dataleft = 0;
}

/*==============================
    usb_rewind
    Rewinds a USB read by the specified amount of bytes
    @param The number of bytes to rewind
==============================*/

void usb_rewind(int nbytes) {
    // Add the amount of bytes to rewind to the data pointers
    usb_dataleft += nbytes;
    if (usb_dataleft > usb_datasize)
        usb_dataleft = usb_datasize;
}

/*==============================
    usb_purge
    Purges the incoming USB data
==============================*/

void usb_purge(void) {
    usb_dataleft = 0;
    usb_datatype = 0;
    usb_datasize = 0;
    usb_readblock = -1;
}

/*==============================
    usb_timedout
    Checks if the USB timed out recently
    @return 1 if the USB timed out, 0 if not
==============================*/

char usb_timedout() {
    return usb_didtimeout;
}

/*==============================
    usb_sendheartbeat
    Sends a heartbeat packet to the PC
    This is done once automatically at initialization,
    but can be called manually to ensure that the
    host side tool is aware of the current USB protocol
    version.
==============================*/

void usb_sendheartbeat(void) {
    u8 buffer[4];

    // First two bytes describe the USB library protocol version
    buffer[0] = (u8) (((USBPROTOCOL_VERSION) >> 8) & 0xFF);
    buffer[1] = (u8) (((USBPROTOCOL_VERSION)) & 0xFF);

    // Next two bytes describe the heartbeat packet version
    buffer[2] = (u8) (((HEARTBEAT_VERSION) >> 8) & 0xFF);
    buffer[3] = (u8) (((HEARTBEAT_VERSION)) & 0xFF);

    // Send through USB
    usb_write(DATATYPE_HEARTBEAT, buffer, sizeof(buffer) / sizeof(buffer[0]));
}


#ifndef LIBDRAGON
static char usb_sc64_execute_cmd(u8 cmd, u32 *args, u32 *result)
#else
char usb_sc64_execute_cmd(u8 cmd, u32 *args, u32 *result)
#endif
{
    u32 sr;

    // Write arguments if provided
    if (args != NULL) {
        usb_io_write(SC64_REG_DATA_0, args[0]);
        usb_io_write(SC64_REG_DATA_1, args[1]);
    }

    // Start execution
    usb_io_write(SC64_REG_SR_CMD, cmd);

    // Wait for completion
    do {
        sr = usb_io_read(SC64_REG_SR_CMD);
    } while (sr & SC64_SR_CMD_BUSY);

    // Read result if provided
    if (result != NULL) {
        result[0] = usb_io_read(SC64_REG_DATA_0);
        result[1] = usb_io_read(SC64_REG_DATA_1);
    }

    // Return error status
    if (sr & SC64_SR_CMD_ERROR)
        return TRUE;
    return FALSE;
}

/*==============================
    usb_sc64_set_writable
    Enable ROM (SDRAM) writes in SC64
    @param  A boolean with whether to enable or disable
    @return Previous value of setting
==============================*/

static u32 usb_sc64_set_writable(u32 enable) {
    u32 args[2];
    u32 result[2];

    args[0] = SC64_CFG_ROM_WRITE_ENABLE;
    args[1] = enable;
    if (usb_sc64_execute_cmd(SC64_CMD_CONFIG_SET, args, result))
        return 0;

    return result[1];
}

/*==============================
    usb_sc64_write
    Sends data through USB from the SC64
    @param The DATATYPE that is being sent
    @param A buffer with the data to send
    @param The size of the data being sent
==============================*/

static void usb_sc64_write(int datatype, const void *data, int size) {
    u32 left = size;
    u32 pi_address = SC64_BASE + usb_getaddr();
    u32 writable_restore;
    u32 timeout;
    u32 args[2];
    u32 result[2];

    // Return if previous transfer timed out
    usb_sc64_execute_cmd(SC64_CMD_USB_WRITE_STATUS, NULL, result);
    if (result[0] & SC64_USB_WRITE_STATUS_BUSY) {
        usb_didtimeout = TRUE;
        return;
    }

    // Enable SDRAM writes and get previous setting
    writable_restore = usb_sc64_set_writable(TRUE);

    while (left > 0) {
        // Calculate transfer size
        u32 block = MIN(left, BUFFER_SIZE);

        // Copy data to PI DMA aligned buffer
        memcpy(usb_buffer, data, block);

        // Copy block of data from RDRAM to SDRAM
        usb_dma_write(usb_buffer, pi_address, ALIGN(block, 2));

        // Update pointers and variables
        data = (void *) (((u32) data) + block);
        left -= block;
        pi_address += block;
    }

    // Restore previous SDRAM writable setting
    usb_sc64_set_writable(writable_restore);

    // Start sending data from buffer in SDRAM
    args[0] = SC64_BASE + usb_getaddr();
    args[1] = USBHEADER_CREATE(datatype, size);
    if (usb_sc64_execute_cmd(SC64_CMD_USB_WRITE, args, NULL)) {
        usb_didtimeout = TRUE;
        return; // Return if USB write was unsuccessful
    }

    // Wait for transfer to end
    timeout = usb_timeout_start();
    do {
        // Took too long, abort
        if (usb_timeout_check(timeout, SC64_WRITE_TIMEOUT)) {
            usb_didtimeout = TRUE;
            return;
        }
        usb_sc64_execute_cmd(SC64_CMD_USB_WRITE_STATUS, NULL, result);
    } while (result[0] & SC64_USB_WRITE_STATUS_BUSY);
    usb_didtimeout = FALSE;
}

/*==============================
    usb_sc64_poll
    Returns the header of data being received via USB on the SC64
    The first byte contains the data type, the next 3 the number of bytes left to read
    @return The data header, or 0
==============================*/

static u32 usb_sc64_poll(void) {
    u8 datatype;
    u32 size;
    u32 args[2];
    u32 result[2];

    // Get read status and extract packet info
    usb_sc64_execute_cmd(SC64_CMD_USB_READ_STATUS, NULL, result);
    datatype = result[0] & 0xFF;
    size = result[1] & 0xFFFFFF;

    // Return 0 if there's no data
    if (size == 0)
        return 0;

    // Fill USB read data variables
    usb_datatype = datatype;
    usb_dataleft = size;
    usb_datasize = usb_dataleft;
    usb_readblock = -1;

    // Start receiving data to buffer in SDRAM
    args[0] = SC64_BASE + usb_getaddr();
    args[1] = size;
    if (usb_sc64_execute_cmd(SC64_CMD_USB_READ, args, NULL))
        return 0; // Return 0 if USB read was unsuccessful

    // Wait for completion
    do {
        usb_sc64_execute_cmd(SC64_CMD_USB_READ_STATUS, NULL, result);
    } while (result[0] & SC64_USB_READ_STATUS_BUSY);

    // Return USB header
    return USBHEADER_CREATE(datatype, size);
}

/*==============================
    usb_sc64_read
    Reads bytes from the SC64 SDRAM into the global buffer with the block offset
==============================*/

static void usb_sc64_read(void) {
    // Set up DMA transfer between RDRAM and the PI
    usb_dma_read(usb_buffer, SC64_BASE + usb_getaddr() + usb_readblock, BUFFER_SIZE);
}
