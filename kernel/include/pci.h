
#ifndef MYOS_PCI_H
#define MYOS_PCI_H

#include <stdbool.h>
#include <stdint.h>

struct pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;

    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;

    uint64_t bar0;
    bool bar0_is_mmio;
    bool bar0_is_64bit;
};

void pci_init(void);
bool pci_find_xhci(struct pci_device *out);
bool pci_enable_bus_mastering(const struct pci_device *dev);

#endif
