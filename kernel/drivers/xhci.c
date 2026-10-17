#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <serial.h>
#include <xhci.h>
#include <dma.h>

#define XHCI_TRBS_PER_RING 256u
#define XHCI_TRB_TYPE_LINK 6u

#define USBCMD_RUN       (1u << 0)
#define USBCMD_RESET     (1u << 1)
#define USBCMD_INTERRUPT (1u << 2)

#define USBSTS_HALTED    (1u << 0)
#define USBSTS_NOT_READY (1u << 11)

struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};

struct xhci_erst_entry {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
};

static volatile uint8_t *xhci_mmio;
static uint32_t xhci_op;
static uint32_t xhci_doorbell;
static uint32_t xhci_runtime;

static struct xhci_trb *command_ring;
static uint64_t command_ring_phys;

static struct xhci_trb *event_ring;
static uint64_t event_ring_phys;

static struct xhci_erst_entry *erst;
static uint64_t erst_phys;

static uint64_t *dcbaa;
static uint64_t dcbaa_phys;

static uint16_t event_dequeue;
static uint8_t event_cycle = 1;

static uint16_t command_enqueue;
static uint8_t command_cycle = 1;

static uint32_t mmio_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static void mmio_write32(
    volatile uint8_t *base,
    uint32_t offset,
    uint32_t value
) {
    *(volatile uint32_t *)(base + offset) = value;
}

static uint64_t mmio_read64(volatile uint8_t *base, uint32_t offset)
{
    uint32_t low = mmio_read32(base, offset);
    uint32_t high = mmio_read32(base, offset + 4);

    return ((uint64_t)high << 32) | low;
}

/* Yüksek yarıyı önce, düşük yarıyı en son yaz. */
static void mmio_write64(
    volatile uint8_t *base,
    uint32_t offset,
    uint64_t value
) {
    mmio_write32(base, offset + 4, (uint32_t)(value >> 32));
    mmio_write32(base, offset, (uint32_t)value);
}

static bool wait_register(
    volatile uint8_t *base,
    uint32_t offset,
    uint32_t mask,
    uint32_t expected
) {
    for (uint32_t i = 0; i < 10000000u; i++) {
        if ((mmio_read32(base, offset) & mask) == expected)
            return true;
    }

    return false;
}

static bool xhci_allocate_rings(
    uint32_t hcsparams1,
    uint32_t hcsparams2
) {
    uint32_t max_slots = hcsparams1 & 0xFFu;

    if (max_slots == 0) {
        serial_write("[xHCI] No device slots available\n");
        return false;
    }

    command_ring = dma_alloc(
        XHCI_TRBS_PER_RING * sizeof(struct xhci_trb),
        &command_ring_phys
    );

    event_ring = dma_alloc(
        XHCI_TRBS_PER_RING * sizeof(struct xhci_trb),
        &event_ring_phys
    );

    erst = dma_alloc(sizeof(struct xhci_erst_entry), &erst_phys);

    dcbaa = dma_alloc(256u * sizeof(uint64_t), &dcbaa_phys);

    if (!command_ring || !event_ring || !erst || !dcbaa) {
        serial_write("[xHCI] Ring/DCBAA DMA allocation failed\n");
        return false;
    }

    /*
     * HCSPARAMS2:
     * Max Scratchpad Buffers Hi = bits 31:27
     * Max Scratchpad Buffers Lo = bits 25:21
     */
    uint32_t scratchpad_count =
        (((hcsparams2 >> 27) & 0x1Fu) << 5) |
        ((hcsparams2 >> 21) & 0x1Fu);

    if (scratchpad_count != 0) {
        uint64_t scratchpad_array_phys = 0;

        uint64_t *scratchpad_array = dma_alloc(
            (size_t)scratchpad_count * sizeof(uint64_t),
            &scratchpad_array_phys
        );

        if (!scratchpad_array) {
            serial_write("[xHCI] Scratchpad array allocation failed\n");
            return false;
        }

        for (uint32_t i = 0; i < scratchpad_count; i++) {
            uint64_t page_phys = 0;

            if (!dma_alloc(4096, &page_phys)) {
                serial_write("[xHCI] Scratchpad page allocation failed\n");
                return false;
            }

            scratchpad_array[i] = page_phys;
        }

        /* DCBAA entry zero points to the scratchpad pointer array. */
        dcbaa[0] = scratchpad_array_phys;
    }

    /*
     * Link TRB: return to the start of the command ring.
     * Type = 6, Toggle Cycle = 1, Cycle = 1.
     */
    struct xhci_trb *link =
        &command_ring[XHCI_TRBS_PER_RING - 1];

    link->parameter = command_ring_phys;
    link->status = 0;
    link->control =
        (XHCI_TRB_TYPE_LINK << 10) |
        (1u << 1) |
        1u;

    /* One event-ring segment, containing 256 TRBs. */
    erst->base = event_ring_phys;
    erst->size = XHCI_TRBS_PER_RING;
    erst->reserved = 0;

    event_dequeue = 0;
    event_cycle = 1;

    __asm__ volatile ("" ::: "memory");

    serial_write("[xHCI] Command ring physical: ");
    serial_write_hex(command_ring_phys);
    serial_write("\n");

    serial_write("[xHCI] Event ring physical: ");
    serial_write_hex(event_ring_phys);
    serial_write("\n");

    serial_write("[xHCI] DCBAA physical: ");
    serial_write_hex(dcbaa_phys);
    serial_write("\n");

    return true;
}

