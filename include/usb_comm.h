#ifndef USB_COMM_H
#define USB_COMM_H

#include <PR/ultratypes.h>

#include "sm64_usb_protocol.h"

void usb_comm_init(void);
void usb_comm_reset(void);
void usb_comm_consume_bytes(const u8 *data, u32 len);
void usb_comm_apply_remote_inputs(void);

#endif
