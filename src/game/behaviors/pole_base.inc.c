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



void bhv_pole_base_loop(void) {
    if (o->oPosY - 10.0f < gMarioObject->oPosY
        && gMarioObject->oPosY < o->oPosY + o->hitboxHeight + 30.0f) {
        if (o->oTimer > 10 && !(gMarioStates[0].action & MARIO_PUNCHING)) {
            cur_obj_push_mario_away(70.0f);
        }
    }
}
