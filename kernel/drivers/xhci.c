
#include <stdint.h>
#include <stdbool.h>
#include <serial.h>
#include <xhci.h>

static uint32_t mmio_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static void mmio_write32(volatile uint8_t *base,
                         uint32_t offset,
                         uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

bool xhci_probe(uint64_t bar_phys, uint64_t hhdm_offset)
{
    if (bar_phys == 0) {
        serial_write("[xHCI] Invalid BAR address\n");
        return false;
    }

    volatile uint8_t *mmio =
        (volatile uint8_t *)(uintptr_t)(bar_phys + hhdm_offset);

    uint8_t cap_length = mmio[0];
    uint16_t version = *(volatile uint16_t *)(mmio + 2);

    if (cap_length < 0x20 || cap_length > 0x80) {
        serial_write("[xHCI] Invalid capability length\n");
        return false;
    }

    uint32_t hcsparams1 = mmio_read32(mmio, 0x04);
    uint32_t dboff = mmio_read32(mmio, 0x14) & ~0x3u;
    uint32_t rtsoff = mmio_read32(mmio, 0x18) & ~0x1Fu;

    serial_write("[xHCI] Version: ");
    serial_write_hex(version);
    serial_write("\n[xHCI] Ports: ");
    serial_write_dec((hcsparams1 >> 24) & 0xFFu);
    serial_write("\n[xHCI] Slots: ");
    serial_write_dec(hcsparams1 & 0xFFu);
    serial_write("\n");

    uint32_t op = cap_length;
    uint32_t usbcmd = mmio_read32(mmio, op + 0x00);

    /* Önce varsa çalışmakta olan denetleyiciyi durdur. */
    mmio_write32(mmio, op + 0x00, usbcmd & ~1u);

    uint32_t timeout = 10000000;
    while (timeout--) {
        if ((mmio_read32(mmio, op + 0x04) & (1u << 0)) != 0)
            break;
    }

    if (timeout == 0) {
        serial_write("[xHCI] Controller did not stop\n");
        return false;
    }

    serial_write("[xHCI] Controller stopped\n");

    /* HCRST, USBCMD bit 1. */
    usbcmd = mmio_read32(mmio, op + 0x00);
    mmio_write32(mmio, op + 0x00, usbcmd | (1u << 1));

    timeout = 10000000;
    while (timeout--) {
        uint32_t cmd = mmio_read32(mmio, op + 0x00);
        uint32_t status = mmio_read32(mmio, op + 0x04);

        if ((cmd & (1u << 1)) == 0 &&
            (status & (1u << 11)) == 0)
            break;
    }

    if (timeout == 0) {
        serial_write("[xHCI] Reset timed out\n");
        return false;
    }

    serial_write("[xHCI] Reset complete\n");
    serial_write("[xHCI] Doorbell offset: ");
    serial_write_hex(dboff);
    serial_write("\n[xHCI] Runtime offset: ");
    serial_write_hex(rtsoff);
    serial_write("\n");

    return true;
}
