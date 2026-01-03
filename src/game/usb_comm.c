#include "usb_comm.h"

#include <PR/ultratypes.h>
#include <PR/os_cont.h>

#include "game/game_init.h"

extern void adjust_analog_stick(struct Controller *controller);

static u16 sUsbSeq;

static u16 usb_comm_crc16_ccitt(const u8 *data, u16 len) {
    u16 crc = 0xFFFF;
    u16 i;
    u16 bit;

    for (i = 0; i < len; i++) {
        crc ^= (u16)data[i] << 8;
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (u16)((crc << 1) ^ 0x1021);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static void usb_comm_update_controller(struct Controller *controller, u16 buttons, s8 stick_x, s8 stick_y) {
    controller->rawStickX = stick_x;
    controller->rawStickY = stick_y;
    controller->buttonPressed = buttons & (buttons ^ controller->buttonDown);
    controller->buttonDown = buttons;
    adjust_analog_stick(controller);
}

void usb_comm_reset(void) {
    sUsbSeq = 0;
}

void usb_comm_apply_player_input(u8 player_index, u16 buttons, s8 stick_x, s8 stick_y) {
    u8 slot = (u8)(player_index + 1);

    if (slot >= ARRAY_COUNT(gControllers)) {
        return;
    }

    // Test hook: hold L+R+Z on P3 to force a left run.
    if (player_index == 1 && ((buttons & (L_TRIG | R_TRIG | Z_TRIG)) == (L_TRIG | R_TRIG | Z_TRIG))) {
        stick_x = -64;
        stick_y = 0;
    }

    usb_comm_update_controller(&gControllers[slot], buttons, stick_x, stick_y);
}

void usb_comm_test_inject_p3_left(void) {
    u8 packet[USB_COMM_HEADER_SIZE + (2 * USB_COMM_RECORD_SIZE) + USB_COMM_CRC_SIZE];
    u16 crc;
    u16 offset = USB_COMM_HEADER_SIZE;

    packet[0] = USB_COMM_SYNC0;
    packet[1] = USB_COMM_SYNC1;
    packet[2] = USB_COMM_VERSION;
    packet[3] = (u8)(sUsbSeq++ & 0xFF);
    packet[4] = 2; // P2 + P3 records
    packet[5] = 0;

    // P2: neutral
    packet[offset + 0] = 0;
    packet[offset + 1] = 0;
    packet[offset + 2] = 0;
    packet[offset + 3] = 0;
    offset += USB_COMM_RECORD_SIZE;

    // P3: stick left
    packet[offset + 0] = 0;
    packet[offset + 1] = 0;
    packet[offset + 2] = (u8)-64;
    packet[offset + 3] = 0;
    offset += USB_COMM_RECORD_SIZE;

    crc = usb_comm_crc16_ccitt(&packet[2], (u16)(sizeof(packet) - 2 - 2));
    packet[offset + 0] = (u8)(crc >> 8);
    packet[offset + 1] = (u8)(crc & 0xFF);

    usb_comm_parse_and_apply(packet, (u16)sizeof(packet));
}

s32 usb_comm_parse_and_apply(const u8 *data, u16 len) {
    u16 expected_len;
    u16 crc_expected;
    u16 crc_actual;
    u8 player_count;
    u8 seq;
    u16 offset;
    u8 i;

    if (len < USB_COMM_HEADER_SIZE + USB_COMM_CRC_SIZE) {
        return USB_COMM_ERR_SHORT;
    }

    if (data[0] != USB_COMM_SYNC0 || data[1] != USB_COMM_SYNC1) {
        return USB_COMM_ERR_SYNC;
    }

    if (data[2] != USB_COMM_VERSION) {
        return USB_COMM_ERR_VERSION;
    }

    seq = data[3];
    player_count = data[4];

    expected_len = (u16)(USB_COMM_HEADER_SIZE + (player_count * USB_COMM_RECORD_SIZE) + USB_COMM_CRC_SIZE);
    if (len != expected_len) {
        return USB_COMM_ERR_LEN;
    }

    crc_expected = (u16)((data[len - 2] << 8) | data[len - 1]);
    crc_actual = usb_comm_crc16_ccitt(&data[2], (u16)(len - 2 - 2));
    if (crc_actual != crc_expected) {
        return USB_COMM_ERR_CRC;
    }

    sUsbSeq = seq;

    offset = USB_COMM_HEADER_SIZE;
    for (i = 0; i < player_count; i++) {
        u16 buttons = (u16)((data[offset] << 8) | data[offset + 1]);
        s8 stick_x = (s8)data[offset + 2];
        s8 stick_y = (s8)data[offset + 3];

        usb_comm_apply_player_input(i, buttons, stick_x, stick_y);
        offset = (u16)(offset + USB_COMM_RECORD_SIZE);
    }

    return USB_COMM_OK;
}
