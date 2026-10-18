#ifndef MYOS_GRAPHICS_H
#define MYOS_GRAPHICS_H

#include <stdbool.h>
#include <stdint.h>
#include <boot/limine.h>

bool framebuffer_init(struct limine_framebuffer *fb);
uint32_t framebuffer_width(void);
uint32_t framebuffer_height(void);
uint32_t framebuffer_rgb(uint8_t r, uint8_t g, uint8_t b);
void framebuffer_clear(uint32_t color);
void framebuffer_draw_rect(int x, int y, int width, int height, uint32_t color);
void framebuffer_draw_text(int x, int y, const char *text, uint32_t color, int scale);
uint32_t framebuffer_get_pixel(int x, int y);
void framebuffer_set_pixel(int x, int y, uint32_t color);
void framebuffer_set_clip(int x, int y, int width, int height);
void framebuffer_reset_clip(void);

#endif