static void xhci_configure_controller(
    uint32_t hcsparams1
) {
    uint32_t max_slots = hcsparams1 & 0xFFu;
    mmio_write64(xhci_mmio, xhci_op + 0x30, dcbaa_phys);

    /* CRCR: önce düşük, sonra yüksek 32 bit. */
    mmio_write32(
        xhci_mmio,
        xhci_op + 0x18,
        (uint32_t)(command_ring_phys | 1u)
    );

    mmio_write32(
        xhci_mmio,
        xhci_op + 0x1C,
        (uint32_t)(command_ring_phys >> 32)
    );

    /* Enable the supported device slots. */
    mmio_write32(xhci_mmio, xhci_op + 0x38, max_slots);

    /*
     * Primary interrupter, at Runtime + 0x20.
     * We poll the event ring for now, so interrupts stay disabled.
     */
    uint32_t intr = xhci_runtime + 0x20;

    mmio_write32(xhci_mmio, intr + 0x04, 0); /* IMOD */
    mmio_write32(xhci_mmio, intr + 0x08, 1); /* ERSTSZ */
    /* ERSTBA: önce düşük 32 bit, sonra yüksek 32 bit. */
    mmio_write32(xhci_mmio, intr + 0x10, (uint32_t)erst_phys);
    mmio_write32(xhci_mmio, intr + 0x14, (uint32_t)(erst_phys >> 32));
    mmio_write64(xhci_mmio, intr + 0x18, event_ring_phys);

    __asm__ volatile ("" ::: "memory");

    serial_write("[xHCI] Command ring configured\n");
    serial_write("[xHCI] Event ring configured\n");
}

static bool xhci_submit_command(
    uint64_t parameter,
    uint32_t status,
    uint32_t control,
    uint32_t *completion_control
)
{
    struct xhci_trb *trb = &command_ring[command_enqueue];

    trb->parameter = parameter;
    trb->status = status;
    trb->control = control | command_cycle;

    uint64_t command_phys =
        command_ring_phys +
        (uint64_t)command_enqueue * sizeof(struct xhci_trb);

    __asm__ volatile ("mfence" ::: "memory");

    command_enqueue++;

    if (command_enqueue >= XHCI_TRBS_PER_RING - 1) {
        command_enqueue = 0;
        command_cycle ^= 1;

        command_ring[XHCI_TRBS_PER_RING - 1].control =
            (XHCI_TRB_TYPE_LINK << 10) |
            (1u << 1) |
            command_cycle;

        __asm__ volatile ("mfence" ::: "memory");
    }

    serial_write("[xHCI] Submit command TRB: ");
    serial_write_hex(command_phys);
    serial_write("\n");

    /* Doorbell 0, Host Controller Command Ring. */
    mmio_write32(xhci_mmio, xhci_doorbell, 0);

    uint32_t intr = xhci_runtime + 0x20;

    for (uint32_t i = 0; i < 10000000u; i++) {
        volatile struct xhci_trb *event =
            &event_ring[event_dequeue];

        uint32_t event_ctl = event->control;

        if ((event_ctl & 1u) != event_cycle)
            continue;

        uint32_t type = (event_ctl >> 10) & 0x3Fu;
        uint32_t event_status = event->status;
        uint64_t event_parameter = event->parameter;

        event_dequeue++;

        if (event_dequeue >= XHCI_TRBS_PER_RING) {
            event_dequeue = 0;
            event_cycle ^= 1;
        }

        uint64_t next_event_phys =
            event_ring_phys +
            (uint64_t)event_dequeue * sizeof(struct xhci_trb);

        mmio_write64(
            xhci_mmio,
            intr + 0x18,
            next_event_phys | 8u
        );

        if (type != 33u)
            continue;

        uint32_t completion_code = event_status >> 24;

        serial_write("[xHCI] Command completion code: ");
        serial_write_dec(completion_code);
        serial_write("\n");

        if (completion_control != NULL)
            *completion_control = event_ctl;

        serial_write("[xHCI] Completed command pointer: ");
        serial_write_hex(event_parameter);
        serial_write("\n");

        return completion_code == 1u;
    }

    serial_write("[xHCI] Command completion timed out\n");
    return false;
}

