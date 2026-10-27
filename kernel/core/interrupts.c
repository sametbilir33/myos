
#include <arch/x86_64/interrupts.h>
#include <stdint.h>
#include <stdbool.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define PIT_COMMAND  0x43
#define PIT_CHANNEL0 0x40
#define PIT_HZ       100u

#define IRQ0_VECTOR  32
#define PIC_EOI      0x20

struct interrupt_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
} __attribute__((packed));

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256] __attribute__((aligned(16)));
static volatile uint64_t timer_ticks;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1"
                      :
                      : "a"(value), "Nd"(port));
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

static void idt_set_gate(uint8_t vector, uintptr_t address,
                         uint16_t selector)
{
    struct idt_entry *entry = &idt[vector];

    entry->offset_low = (uint16_t)(address & 0xFFFF);
    entry->selector = selector;
    entry->ist = 0;
    entry->attributes = 0x8E;
    entry->offset_middle = (uint16_t)((address >> 16) & 0xFFFF);
    entry->offset_high = (uint32_t)(address >> 32);
    entry->reserved = 0;
}

__attribute__((interrupt))
static void timer_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    timer_ticks++;
    outb(PIC1_COMMAND, PIC_EOI);
}

static void pic_remap(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

static void pit_init(void)
{
    uint16_t divisor = (uint16_t)(1193182u / PIT_HZ);

    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
}

void interrupts_init(void)
{
    __asm__ volatile ("cli" ::: "memory");

    uint16_t code_selector;
    __asm__ volatile ("mov %%cs, %0"
                      : "=r"(code_selector));

    for (unsigned int i = 0; i < 256; i++) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].ist = 0;
        idt[i].attributes = 0;
        idt[i].offset_middle = 0;
        idt[i].offset_high = 0;
        idt[i].reserved = 0;
    }

    idt_set_gate(IRQ0_VECTOR,
                 (uintptr_t)timer_irq_handler,
                 code_selector);

    struct idtr descriptor = {
        .limit = (uint16_t)(sizeof(idt) - 1),
        .base = (uint64_t)(uintptr_t)&idt[0]
    };

    __asm__ volatile ("lidt %0"
                      :
                      : "m"(descriptor)
                      : "memory");

    pic_remap();
    pit_init();
}

void interrupts_enable(void)
{
    /* Yalnızca IRQ0, yani PIT zamanlayıcısı etkin. */
    outb(PIC1_DATA, 0xFE);
    outb(PIC2_DATA, 0xFF);

    __asm__ volatile ("sti" ::: "memory");
}

uint64_t interrupts_ticks(void)
{
    return timer_ticks;
}
