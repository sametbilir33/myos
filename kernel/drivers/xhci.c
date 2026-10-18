#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <runtime/serial.h>
#include <drivers/xhci.h>
#include <memory/dma.h>

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

static uint8_t *input_context;
static uint64_t input_context_phys;

static uint8_t *device_context;
static uint64_t device_context_phys;

static struct xhci_trb *ep0_ring;
static uint64_t ep0_ring_phys;
static uint16_t ep0_enqueue = 0;
static uint8_t ep0_cycle = 1;
static uint32_t xhci_context_size;

static uint8_t *usb_descriptor_buffer;
static uint64_t usb_descriptor_buffer_phys;

static uint8_t *usb_mouse_buffer;
static uint64_t usb_mouse_buffer_phys;

static struct xhci_trb *usb_mouse_ring;
static uint64_t usb_mouse_ring_phys;

static uint16_t usb_mouse_enqueue;
static uint8_t usb_mouse_cycle = 1;

static uint8_t usb_mouse_slot;
static uint8_t usb_mouse_dci;
static uint8_t usb_mouse_interface;
static uint8_t usb_mouse_packet_size;

static bool usb_mouse_ready;
static bool usb_mouse_transfer_pending;

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

    command_ring[XHCI_TRBS_PER_RING - 1].control =
        (XHCI_TRB_TYPE_LINK << 10) |
        (1u << 1) |
        command_cycle;

    __asm__ volatile ("mfence" ::: "memory");

    command_cycle ^= 1;
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

static bool xhci_prepare_and_address_device(
    uint8_t port_id,
    uint8_t slot_id
) {
    uint32_t hccparams1 = mmio_read32(xhci_mmio, 0x10);

    /* HCCPARAMS1.CSZ: 0 = 32-byte contexts, 1 = 64-byte contexts. */
    xhci_context_size = (hccparams1 & (1u << 2)) ? 64u : 32u;

    device_context = dma_alloc(
        (size_t)xhci_context_size * 32u,
        &device_context_phys
    );

    input_context = dma_alloc(
        (size_t)xhci_context_size * 33u,
        &input_context_phys
    );

    ep0_ring = dma_alloc(
        XHCI_TRBS_PER_RING * sizeof(struct xhci_trb),
        &ep0_ring_phys
    );

    if (!device_context || !input_context || !ep0_ring) {
        serial_write("[xHCI] Device context allocation failed\n");
        return false;
    }

    uint32_t portsc_offset =
        xhci_op + 0x400u + ((uint32_t)port_id - 1u) * 0x10u;

    uint32_t portsc = mmio_read32(xhci_mmio, portsc_offset);
    uint32_t speed = (portsc >> 10) & 0xFu;

    if (speed == 0 || speed > 4) {
        serial_write("[xHCI] Unsupported USB port speed: ");
        serial_write_dec(speed);
        serial_write("\n");
        return false;
    }

    uint32_t max_packet_size;

    if (speed == 3)
        max_packet_size = 64;
    else if (speed == 4)
        max_packet_size = 512;
    else
        max_packet_size = 8;

    /*
     * The input context contains:
     *   Context 0: Input Control Context
     *   Context 1: Slot Context
     *   Context 2: EP0 Endpoint Context
     */
    uint32_t *input_words = (uint32_t *)input_context;
    uint32_t *slot_input =
        (uint32_t *)(input_context + xhci_context_size);
    uint32_t *ep0_input =
        (uint32_t *)(input_context + 2u * xhci_context_size);

    /* Add Slot Context and EP0 Endpoint Context. */
    input_words[1] = (1u << 0) | (1u << 1);

    /* Slot Context: speed and one configured endpoint context. */
    slot_input[0] = (speed << 20) | (1u << 27);

    /* Slot Context: root hub port number. */
    slot_input[1] = (uint32_t)port_id << 16;

    /*
     * EP0 Endpoint Context:
     * CErr = 3, endpoint type = Control (4),
     * and the initial maximum packet size.
     */
    ep0_input[1] =
        (3u << 1) |
        (4u << 3) |
        (max_packet_size << 16);

    /* EP0 transfer ring dequeue pointer, with DCS = 1. */
    uint64_t *ep0_dequeue = (uint64_t *)&ep0_input[2];
    *ep0_dequeue = ep0_ring_phys | 1u;

    /* Average TRB length for control transfers. */
    ep0_input[4] = 8u;

    /* Initialize the EP0 ring's Link TRB. */
    struct xhci_trb *link =
        &ep0_ring[XHCI_TRBS_PER_RING - 1];

    link->parameter = ep0_ring_phys;
    link->status = 0;
    link->control =
        (XHCI_TRB_TYPE_LINK << 10) |
        (1u << 1) |
        1u;

    /* Install this slot's Device Context in the DCBAA. */
    dcbaa[slot_id] = device_context_phys;

    __asm__ volatile ("mfence" ::: "memory");

    serial_write("[xHCI] Context size: ");
    serial_write_dec(xhci_context_size);
    serial_write("\n[xHCI] USB port speed ID: ");
    serial_write_dec(speed);
    serial_write("\n[xHCI] EP0 max packet size: ");
    serial_write_dec(max_packet_size);
    serial_write("\n[xHCI] Device Context: ");
    serial_write_hex(device_context_phys);
    serial_write("\n[xHCI] Input Context: ");
    serial_write_hex(input_context_phys);
    serial_write("\n[xHCI] EP0 ring: ");
    serial_write_hex(ep0_ring_phys);
    serial_write("\n");

    serial_write("[xHCI] Sending Address Device command\n");

    /*
     * Address Device command: TRB type 11.
     * The slot ID occupies control bits 31:24.
     */
    if (!xhci_submit_command(
            input_context_phys,
            0,
            (11u << 10) | ((uint32_t)slot_id << 24),
            NULL)) {
        serial_write("[xHCI] Address Device command failed\n");
        return false;
    }

    serial_write("[xHCI] USB device addressed successfully\n");
    return true;
}