static bool xhci_test_command_ring(void)
{
    serial_write("[xHCI] Testing command ring with No-op\n");

    if (!xhci_submit_command(
            0,
            0,
            23u << 10,
            NULL)) {
        serial_write("[xHCI] Command ring test FAILED\n");
        return false;
    }

    serial_write("[xHCI] Command ring test PASSED\n");
    return true;
}

static bool xhci_reset_connected_port(uint32_t ports, uint8_t *port_id)
{
    const uint32_t portsc_change_bits = 0x7Fu << 17;

    for (uint32_t i = 0; i < ports; i++) {
        uint32_t offset = xhci_op + 0x400 + i * 0x10;
        uint32_t portsc = mmio_read32(xhci_mmio, offset);

        if ((portsc & 1u) == 0)
            continue;

        serial_write("[xHCI] Resetting connected port ");
        serial_write_dec(i + 1);
        serial_write("\n");

        /*
         * PORTSC change bits are write-one-to-clear.
         * Clear them from our write value so we do not
         * accidentally acknowledge unrelated changes.
         */
        uint32_t value =
            portsc & ~(portsc_change_bits | (1u << 4) |
                       (1u << 1) | (1u << 31));

        /* USB2 port reset: PR, bit 4. */
        mmio_write32(xhci_mmio, offset, value | (1u << 4));

        if (!wait_register(
                xhci_mmio,
                offset,
                1u << 4,
                0)) {
            serial_write("[xHCI] Port reset timed out\n");
            return false;
        }

        portsc = mmio_read32(xhci_mmio, offset);

        serial_write("[xHCI] PORTSC after reset: ");
        serial_write_hex(portsc);
        serial_write("\n");

        if ((portsc & 1u) == 0) {
            serial_write("[xHCI] Device disconnected during reset\n");
            return false;
        }

        if ((portsc & (1u << 1)) == 0) {
            serial_write("[xHCI] Port did not become enabled\n");
            return false;
        }

        *port_id = (uint8_t)(i + 1);
        serial_write("[xHCI] Port reset and enabled\n");
        return true;
    }

    serial_write("[xHCI] No connected USB port found\n");
    return false;
}

static bool xhci_enable_slot(uint8_t *slot_id)
{
    uint32_t completion_control = 0;

    serial_write("[xHCI] Sending Enable Slot command\n");

    /*
     * Enable Slot command: TRB type 9.
     * Slot Type is zero for the default slot type.
     */
    if (!xhci_submit_command(
            0,
            0,
            9u << 10,
            &completion_control)) {
        serial_write("[xHCI] Enable Slot failed\n");
        return false;
    }

    *slot_id = (uint8_t)(completion_control >> 24);

    if (*slot_id == 0) {
        serial_write("[xHCI] Controller returned invalid slot ID\n");
        return false;
    }

    serial_write("[xHCI] Slot enabled, ID: ");
    serial_write_dec(*slot_id);
    serial_write("\n");

    return true;
}

