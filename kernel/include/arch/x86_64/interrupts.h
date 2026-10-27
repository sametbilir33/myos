
#ifndef MYOS_INTERRUPTS_H
#define MYOS_INTERRUPTS_H

#include <stdint.h>

void interrupts_init(void);
void interrupts_enable(void);
uint64_t interrupts_ticks(void);

#endif