static bool xhci_pop_event(struct xhci_trb *out)
{
    volatile struct xhci_trb *event =
        &event_ring[event_dequeue];

    uint32_t control = event->control;

    if ((control & 1u) != event_cycle)
        return false;

    __asm__ volatile ("" ::: "memory");

    out->parameter = event->parameter;
    out->status = event->status;
    out->control = control;

    event_dequeue++;

    if (event_dequeue >= XHCI_TRBS_PER_RING) {
        event_dequeue = 0;
        event_cycle ^= 1;
    }

    uint32_t intr = xhci_runtime + 0x20;

    uint64_t next_event_phys =
        event_ring_phys +
        (uint64_t)event_dequeue * sizeof(struct xhci_trb);

    mmio_write64(
        xhci_mmio,
        intr + 0x18,
        next_event_phys | 8u
    );

    return true;
}

static void xhci_ep0_push_trb(
    uint64_t parameter,
    uint32_t status,
    uint32_t control
)
{
    struct xhci_trb *trb = &ep0_ring[ep0_enqueue];

    trb->parameter = parameter;
    trb->status = status;
    trb->control = control | ep0_cycle;

    __asm__ volatile ("mfence" ::: "memory");

    ep0_enqueue++;

    if (ep0_enqueue >= XHCI_TRBS_PER_RING - 1) {
        ep0_enqueue = 0;

        ep0_ring[XHCI_TRBS_PER_RING - 1].control =
            (XHCI_TRB_TYPE_LINK << 10) |
            (1u << 1) |
            ep0_cycle;

        __asm__ volatile ("mfence" ::: "memory");

        ep0_cycle ^= 1;
    }
}

static bool xhci_wait_control_transfer(uint8_t slot_id)
{
    for (uint32_t i = 0; i < 10000000u; i++) {
        struct xhci_trb event;

        if (!xhci_pop_event(&event))
            continue;

        uint32_t type = (event.control >> 10) & 0x3Fu;

        if (type != 32u)
            continue;

        uint8_t event_slot =
            (uint8_t)(event.control >> 24);

        uint8_t endpoint_id =
            (uint8_t)((event.control >> 16) & 0x1Fu);

        if (event_slot != slot_id || endpoint_id != 1)
            continue;

        uint32_t code = event.status >> 24;

        serial_write("[USB] EP0 completion code: ");
        serial_write_dec(code);
        serial_write("\n");

        /*
         * 1  = Success
         * 13 = Short Packet, which can be valid for IN requests.
         */
        return code == 1u || code == 13u;
    }

    serial_write("[USB] EP0 transfer timed out\n");
    return false;
}