bool xhci_probe(uint64_t bar_phys, uint64_t hhdm_offset)
{
    if (bar_phys == 0) {
        serial_write("[xHCI] Invalid BAR address\n");
        return false;
    }

    xhci_mmio = (volatile uint8_t *)(uintptr_t)
        (bar_phys + hhdm_offset);

    uint8_t cap_length = xhci_mmio[0];
    uint16_t version = *(volatile uint16_t *)(xhci_mmio + 2);

    if (cap_length < 0x20 || cap_length > 0x80) {
        serial_write("[xHCI] Invalid capability length\n");
        return false;
    }

    uint32_t hcsparams1 = mmio_read32(xhci_mmio, 0x04);
    uint32_t hcsparams2 = mmio_read32(xhci_mmio, 0x08);
    uint32_t dboff = mmio_read32(xhci_mmio, 0x14) & ~0x3u;
    uint32_t rtsoff = mmio_read32(xhci_mmio, 0x18) & ~0x1Fu;

    xhci_op = cap_length;
    xhci_doorbell = dboff;
    xhci_runtime = rtsoff;

    serial_write("[xHCI] Version: ");
    serial_write_hex(version);
    serial_write("\n[xHCI] Ports: ");
    serial_write_dec((hcsparams1 >> 24) & 0xFFu);
    serial_write("\n[xHCI] Slots: ");
    serial_write_dec(hcsparams1 & 0xFFu);
    serial_write("\n");

    /* Stop the controller before resetting it. */
    uint32_t cmd = mmio_read32(xhci_mmio, xhci_op);
    mmio_write32(
        xhci_mmio,
        xhci_op,
        cmd & ~(USBCMD_RUN | USBCMD_INTERRUPT)
    );

    if (!wait_register(
            xhci_mmio,
            xhci_op + 0x04,
            USBSTS_HALTED,
            USBSTS_HALTED)) {
        serial_write("[xHCI] Controller did not stop\n");
        return false;
    }

    serial_write("[xHCI] Controller stopped\n");

    cmd = mmio_read32(xhci_mmio, xhci_op);
    mmio_write32(xhci_mmio, xhci_op, cmd | USBCMD_RESET);

    if (!wait_register(
            xhci_mmio,
            xhci_op,
            USBCMD_RESET,
            0)) {
        serial_write("[xHCI] Reset timed out\n");
        return false;
    }

    if (!wait_register(
            xhci_mmio,
            xhci_op + 0x04,
            USBSTS_NOT_READY,
            0)) {
        serial_write("[xHCI] Controller not ready after reset\n");
        return false;
    }

    serial_write("[xHCI] Reset complete\n");

    if (!xhci_allocate_rings(hcsparams1, hcsparams2))
        return false;

    xhci_configure_controller(hcsparams1);

    /* Start the controller, with interrupt delivery disabled. */
    cmd = mmio_read32(xhci_mmio, xhci_op);
    cmd &= ~USBCMD_INTERRUPT;
    cmd |= USBCMD_RUN;
    mmio_write32(xhci_mmio, xhci_op, cmd);

    if (!wait_register(
            xhci_mmio,
            xhci_op + 0x04,
            USBSTS_HALTED,
            0)) {
        serial_write("[xHCI] Controller failed to start\n");
        return false;
    }

    serial_write("[xHCI] Controller running\n");
    serial_write("[xHCI] Doorbell offset: ");
    serial_write_hex(xhci_doorbell);
    serial_write("\n[xHCI] Runtime offset: ");
    serial_write_hex(xhci_runtime);
    serial_write("\n");

    if (!xhci_test_command_ring()) {
        serial_write("[xHCI] Stopping USB initialization after ring test\n");
        return false;
    }

    uint32_t ports = (hcsparams1 >> 24) & 0xFFu;
    uint8_t connected_port = 0;
    uint8_t slot_id = 0;

    if (!xhci_reset_connected_port(ports, &connected_port)) {
        serial_write("[xHCI] USB port initialization failed\n");
        return false;
    }

    serial_write("[xHCI] Connected USB port: ");
    serial_write_dec(connected_port);
    serial_write("\n");

    if (!xhci_enable_slot(&slot_id)) {
        serial_write("[xHCI] Cannot enable USB device slot\n");
        return false;
    }

    for (uint32_t i = 0; i < ports; i++) {
        uint32_t portsc_offset = xhci_op + 0x400 + i * 0x10;
        uint32_t portsc = mmio_read32(xhci_mmio, portsc_offset);

        serial_write("[xHCI] Port ");
        serial_write_dec(i + 1);
        serial_write(": PORTSC=");
        serial_write_hex(portsc);

        if (portsc & 1u)
            serial_write(" connected");

        serial_write("\n");
    }

    return true;
}