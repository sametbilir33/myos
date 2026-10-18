#ifndef MYOS_XHCI_H
#define MYOS_XHCI_H

#include <stdint.h>
#include <stdbool.h>

bool xhci_probe(uint64_t bar_phys, uint64_t hhdm_offset);

bool xhci_mouse_read_packet(
    int *dx,
    int *dy,
    uint8_t *buttons
);

#endif