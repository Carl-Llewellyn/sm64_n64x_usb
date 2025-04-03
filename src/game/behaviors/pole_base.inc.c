// pole_base.inc.c
#include "../level_update.h"
#include <usb.h>

int formatMarioPos(float pos, char *buffer, char cPos) {
    float value = pos;
    int len = 0;

    int int_part = (int) value;
    int frac_part = (int) ((value - int_part) * 100);

    if (frac_part < 0) {
        frac_part = -frac_part;
    }

    sprintf(buffer, "%c%d.%02d", cPos, int_part, frac_part);

    while (buffer[len] != '\0') {
        len++;
    }
    return len;
}

void sendMarioPosUSB() {
    char buffer[16];
    int len = 0;

    len = formatMarioPos(gMarioStates[0].pos[0], buffer, 'x');
    usb_write(DATATYPE_TEXT, buffer, len + 1);

    len = formatMarioPos(gMarioStates[0].pos[1], buffer, 'y');
    usb_write(DATATYPE_TEXT, buffer, len + 1);

    len = formatMarioPos(gMarioStates[0].pos[2], buffer, 'z');
    usb_write(DATATYPE_TEXT, buffer, len + 1);
}

void cc(void *dest, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *) dest;
    const unsigned char *s = (const unsigned char *) src;
    while (n--) {
        *d++ = *s++;
    }
}

#define BUFFSIZE 16
int timer = 0;
void sendMarioPosUSBConcat() {
    char xBuff[BUFFSIZE] = { 0 };
    char yBuff[BUFFSIZE] = { 0 };
    char zBuff[BUFFSIZE] = { 0 };
    char out[BUFFSIZE * 3] = { 0 };

    if (timer == 2) {
        if (usb_poll() != 0) {
            return;
        }

        timer = 0;

        /* formatMarioPos should format the number into a fixed-width string,
           or you should pad the result manually. For example, if the formatted
           result is shorter than BUFFSIZE, fill the rest with spaces or zeros.
           For now, we assume formatMarioPos() writes the string into xBuff, etc. */
        formatMarioPos(gMarioStates[0].pos[0], xBuff, 'x');
        formatMarioPos(gMarioStates[0].pos[2], yBuff, 'y');
        formatMarioPos(gMarioStates[0].pos[1], zBuff, 'z');

        /* Instead of concatenating variable lengths, copy exactly BUFFSIZE bytes
           for each coordinate into the output buffer */
        cc(out, xBuff, BUFFSIZE);
        cc(out + BUFFSIZE, yBuff, BUFFSIZE);
        cc(out + 2 * BUFFSIZE, zBuff, BUFFSIZE);
        /* Send the entire fixed-length 48-byte packet */
        usb_write(DATATYPE_TEXT, out, sizeof(out));
    }
    timer++;
}

#define PC64_REGISTER_UART_TX 0x83000004

void bhv_pole_base_loop(void) {

  //  u32 data = 0xDEADBEEF;
 //   usb_dma_write(&data, 0x00000004, sizeof(data));
   // usb_dma_write(&data, 0x83000004, sizeof(data));
   //     usb_dma_write(&data, 0x123000004, sizeof(data));


    //   osPiRawWriteIo(PC64_REGISTER_UART_TX,  0x00000009);
    // osPiRawStartDma(OS_WRITE, PC64_REGISTER_UART_TX, &data, sizeof(data));
    // usb_dma_write(&data, PC64_REGISTER_UART_TX, sizeof(data));
    // usb_write(DATATYPE_TEXT, "1 Mario start\n", sizeof("Mario start\n"));
    if (o->oPosY - 10.0f < gMarioObject->oPosY
        && gMarioObject->oPosY < o->oPosY + o->hitboxHeight + 30.0f) {
        if (o->oTimer > 10 && !(gMarioStates[0].action & MARIO_PUNCHING)) {
            cur_obj_push_mario_away(70.0f);
        }
    }
}
