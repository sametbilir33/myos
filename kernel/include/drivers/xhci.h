
#ifndef MYOS_XHCI_H
#define MYOS_XHCI_H

#include <stdint.h>
#include <stdbool.h>

bool xhci_probe(uint64_t bar_phys, uint64_t hhdm_offset);

bool xhci_mouse_read_report(
    uint8_t *report,
    uint16_t capacity,
    uint16_t *length
);

#endif