static bool xhci_control_transfer(
    uint8_t slot_id,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint16_t length,
    uint64_t data_phys
)
{
    if (length != 0 && data_phys == 0)
        return false;

    /*
     * USB SETUP packet is 8 bytes, stored in little-endian order.
     */
    uint64_t setup =
        (uint64_t)request_type |
        ((uint64_t)request << 8) |
        ((uint64_t)value << 16) |
        ((uint64_t)index << 32) |
        ((uint64_t)length << 48);

    bool data_in = (request_type & 0x80u) != 0;

    uint32_t transfer_type = 0;

    if (length != 0)
        transfer_type = data_in ? 3u : 2u;

    /*
     * Setup Stage:
     * TRB type = 2, immediate data = 1, Chain = 1.
     * TRT = 3 for IN, 2 for OUT, 0 for no data stage.
     */
    xhci_ep0_push_trb(
        setup,
        8u,
        (2u << 10) |
        (1u << 6) |
        (transfer_type << 16) |
        (1u << 4)
    );

    if (length != 0) {
        /*
         * Data Stage:
         * TRB type = 3, Chain = 1.
         */
        xhci_ep0_push_trb(
            data_phys,
            length,
            (3u << 10) |
            (data_in ? (1u << 16) : 0u) |
            (1u << 4)
        );
    }

    /*
     * Status stage direction is opposite to the data stage.
     * A no-data control request has an IN status stage.
     */
    bool status_in = (length == 0) || !data_in;

    xhci_ep0_push_trb(
        0,
        0,
        (4u << 10) |
        (status_in ? (1u << 16) : 0u) |
        (1u << 5)
    );

    __asm__ volatile ("mfence" ::: "memory");

    /* Doorbell for this device's default control endpoint, DCI 1. */
    mmio_write32(
        xhci_mmio,
        xhci_doorbell + (uint32_t)slot_id * 4u,
        1u
    );

    return xhci_wait_control_transfer(slot_id);
}

static bool xhci_get_descriptor(
    uint8_t slot_id,
    uint8_t descriptor_type,
    uint8_t descriptor_index,
    uint16_t length
)
{
    return xhci_control_transfer(
        slot_id,
        0x80,
        0x06,
        (uint16_t)(((uint16_t)descriptor_type << 8) |
                   descriptor_index),
        0,
        length,
        usb_descriptor_buffer_phys
    );
}

static void xhci_mouse_push_trb(
    uint64_t parameter,
    uint32_t status,
    uint32_t control
)
{
    struct xhci_trb *trb =
        &usb_mouse_ring[usb_mouse_enqueue];

    trb->parameter = parameter;
    trb->status = status;
    trb->control = control | usb_mouse_cycle;

    __asm__ volatile ("mfence" ::: "memory");

    usb_mouse_enqueue++;

    if (usb_mouse_enqueue >= XHCI_TRBS_PER_RING - 1) {
        usb_mouse_enqueue = 0;

        usb_mouse_ring[XHCI_TRBS_PER_RING - 1].control =
            (XHCI_TRB_TYPE_LINK << 10) |
            (1u << 1) |
            usb_mouse_cycle;

        __asm__ volatile ("mfence" ::: "memory");

        usb_mouse_cycle ^= 1;
    }
}

static bool xhci_mouse_arm_transfer(void)
{
    if (usb_mouse_transfer_pending)
        return true;

    /*
     * Normal TRB, interrupt-IN endpoint, IOC enabled.
     */
    xhci_mouse_push_trb(
        usb_mouse_buffer_phys,
        usb_mouse_packet_size,
        (1u << 10) |
        (1u << 5)
    );

    __asm__ volatile ("mfence" ::: "memory");

    mmio_write32(
        xhci_mmio,
        xhci_doorbell + (uint32_t)usb_mouse_slot * 4u,
        usb_mouse_dci
    );

    usb_mouse_transfer_pending = true;
    return true;
}

