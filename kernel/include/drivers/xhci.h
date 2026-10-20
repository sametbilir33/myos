#ifndef MYOS_XHCI_H
#define MYOS_XHCI_H

#include <stdbool.h>
#include <stdint.h>

bool xhci_probe_all(
    uint64_t bar_phys,
    uint64_t hhdm_offset,
    uint8_t *slot_ids,
    uint8_t *speeds,
    uint8_t capacity,
    uint8_t *out_count
);

bool xhci_control_transfer(
    uint8_t slot_id,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint16_t length,
    uint64_t data_phys
);

bool xhci_interrupt_in_configure(
    uint8_t slot_id,
    uint8_t endpoint_address,
    uint16_t max_packet_size,
    uint8_t interval,
    uint8_t speed,
    uint8_t *buffer,
    uint64_t buffer_phys
);

bool xhci_interrupt_in_start(uint8_t slot_id);

bool xhci_interrupt_in_read(
    uint8_t slot_id,
    uint8_t *report,
    uint16_t capacity,
    uint16_t *length
);

#endif