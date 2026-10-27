#ifndef MYOS_USB_HID_MOUSE_H
#define MYOS_USB_HID_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

#define USB_HID_MOUSE_MAX_DEVICES 32
#define USB_HID_MOUSE_MAX_SLOTS   256

struct usb_hid_mouse_endpoint_info {
    uint8_t interface_number;
    uint8_t endpoint_address;
    uint8_t interval;
    uint16_t max_packet_size;
};

struct usb_hid_mouse {
    bool active;
    uint8_t slot_id;
    uint8_t speed;
    uint8_t buttons;

    uint8_t *descriptor_buffer;
    uint64_t descriptor_buffer_phys;

    uint8_t *report_buffer;
    uint64_t report_buffer_phys;
};

bool usb_hid_mouse_init(uint8_t slot_id, uint8_t speed);

bool usb_hid_mouse_read_packet(
    uint8_t slot_id,
    int *dx,
    int *dy,
    uint8_t *buttons
);

bool usb_hid_mouse_decode_boot_report(
    const uint8_t *report,
    uint16_t length,
    int *dx,
    int *dy,
    uint8_t *buttons
);

bool usb_hid_mouse_find_boot_endpoint(
    const uint8_t *descriptors,
    uint16_t length,
    struct usb_hid_mouse_endpoint_info *info
);

#endif