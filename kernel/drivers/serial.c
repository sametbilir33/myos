#include <serial.h>
#include <stdint.h>

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void serial_write_char(char c)
{
    for (unsigned int i = 0; i < 1000000; i++) {
        if (inb(COM1 + 5) & 0x20) {
            outb(COM1, (uint8_t)c);
            return;
        }
    }
}

void serial_write(const char *text)
{
    while (*text) {
        if (*text == '\n')
            serial_write_char('\r');

        serial_write_char(*text++);
    }
}

void serial_write_hex(unsigned long long value)
{
    static const char digits[] = "0123456789ABCDEF";

    serial_write("0x");

    int started = 0;

    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned int digit = (unsigned int)((value >> shift) & 0xF);

        if (digit || started || shift == 0) {
            serial_write_char(digits[digit]);
            started = 1;
        }
    }
}

void serial_write_dec(unsigned long long value)
{
    char buffer[21];
    unsigned int i = 0;

    if (value == 0) {
        serial_write_char('0');
        return;
    }

    while (value && i < sizeof(buffer)) {
        buffer[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0)
        serial_write_char(buffer[--i]);
}