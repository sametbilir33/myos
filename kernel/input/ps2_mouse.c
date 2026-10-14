#include <ps2_mouse.h>
#include <serial.h>

#define PS2_STATUS_PORT 0x64
#define PS2_DATA_PORT   0x60
#define PS2_OUTPUT_FULL 0x01
#define PS2_INPUT_FULL  0x02
#define PS2_AUX_DATA    0x20

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static bool wait_input_clear(void)
{
    for (uint32_t i = 0; i < 100000; i++) {
        if ((inb(PS2_STATUS_PORT) & PS2_INPUT_FULL) == 0)
            return true;
    }
    return false;
}

static bool wait_output_full(void)
{
    for (uint32_t i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS_PORT) & PS2_OUTPUT_FULL)
            return true;
    }
    return false;
}

static bool wait_mouse_ack(void)
{
    for (uint32_t i = 0; i < 100000; i++) {
        if (!wait_output_full())
            return false;

        uint8_t status = inb(PS2_STATUS_PORT);
        uint8_t data = inb(PS2_DATA_PORT);

        /* Klavye verisi fare komutunun yanıtı değildir. */
        if ((status & PS2_AUX_DATA) == 0)
            continue;

        if (data == 0xFA)
            return true;

        if (data == 0xFE)
            return false;
    }

    return false;
}

static bool mouse_command(uint8_t command)
{
    if (!wait_input_clear())
        return false;

    outb(PS2_STATUS_PORT, 0xD4);

    if (!wait_input_clear())
        return false;

    outb(PS2_DATA_PORT, command);
    return wait_mouse_ack();
}

bool ps2_mouse_init(void)
{
    serial_write("[PS2] Enabling auxiliary port...\n");

    if (!wait_input_clear()) {
        serial_write("[PS2] Timeout enabling auxiliary port\n");
        return false;
    }
    outb(PS2_STATUS_PORT, 0xA8);

    if (!wait_input_clear()) {
        serial_write("[PS2] Timeout reading controller config\n");
        return false;
    }
    outb(PS2_STATUS_PORT, 0x20);

    if (!wait_output_full()) {
        serial_write("[PS2] Controller config read timed out\n");
        return false;
    }

    uint8_t config = inb(PS2_DATA_PORT);

    /* IRQ12 kapalı, fare saati açık: polling kullanıyoruz. */
    config &= (uint8_t)~0x02;
    config &= (uint8_t)~0x20;

    if (!wait_input_clear()) {
        serial_write("[PS2] Timeout writing controller config\n");
        return false;
    }
    outb(PS2_STATUS_PORT, 0x60);

    if (!wait_input_clear()) {
        serial_write("[PS2] Timeout sending controller config\n");
        return false;
    }
    outb(PS2_DATA_PORT, config);

    serial_write("[PS2] Sending defaults command...\n");
    if (!mouse_command(0xF6)) {
        serial_write("[PS2] Defaults command failed: no mouse ACK\n");
        return false;
    }

    serial_write("[PS2] Enabling mouse data...\n");
    if (!mouse_command(0xF4)) {
        serial_write("[PS2] Enable command failed: no mouse ACK\n");
        return false;
    }

    serial_write("[PS2] Mouse initialized successfully\n");
    return true;
}

bool ps2_mouse_poll(int *dx, int *dy, uint8_t *buttons)
{
    static uint8_t packet[3];
    static unsigned int packet_index;

    bool packet_ready = false;

    if (!dx || !dy || !buttons)
        return false;

    *dx = 0;
    *dy = 0;

    for (;;) {
        uint8_t status = inb(PS2_STATUS_PORT);

        if ((status & PS2_OUTPUT_FULL) == 0)
            break;

        uint8_t data = inb(PS2_DATA_PORT);

        if ((status & PS2_AUX_DATA) == 0)
            continue;

        if (packet_index == 0 && (data & 0x08) == 0)
            continue;

        packet[packet_index++] = data;

        if (packet_index == 3) {
            packet_index = 0;

            if (packet[0] & 0xC0)
                continue;

            *dx = (int)(int8_t)packet[1];
            *dy = -(int)(int8_t)packet[2];
            *buttons = packet[0] & 0x07;
            packet_ready = true;
        }
    }

    return packet_ready;
}