static bool xhci_configure_mouse_endpoint(
    uint8_t slot_id,
    uint8_t interface_number,
    uint8_t endpoint_address,
    uint16_t max_packet_size,
    uint8_t interval,
    uint8_t speed
)
{
    uint8_t endpoint_number = endpoint_address & 0x0Fu;

    if (endpoint_number == 0 || endpoint_number > 15)
        return false;

    if ((endpoint_address & 0x80u) == 0)
        return false;

    usb_mouse_dci = (uint8_t)(endpoint_number * 2u + 1u);
    usb_mouse_slot = slot_id;
    usb_mouse_interface = interface_number;
    usb_mouse_packet_size = (uint8_t)max_packet_size;

    usb_mouse_ring = dma_alloc(
        XHCI_TRBS_PER_RING * sizeof(struct xhci_trb),
        &usb_mouse_ring_phys
    );

    usb_mouse_buffer = dma_alloc(
        4096,
        &usb_mouse_buffer_phys
    );

    if (!usb_mouse_ring || !usb_mouse_buffer) {
        serial_write("[USB] Interrupt endpoint DMA allocation failed\n");
        return false;
    }

    struct xhci_trb *link =
        &usb_mouse_ring[XHCI_TRBS_PER_RING - 1];

    link->parameter = usb_mouse_ring_phys;
    link->status = 0;
    link->control =
        (XHCI_TRB_TYPE_LINK << 10) |
        (1u << 1) |
        1u;

    usb_mouse_enqueue = 0;
    usb_mouse_cycle = 1;

    /*
     * Rebuild an Input Context for Configure Endpoint.
     * Context 0 = Input Control Context.
     * Context 1 = Slot Context.
     * Context (DCI + 1) = requested endpoint context.
     */
    uint32_t *input_words = (uint32_t *)input_context;

    for (uint32_t i = 0;
         i < (xhci_context_size * 33u) / sizeof(uint32_t);
         i++) {
        input_words[i] = 0;
    }

    uint32_t *slot_output = (uint32_t *)device_context;

    uint32_t *slot_input =
        (uint32_t *)(input_context + xhci_context_size);

    for (uint32_t i = 0;
         i < xhci_context_size / sizeof(uint32_t);
         i++) {
        slot_input[i] = slot_output[i];
    }

    /*
     * Slot Context Context Entries = highest enabled DCI.
     * Preserve the other slot fields.
     */
    slot_input[0] =
        (slot_input[0] & ~(0x1Fu << 27)) |
        ((uint32_t)usb_mouse_dci << 27);

    /*
     * Add Slot Context and this endpoint context.
     * Add Context flags are in Input Control Context DWORD 1.
     */
    input_words[1] =
        (1u << 0) |
        (1u << usb_mouse_dci);

    uint32_t *ep_context =
        (uint32_t *)(input_context +
                     ((uint32_t)usb_mouse_dci + 1u) *
                     xhci_context_size);

    /*
     * For High-Speed and SuperSpeed interrupt endpoints,
     * xHCI Interval = bInterval - 1.
     * The QEMU mouse in this setup enumerates at High Speed.
     */
    uint32_t xhci_interval = 0;

    if (speed == 3u || speed == 4u) {
        xhci_interval = interval ? (uint32_t)interval - 1u : 0u;
    } else {
        /*
         * Full/Low-Speed interrupt intervals are expressed in ms.
         * Convert to the nearest supported xHCI interval.
         */
        uint32_t ms = interval ? interval : 1u;
        xhci_interval = 3u;

        while (xhci_interval < 10u &&
               (1u << (xhci_interval - 3u)) < ms) {
            xhci_interval++;
        }
    }

    if (xhci_interval > 15u)
        xhci_interval = 15u;

    ep_context[0] = xhci_interval << 16;

    /*
     * CErr = 3, Endpoint Type = 7 (Interrupt IN),
     * Max Packet Size in bits 31:16.
     */
    ep_context[1] =
        (3u << 1) |
        (7u << 3) |
        ((uint32_t)max_packet_size << 16);

    uint64_t *dequeue =
        (uint64_t *)&ep_context[2];

    *dequeue = usb_mouse_ring_phys | 1u;

    /*
     * Average TRB Length and Max ESIT Payload.
     */
    ep_context[4] =
        (uint32_t)max_packet_size |
        ((uint32_t)max_packet_size << 16);

    __asm__ volatile ("mfence" ::: "memory");

    serial_write("[USB] Configuring interrupt endpoint, DCI: ");
    serial_write_dec(usb_mouse_dci);
    serial_write("\n");

    /*
     * Configure Endpoint command, TRB type = 12.
     */
    if (!xhci_submit_command(
            input_context_phys,
            0,
            (12u << 10) | ((uint32_t)slot_id << 24),
            NULL)) {
        serial_write("[USB] Configure Endpoint command failed\n");
        return false;
    }

    return true;
}

