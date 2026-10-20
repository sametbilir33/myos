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

#define XHCI_PORTSC_CCS       (1u << 0)
#define XHCI_PORTSC_PED       (1u << 1)
#define XHCI_PORTSC_PR        (1u << 4)
#define XHCI_PORTSC_PP        (1u << 9)
#define XHCI_PORTSC_SPEED     (0xFu << 10)

#define XHCI_PORTSC_CSC       (1u << 17)
#define XHCI_PORTSC_PEC       (1u << 18)
#define XHCI_PORTSC_WRC       (1u << 19)
#define XHCI_PORTSC_OCC       (1u << 20)
#define XHCI_PORTSC_PRC       (1u << 21)
#define XHCI_PORTSC_PLC       (1u << 22)
#define XHCI_PORTSC_CEC       (1u << 23)

#define XHCI_PORTSC_CHANGE_MASK (0x7Fu << 17)
#define XHCI_PORTSC_WPR       (1u << 31)
#define XHCI_PORTSC_LWS       (1u << 16)

struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};

#define XHCI_EVENT_QUEUE_SIZE 256u

enum xhci_event_queue_id {
    XHCI_QUEUE_COMMAND,
    XHCI_QUEUE_CONTROL,
    XHCI_QUEUE_INTERRUPT,
    XHCI_QUEUE_COUNT
};

struct xhci_event_queue {
    struct xhci_trb entries[XHCI_EVENT_QUEUE_SIZE];
    uint16_t read;
    uint16_t write;
    uint16_t count;
};

#define XHCI_MAX_DEVICE_SLOTS 256u

struct xhci_device_state {
    bool addressed;

    uint8_t speed;
    uint8_t port_id;

    uint8_t *input_context;
    uint64_t input_context_phys;

    uint8_t *device_context;
    uint64_t device_context_phys;

    struct xhci_trb *ep0_ring;
    uint64_t ep0_ring_phys;
    uint16_t ep0_enqueue;
    uint8_t ep0_cycle;

    struct xhci_trb *interrupt_ring;
    uint64_t interrupt_ring_phys;
    uint16_t interrupt_enqueue;
    uint8_t interrupt_cycle;

    uint8_t *interrupt_buffer;
    uint64_t interrupt_buffer_phys;
    uint16_t interrupt_packet_size;
    uint8_t interrupt_dci;

    bool interrupt_configured;
    bool interrupt_ready;
    bool interrupt_transfer_pending;

    struct xhci_trb interrupt_event;
    bool interrupt_event_ready;
};

static struct xhci_device_state xhci_devices[XHCI_MAX_DEVICE_SLOTS];

static uint8_t xhci_active_slot;

#define input_context              (xhci_devices[xhci_active_slot].input_context)
#define input_context_phys         (xhci_devices[xhci_active_slot].input_context_phys)
#define device_context             (xhci_devices[xhci_active_slot].device_context)
#define device_context_phys        (xhci_devices[xhci_active_slot].device_context_phys)
#define ep0_ring                   (xhci_devices[xhci_active_slot].ep0_ring)
#define ep0_ring_phys              (xhci_devices[xhci_active_slot].ep0_ring_phys)
#define ep0_enqueue                (xhci_devices[xhci_active_slot].ep0_enqueue)
#define ep0_cycle                  (xhci_devices[xhci_active_slot].ep0_cycle)
#define interrupt_in_buffer        (xhci_devices[xhci_active_slot].interrupt_buffer)
#define interrupt_in_buffer_phys   (xhci_devices[xhci_active_slot].interrupt_buffer_phys)
#define interrupt_in_ring          (xhci_devices[xhci_active_slot].interrupt_ring)
#define interrupt_in_ring_phys     (xhci_devices[xhci_active_slot].interrupt_ring_phys)
#define interrupt_in_enqueue       (xhci_devices[xhci_active_slot].interrupt_enqueue)
#define interrupt_in_cycle         (xhci_devices[xhci_active_slot].interrupt_cycle)
#define interrupt_in_packet_size   (xhci_devices[xhci_active_slot].interrupt_packet_size)
#define interrupt_in_dci           (xhci_devices[xhci_active_slot].interrupt_dci)
#define interrupt_in_configured    (xhci_devices[xhci_active_slot].interrupt_configured)
#define interrupt_in_ready         (xhci_devices[xhci_active_slot].interrupt_ready)
#define interrupt_in_transfer_pending (xhci_devices[xhci_active_slot].interrupt_transfer_pending)

/* Port numarasına göre Supported Protocol major version. */
static uint8_t xhci_port_major[256];

