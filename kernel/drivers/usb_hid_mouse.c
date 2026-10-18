
#include <drivers/usb_hid_mouse.h>

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