static bool xhci_initialize_hid_mouse(uint8_t slot_id)
{
    usb_descriptor_buffer = dma_alloc(
        4096,
        &usb_descriptor_buffer_phys
    );

    if (!usb_descriptor_buffer) {
        serial_write("[USB] Descriptor DMA allocation failed\n");
        return false;
    }

    serial_write("[USB] Reading Device Descriptor\n");

    if (!xhci_get_descriptor(slot_id, 1, 0, 18)) {
        serial_write("[USB] Device Descriptor request failed\n");
        return false;
    }

    if (usb_descriptor_buffer[0] != 18 ||
        usb_descriptor_buffer[1] != 1) {
        serial_write("[USB] Invalid Device Descriptor\n");
        return false;
    }

    uint16_t vendor_id =
        (uint16_t)usb_descriptor_buffer[8] |
        ((uint16_t)usb_descriptor_buffer[9] << 8);

    uint16_t product_id =
        (uint16_t)usb_descriptor_buffer[10] |
        ((uint16_t)usb_descriptor_buffer[11] << 8);

    serial_write("[USB] VID: ");
    serial_write_hex(vendor_id);
    serial_write(" PID: ");
    serial_write_hex(product_id);
    serial_write("\n");

    serial_write("[USB] Reading Configuration Descriptor header\n");

    if (!xhci_get_descriptor(slot_id, 2, 0, 9)) {
        serial_write("[USB] Configuration header request failed\n");
        return false;
    }

    if (usb_descriptor_buffer[0] < 9 ||
        usb_descriptor_buffer[1] != 2) {
        serial_write("[USB] Invalid Configuration Descriptor header\n");
        return false;
    }

    uint16_t total_length =
        (uint16_t)usb_descriptor_buffer[2] |
        ((uint16_t)usb_descriptor_buffer[3] << 8);

    uint8_t configuration_value = usb_descriptor_buffer[5];

    if (total_length < 9 || total_length > 4096) {
        serial_write("[USB] Invalid configuration descriptor length\n");
        return false;
    }

    serial_write("[USB] Reading full Configuration Descriptor\n");

    if (!xhci_get_descriptor(
            slot_id,
            2,
            0,
            total_length)) {
        serial_write("[USB] Full Configuration Descriptor request failed\n");
        return false;
    }

    uint8_t interface_number = 0;
    uint8_t endpoint_address = 0;
    uint8_t endpoint_interval = 0;
    uint16_t endpoint_packet_size = 0;
    bool matching_interface = false;
    bool found_mouse_endpoint = false;

    for (uint32_t offset = 0; offset + 2 <= total_length;) {
        uint8_t descriptor_length = usb_descriptor_buffer[offset];
        uint8_t descriptor_type = usb_descriptor_buffer[offset + 1];

        if (descriptor_length < 2 ||
            offset + descriptor_length > total_length) {
            serial_write("[USB] Malformed configuration descriptor\n");
            return false;
        }

        if (descriptor_type == 4 && descriptor_length >= 9) {
            /*
             * Interface descriptor:
             * Class = 3 (HID), Subclass = 1 (Boot),
             * Protocol = 2 (Mouse).
             */
            matching_interface =
                usb_descriptor_buffer[offset + 5] == 3 &&
                usb_descriptor_buffer[offset + 6] == 1 &&
                usb_descriptor_buffer[offset + 7] == 2;

            if (matching_interface) {
                interface_number = usb_descriptor_buffer[offset + 2];

                serial_write("[USB] HID Boot Mouse interface: ");
                serial_write_dec(interface_number);
                serial_write("\n");
            }
        } else if (descriptor_type == 5 &&
                   descriptor_length >= 7 &&
                   matching_interface) {
            uint8_t address = usb_descriptor_buffer[offset + 2];
            uint8_t attributes = usb_descriptor_buffer[offset + 3];

            uint16_t packet_size =
                (uint16_t)usb_descriptor_buffer[offset + 4] |
                ((uint16_t)usb_descriptor_buffer[offset + 5] << 8);

            packet_size &= 0x07FFu;

            bool is_in = (address & 0x80u) != 0;
            bool is_interrupt = (attributes & 0x03u) == 3u;

            if (is_in && is_interrupt && packet_size >= 3 &&
                packet_size <= 255) {
                endpoint_address = address;
                endpoint_packet_size = packet_size;
                endpoint_interval = usb_descriptor_buffer[offset + 6];
                found_mouse_endpoint = true;
                break;
            }
        }

        offset += descriptor_length;
    }

    if (!found_mouse_endpoint) {
        serial_write("[USB] HID Boot Mouse interrupt-IN endpoint not found\n");
        return false;
    }

    serial_write("[USB] Interrupt endpoint address: ");
    serial_write_hex(endpoint_address);
    serial_write("\n[USB] Interrupt packet size: ");
    serial_write_dec(endpoint_packet_size);
    serial_write("\n[USB] Interrupt interval: ");
    serial_write_dec(endpoint_interval);
    serial_write("\n");

    /*
     * The device context's Slot Context contains the port speed.
     */
    uint32_t *slot_context = (uint32_t *)device_context;
    uint32_t speed = (slot_context[0] >> 20) & 0xFu;

    if (!xhci_configure_mouse_endpoint(
            slot_id,
            interface_number,
            endpoint_address,
            endpoint_packet_size,
            endpoint_interval,
            (uint8_t)speed)) {
        return false;
    }

    /*
     * SET_CONFIGURATION, after successful Configure Endpoint.
     */
    if (!xhci_control_transfer(
            slot_id,
            0x00,
            0x09,
            configuration_value,
            0,
            0,
            0)) {
        serial_write("[USB] SET_CONFIGURATION failed\n");
        return false;
    }

    /*
     * SET_PROTOCOL: Boot Protocol (value 0).
     * bmRequestType = Class, Interface, Host-to-Device.
     */
    if (!xhci_control_transfer(
        slot_id,
        0x21,
        0x0B,
        0,
        usb_mouse_interface,
        0,
        0)) {
        serial_write("[USB] SET_PROTOCOL Boot failed\n");
        return false;
    }

    usb_mouse_ready = true;
    usb_mouse_transfer_pending = false;

    if (!xhci_mouse_arm_transfer()) {
        usb_mouse_ready = false;
        return false;
    }

    serial_write("[USB] HID Boot Mouse initialized\n");
    serial_write("[USB] Interrupt-IN polling started\n");

    return true;
}

