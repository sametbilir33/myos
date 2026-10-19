
#include <drivers/pci.h>
#include <runtime/serial.h>
#include <stddef.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1"
                      :
                      : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0"
                      : "=a"(value)
                      : "Nd"(port));
    return value;
}

static uint32_t pci_read32(uint8_t bus,
                           uint8_t device,
                           uint8_t function,
                           uint8_t offset)
{
    uint32_t address =
        0x80000000u |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        ((uint32_t)offset & 0xFCu);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus,
                           uint8_t device,
                           uint8_t function,
                           uint8_t offset)
{
    uint32_t value = pci_read32(bus, device, function, offset);
    return (uint16_t)(value >> ((offset & 2u) * 8u));
}

static uint8_t pci_read8(uint8_t bus,
                         uint8_t device,
                         uint8_t function,
                         uint8_t offset)
{
    uint32_t value = pci_read32(bus, device, function, offset);
    return (uint8_t)(value >> ((offset & 3u) * 8u));
}

static bool pci_read_device(uint8_t bus,
                            uint8_t device,
                            uint8_t function,
                            struct pci_device *out)
{
    uint16_t vendor = pci_read16(bus, device, function, 0x00);

    if (vendor == 0xFFFF)
        return false;

    out->bus = bus;
    out->device = device;
    out->function = function;
    out->vendor_id = vendor;
    out->device_id = pci_read16(bus, device, function, 0x02);
    out->class_code = pci_read8(bus, device, function, 0x0B);
    out->subclass = pci_read8(bus, device, function, 0x0A);
    out->prog_if = pci_read8(bus, device, function, 0x09);
    out->header_type = pci_read8(bus, device, function, 0x0E);

    uint32_t bar_low = pci_read32(bus, device, function, 0x10);
    out->bar0 = 0;
    out->bar0_is_mmio = false;
    out->bar0_is_64bit = false;

    if ((bar_low & 1u) == 0) {
        uint32_t type = (bar_low >> 1) & 3u;
        out->bar0_is_mmio = true;

        if (type == 2u) {
            uint32_t bar_high =
                pci_read32(bus, device, function, 0x14);

            out->bar0 =
                ((uint64_t)bar_high << 32) |
                (uint64_t)(bar_low & ~0xFu);

            out->bar0_is_64bit = true;
        } else if (type == 0u) {
            out->bar0 = (uint64_t)(bar_low & ~0xFu);
        }
    } else {
        out->bar0 = (uint64_t)(bar_low & ~3u);
    }

    return true;
}

static bool is_xhci(const struct pci_device *dev)
{
    return dev->class_code == 0x0C &&
           dev->subclass == 0x03 &&
           dev->prog_if == 0x30;
}

static bool scan_function(uint8_t bus,
                          uint8_t device,
                          uint8_t function,
                          struct pci_device *xhci)
{
    struct pci_device dev;

    if (!pci_read_device(bus, device, function, &dev))
        return false;

    if (is_xhci(&dev)) {
        *xhci = dev;
        return true;
    }

    return false;
}

bool pci_find_xhci(struct pci_device *out)
{
    if (!out)
        return false;

    /*
     * PCI cihazlarını tarar. Birden fazla PCI host bridge
     * bulunan sistemlerde sonraki aşamada bridge traversal
     * ile erişilebilir bus'lar ayrıca ele alınacak.
     */
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            uint16_t vendor =
                pci_read16((uint8_t)bus, device, 0, 0x00);

            if (vendor == 0xFFFF)
                continue;

            uint8_t header =
                pci_read8((uint8_t)bus, device, 0, 0x0E);

            uint8_t functions = (header & 0x80u) ? 8 : 1;

            for (uint8_t function = 0;
                 function < functions;
                 function++) {
                if (scan_function((uint8_t)bus, device,
                                  function, out)) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool pci_init(struct pci_device *out)
{
    if (out == NULL)
        return false;

    serial_write("[PCI] Scanning PCI configuration space\n");

    if (!pci_find_xhci(out)) {
        serial_write("[PCI] No xHCI controller found\n");
        serial_write("[PCI] Check that QEMU has a USB controller\n");
        return false;
    }

    serial_write("[PCI] xHCI controller found at ");
    serial_write_dec(out->bus);
    serial_write(":");
    serial_write_dec(out->device);
    serial_write(".");
    serial_write_dec(out->function);
    serial_write("\n");

    serial_write("[PCI] Vendor ID: ");
    serial_write_hex(out->vendor_id);
    serial_write(" Device ID: ");
    serial_write_hex(out->device_id);
    serial_write("\n");

    serial_write("[PCI] BAR0: ");
    serial_write_hex(out->bar0);
    serial_write("\n");

    if (!out->bar0_is_mmio ||
        !out->bar0_is_64bit ||
        out->bar0 == 0) {
        serial_write("[PCI] Unsupported xHCI BAR0 configuration\n");
        return false;
    }

    serial_write("[PCI] xHCI MMIO BAR identified\n");
    return true;
}

bool pci_enable_bus_mastering(const struct pci_device *dev)
{
    if (dev == 0) {
        return false;
    }

    uint32_t address =
        0x80000000u |
        ((uint32_t)dev->bus << 16) |
        ((uint32_t)dev->device << 11) |
        ((uint32_t)dev->function << 8) |
        0x04u;

    uint32_t command_status;

    uint16_t config_address_port = 0xCF8;
    uint16_t config_data_port = 0xCFC;

    __asm__ volatile (
        "outl %%eax, %%dx"
        :
        : "a"(address), "d"(config_address_port)
        : "memory"
    );

    __asm__ volatile (
        "inl %%dx, %%eax"
        : "=a"(command_status)
        : "d"(config_data_port)
        : "memory"
    );

    uint16_t command = (uint16_t)command_status;
    command |= (1u << 1) | (1u << 2);

    uint32_t new_value =
        (command_status & 0xFFFF0000u) | command;

    __asm__ volatile (
        "outl %%eax, %%dx"
        :
        : "a"(address), "d"(config_address_port)
        : "memory"
    );

    __asm__ volatile (
        "outl %%eax, %%dx"
        :
        : "a"(new_value), "d"(config_data_port)
        : "memory"
    );

    return true;
}
