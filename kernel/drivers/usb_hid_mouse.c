
#include <drivers/usb_hid_mouse.h>
#include <drivers/xhci.h>
#include <memory/dma.h>
#include <runtime/serial.h>

#include <stddef.h>
#include <stdint.h>

#define USB_HID_DESCRIPTOR_BUFFER_SIZE 4096u

static uint8_t *descriptor_buffer;
static uint64_t descriptor_buffer_phys;

static uint8_t *report_buffer;
static uint64_t report_buffer_phys;

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
        buttons == NULL ||
        length < 3) {
        return false;
    }

    *buttons = report[0] & 0x07u;
    *dx = (int)(int8_t)report[1];
    *dy = (int)(int8_t)report[2];

    return true;
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

bool usb_hid_mouse_init(uint8_t slot_id, uint8_t speed)
{
    descriptor_buffer = dma_alloc(
        USB_HID_DESCRIPTOR_BUFFER_SIZE,
        &descriptor_buffer_phys
    );

    report_buffer = dma_alloc(
        USB_HID_DESCRIPTOR_BUFFER_SIZE,
        &report_buffer_phys
    );

    if (descriptor_buffer == NULL || report_buffer == NULL) {
        serial_write("[USB HID] DMA buffer allocation failed\n");
        return false;
    }

    serial_write("[USB HID] Reading Device Descriptor\n");

    if (!xhci_control_transfer(
            slot_id,
            0x80,
            0x06,
            0x0100,
            0,
            18,
            descriptor_buffer_phys)) {
        serial_write("[USB HID] Device Descriptor request failed\n");
        return false;
    }

    if (descriptor_buffer[0] != 18 ||
        descriptor_buffer[1] != 1) {
        serial_write("[USB HID] Invalid Device Descriptor\n");
        return false;
    }

    uint16_t vendor_id =
        (uint16_t)descriptor_buffer[8] |
        ((uint16_t)descriptor_buffer[9] << 8);

    uint16_t product_id =
        (uint16_t)descriptor_buffer[10] |
        ((uint16_t)descriptor_buffer[11] << 8);

    serial_write("[USB HID] VID: ");
    serial_write_hex(vendor_id);
    serial_write(" PID: ");
    serial_write_hex(product_id);
    serial_write("\n");

    serial_write("[USB HID] Reading Configuration Descriptor header\n");

    if (!xhci_control_transfer(
            slot_id,
            0x80,
            0x06,
            0x0200,
            0,
            9,
            descriptor_buffer_phys)) {
        serial_write("[USB HID] Configuration header request failed\n");
        return false;
    }

    if (descriptor_buffer[0] < 9 ||
        descriptor_buffer[1] != 2) {
        serial_write("[USB HID] Invalid Configuration Descriptor\n");
        return false;
    }

    uint16_t total_length =
        (uint16_t)descriptor_buffer[2] |
        ((uint16_t)descriptor_buffer[3] << 8);

    uint8_t configuration_value = descriptor_buffer[5];

    if (total_length < 9 ||
        total_length > USB_HID_DESCRIPTOR_BUFFER_SIZE) {
        serial_write("[USB HID] Invalid configuration length\n");
        return false;
    }

    serial_write("[USB HID] Reading full Configuration Descriptor\n");

    if (!xhci_control_transfer(
            slot_id,
            0x80,
            0x06,
            0x0200,
            0,
            total_length,
            descriptor_buffer_phys)) {
        serial_write("[USB HID] Configuration Descriptor request failed\n");
        return false;
    }

    struct usb_hid_mouse_endpoint_info endpoint_info = {0};

    if (!usb_hid_mouse_find_boot_endpoint(
            descriptor_buffer,
            total_length,
            &endpoint_info)) {
        serial_write("[USB HID] Boot Mouse endpoint not found\n");
        return false;
    }

    serial_write("[USB HID] Interface: ");
    serial_write_dec(endpoint_info.interface_number);
    serial_write("\n[USB HID] Endpoint: ");
    serial_write_hex(endpoint_info.endpoint_address);
    serial_write("\n[USB HID] Packet size: ");
    serial_write_dec(endpoint_info.max_packet_size);
    serial_write("\n[USB HID] Interval: ");
    serial_write_dec(endpoint_info.interval);
    serial_write("\n");

    /*
     * Configure the USB interrupt-IN endpoint.
     * The xHCI layer owns the endpoint ring and TRBs.
     * This module owns the HID report buffer.
     */
    if (!xhci_interrupt_in_configure(
            slot_id,
            endpoint_info.endpoint_address,
            endpoint_info.max_packet_size,
            endpoint_info.interval,
            speed,
            report_buffer,
            report_buffer_phys)) {
        serial_write("[USB HID] Endpoint configuration failed\n");
        return false;
    }

    /* SET_CONFIGURATION */
    if (!xhci_control_transfer(
            slot_id,
            0x00,
            0x09,
            configuration_value,
            0,
            0,
            0)) {
        serial_write("[USB HID] SET_CONFIGURATION failed\n");
        return false;
    }

    /* SET_PROTOCOL: Boot Protocol (value 0). */
    if (!xhci_control_transfer(
            slot_id,
            0x21,
            0x0B,
            0,
            endpoint_info.interface_number,
            0,
            0)) {
        serial_write("[USB HID] SET_PROTOCOL Boot failed\n");
        return false;
    }

    if (!xhci_interrupt_in_start()) {
        serial_write("[USB HID] Interrupt-IN start failed\n");
        return false;
    }

    serial_write("[USB HID] Boot Mouse initialized\n");
    serial_write("[USB HID] Interrupt-IN polling started\n");

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

    if (dx == NULL || dy == NULL || buttons == NULL)
        return false;

    if (!xhci_interrupt_in_read(
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