bool xhci_mouse_read_packet(
    int *dx,
    int *dy,
    uint8_t *buttons
)
{
    if (!usb_mouse_ready || !dx || !dy || !buttons)
        return false;

    /*
     * This function is polled by the desktop loop.
     * The controller runs the transfer asynchronously.
     */
    for (uint32_t i = 0; i < 1000u; i++) {
        struct xhci_trb event;

        if (!xhci_pop_event(&event))
            return false;

        uint32_t type = (event.control >> 10) & 0x3Fu;

        if (type != 32u)
            continue;

        uint8_t event_slot =
            (uint8_t)(event.control >> 24);

        uint8_t endpoint_id =
            (uint8_t)((event.control >> 16) & 0x1Fu);

        if (event_slot != usb_mouse_slot ||
            endpoint_id != usb_mouse_dci) {
            continue;
        }

        usb_mouse_transfer_pending = false;

        uint32_t code = event.status >> 24;
        uint32_t residual = event.status & 0x00FFFFFFu;

        bool valid = code == 1u || code == 13u;

        if (valid && residual <= usb_mouse_packet_size) {
            uint32_t transferred =
                usb_mouse_packet_size - residual;

            if (transferred >= 3) {
                *buttons = usb_mouse_buffer[0] & 0x07u;
                *dx = (int)(int8_t)usb_mouse_buffer[1];
                *dy = (int)(int8_t)usb_mouse_buffer[2];

                xhci_mouse_arm_transfer();
                return true;
            }
        }

        if (!valid) {
            serial_write("[USB] Mouse transfer error, code: ");
            serial_write_dec(code);
            serial_write("\n");
        }

        xhci_mouse_arm_transfer();
        return false;
    }

    return false;
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

    if (!xhci_prepare_and_address_device(connected_port, slot_id)) {
    serial_write("[xHCI] USB device addressing failed\n");
    return false;
}

if (!xhci_initialize_hid_mouse(slot_id)) {
    serial_write("[USB] HID mouse initialization failed\n");
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