#include <ps2_mouse.h>

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static bool wait_input_clear(void)
{
    for (uint32_t i = 0; i < 100000; i++) {
        if ((inb(0x64) & 2) == 0)
            return true;
    }

    return false;
}

static bool wait_output_full(void)
{
    for (uint32_t i = 0; i < 100000; i++) {
        if (inb(0x64) & 1)
            return true;
    }

    return false;
}

static bool mouse_command(uint8_t command)
{
    if (!wait_input_clear())
        return false;

    outb(0x64, 0xD4);

    if (!wait_input_clear())
        return false;

    outb(0x60, command);

    if (!wait_output_full())
        return false;

    return inb(0x60) == 0xFA;
}

bool ps2_mouse_init(void)
{
    /* Yardımcı PS/2 aygıtının saatini etkinleştir. */
    if (!wait_input_clear())
        return false;

    outb(0x64, 0xA8);

    /* Denetleyicinin yapılandırma baytını oku. */
    if (!wait_input_clear())
        return false;

    outb(0x64, 0x20);

    if (!wait_output_full())
        return false;

    uint8_t config = inb(0x60);

    /* Fare kesmesini kapalı tut, fare saatini açık bırak. */
    config &= (uint8_t)~0x02;
    config &= (uint8_t)~0x20;

    if (!wait_input_clear())
        return false;

    outb(0x64, 0x60);

    if (!wait_input_clear())
        return false;

    outb(0x60, config);

    /* Varsayılan ayarları uygula ve veri akışını başlat. */
    if (!mouse_command(0xF6))
        return false;

    if (!mouse_command(0xF4))
        return false;

    return true;
}

bool ps2_mouse_poll(int *dx, int *dy, uint8_t *buttons)
{
    static uint8_t packet[3];
    static unsigned int packet_index;

    bool packet_ready = false;

    *dx = 0;
    *dy = 0;

    while (inb(0x64) & 1) {
        uint8_t status = inb(0x64);
        uint8_t data = inb(0x60);

        /* Klavye verisini tüket ama fare paketi olarak yorumlama. */
        if ((status & 0x20) == 0)
            continue;

        if (packet_index == 0 && (data & 0x08) == 0)
            continue;

        packet[packet_index++] = data;

        if (packet_index == 3) {
            packet_index = 0;

            /* Taşma bayraklı paketleri kullanma. */
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