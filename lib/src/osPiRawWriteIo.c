#include "libultra_internal.h"
#include "PR/rcp.h"
#include "piint.h"
s32 osPiRawWriteIo(u32 devAddr, u32 data){
    register u32 stat;
    WAIT_ON_IO_BUSY(stat);
    IO_WRITE((u32)osRomBase | devAddr, data);
    return 0;
}
