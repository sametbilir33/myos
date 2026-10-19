
#include <drivers/usb_hid_mouse.h>
#include <drivers/xhci.h>

#include <stddef.h>
#include <stdint.h>

bool usb_hid_mouse_decode_boot_report(
    const uint8_t *report,
    uint16_t length,
    int *dx,
    int *dy,
    uint8_t *buttons
)
{
    if (report == NULL ||
        dx == NULL ||
        dy == NULL ||
        buttons == NULL) {
        return false;
    }

    /*
     * A standard boot mouse report contains:
     *   Byte 0: Button bits
     *   Byte 1: Relative X movement
     *   Byte 2: Relative Y movement
     *
     * Additional bytes, such as a wheel field, are
     * intentionally ignored by this decoder.
     */
    if (length < 3) {
        return false;
    }

    *buttons = report[0] & 0x07u;
    *dx = (int)(int8_t)report[1];
    *dy = (int)(int8_t)report[2];

    return true;
}


bool usb_hid_mouse_read_packet(
    int *dx,
    int *dy,
    uint8_t *buttons
)
{
    uint8_t report[255];
    uint16_t length = 0;

    if (dx == NULL ||
        dy == NULL ||
        buttons == NULL) {
        return false;
    }

    if (!xhci_mouse_read_report(
            report,
            sizeof(report),
            &length)) {
        return false;
    }

    return usb_hid_mouse_decode_boot_report(
        report,
        length,
        dx,
        dy,
        buttons
    );
}


bool usb_hid_mouse_find_boot_endpoint(
    const uint8_t *descriptors,
    uint16_t length,
    struct usb_hid_mouse_endpoint_info *info
)
{
    if (descriptors == NULL || info == NULL || length < 9)
        return false;

    bool matching_interface = false;

    for (uint32_t offset = 0; offset + 2 <= length;) {
        uint8_t descriptor_length = descriptors[offset];
        uint8_t descriptor_type = descriptors[offset + 1];

        if (descriptor_length < 2 ||
            offset + descriptor_length > length) {
            return false;
        }

        if (descriptor_type == 4 && descriptor_length >= 9) {
            /*
             * Interface descriptor:
             * Class = HID, Subclass = Boot, Protocol = Mouse.
             */
            matching_interface =
                descriptors[offset + 5] == 3 &&
                descriptors[offset + 6] == 1 &&
                descriptors[offset + 7] == 2;

            if (matching_interface) {
                info->interface_number = descriptors[offset + 2];
            }
        } else if (descriptor_type == 5 &&
                   descriptor_length >= 7 &&
                   matching_interface) {
            uint8_t address = descriptors[offset + 2];
            uint8_t attributes = descriptors[offset + 3];

            uint16_t packet_size =
                (uint16_t)descriptors[offset + 4] |
                ((uint16_t)descriptors[offset + 5] << 8);

            packet_size &= 0x07FFu;

            bool is_in = (address & 0x80u) != 0;
            bool is_interrupt = (attributes & 0x03u) == 3u;

            if (is_in &&
                is_interrupt &&
                packet_size >= 3 &&
                packet_size <= 255) {
                info->endpoint_address = address;
                info->max_packet_size = packet_size;
                info->interval = descriptors[offset + 6];

                return true;
            }
        }

        offset += descriptor_length;
    }

    return false;
}
