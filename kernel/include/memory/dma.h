#ifndef MYOS_DMA_H
#define MYOS_DMA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

bool dma_init(
    struct limine_memmap_response *memmap,
    uint64_t hhdm_offset
);

void *dma_alloc(size_t size, uint64_t *physical_address);

#endif