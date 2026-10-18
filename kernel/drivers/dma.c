#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <boot/limine.h>
#include <memory/dma.h>
#include <runtime/serial.h>

#define PAGE_SIZE 4096
#define MAX_REGIONS 128

struct dma_region {
    uint64_t next;
    uint64_t end;
};

static struct dma_region regions[MAX_REGIONS];
static size_t region_count;
static uint64_t direct_map_offset;
static bool initialized;

bool dma_init(
    struct limine_memmap_response *memmap,
    uint64_t hhdm_offset
) {
    if (!memmap || !memmap->entries) {
        serial_write("[DMA] Memory map missing\n");
        return false;
    }

    region_count = 0;
    direct_map_offset = hhdm_offset;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (!entry || entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t base = entry->base;
        uint64_t end = entry->base + entry->length;

        if (end < base)
            continue;

        /* İlk 1 MiB içindeki düşük belleği DMA için kullanma. */
        if (base < 0x100000)
            base = 0x100000;

        /* Başlangıcı sayfa sınırına hizala. */
        if (base > UINT64_MAX - (PAGE_SIZE - 1))
            continue;

        base = (base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        end &= ~(uint64_t)(PAGE_SIZE - 1);

        if (base >= end)
            continue;

        if (region_count >= MAX_REGIONS)
            break;

        regions[region_count].next = base;
        regions[region_count].end = end;
        region_count++;
    }

    initialized = region_count > 0;

    if (initialized) {
        serial_write("[DMA] Usable RAM regions: ");
        serial_write_dec(region_count);
        serial_write("\n");
    } else {
        serial_write("[DMA] No usable RAM regions found\n");
    }

    return initialized;
}

void *dma_alloc(size_t size, uint64_t *physical_address) {
    if (!initialized || size == 0)
        return NULL;

    if ((uint64_t)size > UINT64_MAX - (PAGE_SIZE - 1))
        return NULL;

    uint64_t rounded =
        ((uint64_t)size + PAGE_SIZE - 1) &
        ~(uint64_t)(PAGE_SIZE - 1);

    for (size_t i = 0; i < region_count; i++) {
        uint64_t start = regions[i].next;

        if (start > regions[i].end)
            continue;

        if (rounded > regions[i].end - start)
            continue;

        uint64_t end = start + rounded;

        /* HHDM sanal adresi taşmasın. */
        if (start > UINT64_MAX - direct_map_offset ||
            end - 1 > UINT64_MAX - direct_map_offset) {
            continue;
        }

        regions[i].next = end;

        volatile uint8_t *ptr =
            (volatile uint8_t *)(uintptr_t)
            (start + direct_map_offset);

        for (uint64_t j = 0; j < rounded; j++)
            ptr[j] = 0;

        if (physical_address)
            *physical_address = start;

        return (void *)(uintptr_t)(start + direct_map_offset);
    }

    serial_write("[DMA] Allocation failed, bytes: ");
    serial_write_dec(size);
    serial_write("\n");

    return NULL;
}