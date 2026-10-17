
#ifndef MYOS_PS2_MOUSE_H
#define MYOS_PS2_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

bool ps2_mouse_init(void);

/* IRQ12 işleyicisi her fare baytında bunu çağırır. */
void ps2_mouse_irq_byte(uint8_t data);

/* Donanımı poll etmez, kuyruktan hazır paketi alır. */
bool ps2_mouse_read_packet(int *dx, int *dy, uint8_t *buttons);

#endif
