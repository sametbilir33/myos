
#ifndef MYOS_USB_HID_MOUSE_H
#define MYOS_USB_HID_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

bool usb_hid_mouse_decode_boot_report(
    const uint8_t *report,
    uint16_t length,
    int *dx,
    int *dy,
    uint8_t *buttons
);

bool usb_hid_mouse_read_packet(
    int *dx,
    int *dy,
    uint8_t *buttons
);

#endif
