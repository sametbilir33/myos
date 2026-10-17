
#include <ps2_mouse.h>
#include <serial.h>

#define PS2_STATUS_PORT 0x64
#define PS2_DATA_PORT   0x60
#define PS2_OUTPUT_FULL 0x01
#define PS2_INPUT_FULL  0x02
#define PS2_AUX_DATA    0x20

#define MOUSE_QUEUE_SIZE 64
#define MOUSE_QUEUE_MASK (MOUSE_QUEUE_SIZE - 1)

struct mouse_packet {
    int dx;
    int dy;
    uint8_t buttons;
};

static volatile struct mouse_packet queue[MOUSE_QUEUE_SIZE];
static volatile uint8_t queue_read;
static volatile uint8_t queue_write;

static uint8_t packet[3];
static unsigned int packet_index;

static volatile uint64_t mouse_bytes_received;
static volatile uint64_t mouse_packets_completed;
static volatile uint64_t mouse_packets_queued;

uint64_t ps2_mouse_bytes_count(void)
{
    return mouse_bytes_received;
}

uint64_t ps2_mouse_packets_count(void)
{
    return mouse_packets_completed;
}

uint64_t ps2_mouse_queued_count(void)
{
    return mouse_packets_queued;
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0"
                      : "=a"(value)
                      : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1"
                      :
                      : "a"(value), "Nd"(port));
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

    if (!wait_input_clear())
        return false;

    outb(PS2_STATUS_PORT, 0xA8);

    if (!wait_input_clear())
        return false;

    outb(PS2_STATUS_PORT, 0x20);

    if (!wait_output_full())
        return false;

    uint8_t config = inb(PS2_DATA_PORT);

    /*
     * Bit 1: auxiliary IRQ12 etkin.
     * Bit 5: auxiliary clock devre dışı değil.
     * Global CPU interrupt flag'i henüz kapalı tutuluyor.
     */
    config |= 0x02;
    config &= (uint8_t)~0x20;

    if (!wait_input_clear())
        return false;

    outb(PS2_STATUS_PORT, 0x60);

    if (!wait_input_clear())
        return false;

    outb(PS2_DATA_PORT, config);

    serial_write("[PS2] Sending defaults command...\n");

    if (!mouse_command(0xF6)) {
        serial_write("[PS2] Defaults command failed\n");
        return false;
    }

    serial_write("[PS2] Enabling mouse data...\n");

    if (!mouse_command(0xF4)) {
        serial_write("[PS2] Enable command failed\n");
        return false;
    }

    packet_index = 0;
    queue_read = 0;
    queue_write = 0;

    serial_write("[PS2] Mouse initialized for IRQ12\n");
    return true;
}

/*
 * Yalnızca IRQ12 işleyicisinden çağrılır.
 * Burada framebuffer çizimi veya seri log yapılmaz.
 */
void ps2_mouse_irq_byte(uint8_t data)
{
    mouse_bytes_received++;

    if (packet_index == 0 && (data & 0x08) == 0)
        return;

    packet[packet_index++] = data;

    if (packet_index < 3)
        return;

    packet_index = 0;

    if (packet[0] & 0xC0)
        return;

    uint8_t next = (uint8_t)((queue_write + 1) & MOUSE_QUEUE_MASK);

    /*
     * Kuyruk doluysa en eski paketi düşür.
     * Hareketin en güncel kısmı korunur.
     */
    if (next == queue_read)
        queue_read = (uint8_t)((queue_read + 1) & MOUSE_QUEUE_MASK);

    queue[queue_write].dx = (int)(int8_t)packet[1];
    queue[queue_write].dy = -(int)(int8_t)packet[2];
    queue[queue_write].buttons = packet[0] & 0x07;

    __asm__ volatile ("" ::: "memory");
    queue_write = next;

    mouse_packets_queued++;
}

/*
 * Kuyruktan bir paket alır. Bu fonksiyon donanımı sorgulamaz.
 * Paylaşılan kuyruk durumunu okurken IRQ'ları kısa süre kapatır.
 */
bool ps2_mouse_read_packet(int *dx, int *dy, uint8_t *buttons)
{
    if (!dx || !dy || !buttons)
        return false;

    uint64_t flags;

    __asm__ volatile (
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    if (queue_read == queue_write) {
        if (flags & (1ULL << 9))
            __asm__ volatile ("sti" ::: "memory");

        return false;
    }

    uint8_t index = queue_read;

    *dx = queue[index].dx;
    *dy = queue[index].dy;
    *buttons = queue[index].buttons;

    __asm__ volatile ("" ::: "memory");
    queue_read = (uint8_t)((queue_read + 1) & MOUSE_QUEUE_MASK);

    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");

    return true;
}