static struct xhci_event_queue xhci_event_queues[XHCI_QUEUE_COUNT];

static bool xhci_pop_event(struct xhci_trb *out);

static bool xhci_event_queue_push(
    enum xhci_event_queue_id id,
    const struct xhci_trb *event
)
{
    struct xhci_event_queue *queue = &xhci_event_queues[id];

    if (queue->count == XHCI_EVENT_QUEUE_SIZE)
        return false;

    queue->entries[queue->write] = *event;
    queue->write =
        (uint16_t)((queue->write + 1u) % XHCI_EVENT_QUEUE_SIZE);
    queue->count++;

    return true;
}

static bool xhci_event_queue_pop(
    enum xhci_event_queue_id id,
    struct xhci_trb *out
)
{
    struct xhci_event_queue *queue = &xhci_event_queues[id];

    if (queue->count == 0)
        return false;

    *out = queue->entries[queue->read];
    queue->read =
        (uint16_t)((queue->read + 1u) % XHCI_EVENT_QUEUE_SIZE);
    queue->count--;

    return true;
}

static void xhci_dispatch_events(void)
{
    struct xhci_trb event;

    while (xhci_pop_event(&event)) {
        uint32_t type = (event.control >> 10) & 0x3Fu;

        if (type == 33u) {
            if (!xhci_event_queue_push(XHCI_QUEUE_COMMAND, &event))
                serial_write("[xHCI] Command event queue full\n");
            continue;
        }

        if (type != 32u)
            continue;

        uint8_t slot = (uint8_t)(event.control >> 24);
        uint8_t endpoint = (uint8_t)((event.control >> 16) & 0x1Fu);

        if (slot == 0 || slot >= XHCI_MAX_DEVICE_SLOTS)
            continue;

        if (endpoint == 1u) {
            if (!xhci_event_queue_push(XHCI_QUEUE_CONTROL, &event))
                serial_write("[xHCI] Control event queue full\n");
            continue;
        }

        struct xhci_device_state *dev = &xhci_devices[slot];

        if (dev->interrupt_configured &&
            endpoint == dev->interrupt_dci) {
            dev->interrupt_event = event;
            dev->interrupt_event_ready = true;
        }
    }
}

static bool xhci_wait_queued_event(
    enum xhci_event_queue_id id,
    struct xhci_trb *out,
    uint32_t spin_limit
)
{
    for (uint32_t i = 0; i < spin_limit; i++) {
        xhci_dispatch_events();

        if (xhci_event_queue_pop(id, out))
            return true;
    }

    return false;
}

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

static uint32_t xhci_context_size;

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

