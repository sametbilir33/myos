
#ifndef MYOS_INTERRUPTS_H
#define MYOS_INTERRUPTS_H

#include <stdbool.h>
#include <stdint.h>

void interrupts_init(void);
void interrupts_enable(bool mouse_enabled);
uint64_t interrupts_ticks(void);
uint64_t interrupts_mouse_irq_count(void);

#endif
