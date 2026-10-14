#ifndef MYOS_PS2_MOUSE_H
#define MYOS_PS2_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

bool ps2_mouse_init(void);

/* Yeni bir tam fare paketi geldiyse true döndürür. */
bool ps2_mouse_poll(int *dx, int *dy, uint8_t *buttons);

#endif