static void xhci_read_supported_protocols(
    uint32_t hccparams1,
    uint32_t max_ports
) {
    for (uint32_t i = 0; i < 256; i++)
        xhci_port_major[i] = 0;

    /* HCCPARAMS1.xECP, DWORD cinsinden extended capability konumu. */
    uint32_t offset = ((hccparams1 >> 16) & 0xFFFFu) * 4u;

    if (offset == 0)
        return;

    /* Hatalı capability zincirinde sonsuz döngü oluşmasını önle. */
    for (uint32_t n = 0; n < 256; n++) {
        if (offset < 0x20 || offset >= 0x10000u)
            break;

        uint32_t header = mmio_read32(xhci_mmio, offset);

        if (header == 0 || header == 0xFFFFFFFFu)
            break;

        uint8_t cap_id = (uint8_t)(header & 0xFFu);
        uint8_t next = (uint8_t)((header >> 8) & 0xFFu);

        /*
         * Supported Protocol capability, ID = 2.
         * Header bits 31:24 = major version.
         * Capability + 8: Compatible Port Offset / Count.
         */
        if (cap_id == 2u) {
            uint8_t major = (uint8_t)(header >> 24);
            uint32_t port_info =
                mmio_read32(xhci_mmio, offset + 8u);

            uint32_t first = port_info & 0xFFu;
            uint32_t count = (port_info >> 8) & 0xFFu;

            if (first != 0 && count != 0 &&
                first <= max_ports &&
                count <= max_ports - first + 1u) {

                for (uint32_t p = first;
                     p < first + count && p < 256u;
                     p++) {
                    xhci_port_major[p] = major;
                }
            }
        }

        if (next == 0)
            break;

        uint32_t step = (uint32_t)next * 4u;

        if (offset > 0x10000u - step)
            break;

        offset += step;
    }

    serial_write("[xHCI] Port protocol map:\n");

    for (uint32_t p = 1; p <= max_ports; p++) {
        serial_write("  Port ");
        serial_write_dec(p);
        serial_write(": USB ");
        serial_write_dec(xhci_port_major[p]);
        serial_write("\n");
    }
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

    for (uint32_t i = 0; i < XHCI_QUEUE_COUNT; i++) {
        xhci_event_queues[i].read = 0;
        xhci_event_queues[i].write = 0;
        xhci_event_queues[i].count = 0;
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

    struct xhci_trb event;

if (!xhci_wait_queued_event(
        XHCI_QUEUE_COMMAND,
        &event,
        10000000u)) {
    serial_write("[xHCI] Command completion timed out\n");
    return false;
}

uint32_t completion_code = event.status >> 24;

serial_write("[xHCI] Command completion code: ");
serial_write_dec(completion_code);
serial_write("\n");

if (completion_control != NULL)
    *completion_control = event.control;

serial_write("[xHCI] Completed command pointer: ");
serial_write_hex(event.parameter);
serial_write("\n");

return completion_code == 1u;
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

static bool xhci_reset_connected_port(
    uint32_t ports,
    uint32_t start_index,
    uint8_t *port_id
){
    for (uint32_t i = start_index; i < ports; i++) {
        uint32_t offset = xhci_op + 0x400u + i * 0x10u;
        uint32_t portsc = mmio_read32(xhci_mmio, offset);

        if ((portsc & XHCI_PORTSC_CCS) == 0)
            continue;

        uint32_t port_number = i + 1u;

        serial_write("[xHCI] Connected port ");
        serial_write_dec(port_number);
        serial_write(", protocol major: ");
        serial_write_dec(
            port_number < 256u
                ? xhci_port_major[port_number]
                : 0
        );
        serial_write("\n");

        /*
         * PORTSC özel yazma kurallarına sahiptir:
         * - PED ve reset bitlerini yazma değerinde 0 tut.
         * - RW1C change bitlerini 0 tut.
         * - LWS ve ayrılmış bitleri temizle.
         * - PP değerini koru.
         */
        uint32_t value = portsc;

        value &= ~(XHCI_PORTSC_PED |
                   XHCI_PORTSC_PR |
                   XHCI_PORTSC_WPR |
                   XHCI_PORTSC_CHANGE_MASK |
                   XHCI_PORTSC_LWS |
                   (1u << 2) |
                   (3u << 28));

        /*
         * PR, xHCI port reset bitidir.
         * USB 3.x için de hot reset başlatabilir.
         * WPR farklı bir warm-reset işlemidir ve
         * her USB 3.x portunda zorunlu değildir.
         */
        mmio_write32(
            xhci_mmio,
            offset,
            value | XHCI_PORTSC_PR
        );

        if (!wait_register(
                xhci_mmio,
                offset,
                XHCI_PORTSC_PR,
                0)) {
            serial_write("[xHCI] Port reset timed out\n");
            continue;
        }

        portsc = mmio_read32(xhci_mmio, offset);

        serial_write("[xHCI] PORTSC after reset: ");
        serial_write_hex(portsc);
        serial_write("\n");

        if ((portsc & XHCI_PORTSC_CCS) == 0) {
            serial_write("[xHCI] Device disconnected during reset\n");
            continue;
        }

        if ((portsc & XHCI_PORTSC_PED) == 0) {
            serial_write("[xHCI] Port did not become enabled\n");
            continue;
        }

        *port_id = (uint8_t)port_number;

        serial_write("[xHCI] Port reset and enabled\n");
        return true;
    }

    serial_write("[xHCI] No usable connected USB port found\n");
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
    uint8_t slot_id,
    uint8_t *out_slot_id,
    uint8_t *out_speed
) {
    if (out_slot_id == NULL || out_speed == NULL) {
        return false;
    }

xhci_active_slot = slot_id;
    ep0_enqueue = 0;
    ep0_cycle = 1;

    xhci_active_slot = slot_id;
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

uint32_t *slot_context = (uint32_t *)device_context;

    *out_slot_id = slot_id;
    *out_speed = (uint8_t)((slot_context[0] >> 20) & 0x0Fu);

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

        if (!xhci_wait_queued_event(
                XHCI_QUEUE_CONTROL,
                &event,
                1u)) {
            continue;
        }

        uint8_t event_slot =
            (uint8_t)(event.control >> 24);

        uint8_t endpoint_id =
            (uint8_t)((event.control >> 16) & 0x1Fu);

        if (event_slot != slot_id || endpoint_id != 1u)
            continue;

        uint32_t code = event.status >> 24;

        serial_write("[USB] EP0 completion code: ");
        serial_write_dec(code);
        serial_write("\n");

        return code == 1u || code == 13u;
    }

    serial_write("[USB] EP0 transfer timed out\n");
    return false;
}

bool xhci_control_transfer(
    uint8_t slot_id,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint16_t length,
    uint64_t data_phys
)
{
    if (slot_id == 0 || slot_id >= XHCI_MAX_DEVICE_SLOTS)
    return false;

    xhci_active_slot = slot_id;

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

static void xhci_interrupt_in_push_trb(
    uint64_t parameter,
    uint32_t status,
    uint32_t control
)
{
    struct xhci_trb *trb =
        &interrupt_in_ring[interrupt_in_enqueue];

    trb->parameter = parameter;
    trb->status = status;
    trb->control = control | interrupt_in_cycle;

    __asm__ volatile ("mfence" ::: "memory");

    interrupt_in_enqueue++;

    if (interrupt_in_enqueue >= XHCI_TRBS_PER_RING - 1) {
        interrupt_in_enqueue = 0;

        interrupt_in_ring[XHCI_TRBS_PER_RING - 1].control =
            (XHCI_TRB_TYPE_LINK << 10) |
            (1u << 1) |
            interrupt_in_cycle;

        __asm__ volatile ("mfence" ::: "memory");

        interrupt_in_cycle ^= 1;
    }
}

static bool xhci_interrupt_in_arm_transfer(void)
{
    if (interrupt_in_transfer_pending)
        return true;

    xhci_interrupt_in_push_trb(
        interrupt_in_buffer_phys,
        interrupt_in_packet_size,
        (1u << 10) |
        (1u << 5)
    );

    __asm__ volatile ("mfence" ::: "memory");

    mmio_write32(
        xhci_mmio,
        xhci_doorbell + (uint32_t)interrupt_in_slot * 4u,
        interrupt_in_dci
    );

    interrupt_in_transfer_pending = true;
    return true;
}


bool xhci_interrupt_in_configure(
    uint8_t slot_id,
    uint8_t endpoint_address,
    uint16_t max_packet_size,
    uint8_t interval,
    uint8_t speed,
    uint8_t *buffer,
    uint64_t buffer_phys
)
{
if (slot_id == 0 || slot_id >= XHCI_MAX_DEVICE_SLOTS)
    return false;

xhci_active_slot = slot_id;

    uint8_t endpoint_number = endpoint_address & 0x0Fu;

    if (endpoint_number == 0 ||
        endpoint_number > 15 ||
        (endpoint_address & 0x80u) == 0 ||
        max_packet_size < 3 ||
        max_packet_size > 255 ||
        buffer == NULL ||
        buffer_phys == 0) {
        return false;
    }

    interrupt_in_dci = (uint8_t)(endpoint_number * 2u + 1u);
    interrupt_in_slot = slot_id;
    interrupt_in_packet_size = max_packet_size;
    interrupt_in_buffer = buffer;
    interrupt_in_buffer_phys = buffer_phys;

    interrupt_in_ring = dma_alloc(
        XHCI_TRBS_PER_RING * sizeof(struct xhci_trb),
        &interrupt_in_ring_phys
    );

    if (interrupt_in_ring == NULL) {
        serial_write("[xHCI] Interrupt endpoint ring allocation failed\n");
        return false;
    }

    struct xhci_trb *link =
        &interrupt_in_ring[XHCI_TRBS_PER_RING - 1];

    link->parameter = interrupt_in_ring_phys;
    link->status = 0;
    link->control =
        (XHCI_TRB_TYPE_LINK << 10) |
        (1u << 1) |
        1u;

    interrupt_in_enqueue = 0;
    interrupt_in_cycle = 1;

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

    slot_input[0] =
        (slot_input[0] & ~(0x1Fu << 27)) |
        ((uint32_t)interrupt_in_dci << 27);

    input_words[1] =
        (1u << 0) |
        (1u << interrupt_in_dci);

    uint32_t *ep_context =
        (uint32_t *)(input_context +
                     ((uint32_t)interrupt_in_dci + 1u) *
                     xhci_context_size);

    uint32_t xhci_interval;

    if (speed == 3u || speed == 4u) {
        xhci_interval = interval ? (uint32_t)interval - 1u : 0u;
    } else {
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

    ep_context[1] =
        (3u << 1) |
        (7u << 3) |
        ((uint32_t)max_packet_size << 16);

    uint64_t *dequeue = (uint64_t *)&ep_context[2];
    *dequeue = interrupt_in_ring_phys | 1u;

    ep_context[4] =
        (uint32_t)max_packet_size |
        ((uint32_t)max_packet_size << 16);

    __asm__ volatile ("mfence" ::: "memory");

    serial_write("[xHCI] Configuring interrupt-IN endpoint, DCI: ");
    serial_write_dec(interrupt_in_dci);
    serial_write("\n");

    if (!xhci_submit_command(
            input_context_phys,
            0,
            (12u << 10) | ((uint32_t)slot_id << 24),
            NULL)) {
        serial_write("[xHCI] Configure Endpoint command failed\n");
        return false;
    }

    interrupt_in_configured = true;
    return true;
}

bool xhci_interrupt_in_start(uint8_t slot_id)
{
    if (slot_id == 0 || slot_id >= XHCI_MAX_DEVICE_SLOTS)
        return false;

    xhci_active_slot = slot_id;

    if (!interrupt_in_configured)
        return false;

    interrupt_in_transfer_pending = false;
    interrupt_in_ready = true;
    xhci_devices[slot_id].interrupt_event_ready = false;

    if (!xhci_interrupt_in_arm_transfer()) {
        interrupt_in_ready = false;
        return false;
    }

    return true;
}

bool xhci_interrupt_in_read(
    uint8_t slot_id,
    uint8_t *report,
    uint16_t capacity,
    uint16_t *length
)
{
    if (length == NULL)
        return false;

    *length = 0;

    if (slot_id == 0 ||
        slot_id >= XHCI_MAX_DEVICE_SLOTS ||
        report == NULL ||
        capacity == 0) {
        return false;
    }

    xhci_active_slot = slot_id;

    struct xhci_device_state *dev = &xhci_devices[slot_id];

    if (!dev->interrupt_ready)
        return false;

    xhci_dispatch_events();

    if (!dev->interrupt_event_ready)
        return false;

    struct xhci_trb event = dev->interrupt_event;
    dev->interrupt_event_ready = false;
    dev->interrupt_transfer_pending = false;

    uint32_t code = event.status >> 24;
    uint32_t residual = event.status & 0x00FFFFFFu;
    bool valid = (code == 1u || code == 13u);
    bool report_ready = false;

    if (valid && residual <= dev->interrupt_packet_size) {
        uint32_t transferred = dev->interrupt_packet_size - residual;

        if (transferred > 0 && transferred <= capacity) {
            for (uint32_t i = 0; i < transferred; i++)
                report[i] = dev->interrupt_buffer[i];

            *length = (uint16_t)transferred;
            report_ready = true;
        }
    }

    if (!valid) {
        serial_write("[xHCI] Interrupt-IN error, slot=");
        serial_write_dec(slot_id);
        serial_write(" code=");
        serial_write_dec(code);
        serial_write("\n");
    }

    if (!xhci_interrupt_in_arm_transfer())
        dev->interrupt_ready = false;

    return report_ready;
}

bool xhci_probe_all(
    uint64_t bar_phys,
    uint64_t hhdm_offset,
    uint8_t *slot_ids,
    uint8_t *speeds,
    uint8_t *out_count,
    uint8_t capacity
)
{
    if (slot_ids == NULL ||
        speeds == NULL ||
        out_count == NULL ||
        capacity == 0) {
        return false;
    }

    *out_count = 0;

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

    uint32_t hccparams1 = mmio_read32(xhci_mmio, 0x10);

    xhci_read_supported_protocols(
        hccparams1,
        (hcsparams1 >> 24) & 0xFFu
    );

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
    uint32_t next_port_index = 0;

    while (next_port_index < ports && *out_count < capacity) {
        uint8_t connected_port = 0;

        if (!xhci_reset_connected_port(
                ports,
                next_port_index,
                &connected_port)) {
            break;
        }

        /* Sonraki arama, bulunan portun ardından devam eder. */
        next_port_index = connected_port;

        uint8_t slot_id = 0;

        if (!xhci_enable_slot(&slot_id)) {
            serial_write("[xHCI] Could not enable slot\n");
            continue;
        }

        uint8_t addressed_slot_id = 0;
        uint8_t addressed_speed = 0;

        if (!xhci_prepare_and_address_device(
                connected_port,
                slot_id,
                &addressed_slot_id,
                &addressed_speed)) {
            serial_write("[xHCI] Device addressing failed\n");
            continue;
        }

        slot_ids[*out_count] = addressed_slot_id;
        speeds[*out_count] = addressed_speed;
        (*out_count)++;

        serial_write("[xHCI] Device initialized on port ");
        serial_write_dec(connected_port);
        serial_write(", slot ");
        serial_write_dec(addressed_slot_id);
        serial_write("\n");
    }

    serial_write("[xHCI] Addressed devices: ");
    serial_write_dec(*out_count);
    serial_write("\n");

    return *out_count != 0;
}