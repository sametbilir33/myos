
#include <stdint.h>
#include <graphics.h>
#include <gui/cursor.h>

#define CURSOR_W 16
#define CURSOR_H 20

static uint32_t cursor_underlay[CURSOR_W * CURSOR_H];

static const uint16_t cursor_bitmap[CURSOR_H] = {
    0x8000, 0xC000, 0xE000, 0xF000, 0xF800,
    0xFC00, 0xFE00, 0xFF00, 0xFF80, 0xFFC0,
    0xFFE0, 0xFFF0, 0xFFF8, 0xF7C0, 0xE3E0,
    0xC1F0, 0x80F8, 0x007C, 0x0038, 0x0010
};

static bool cursor_bit(int x, int y)
{
    if (x < 0 || x >= CURSOR_W || y < 0 || y >= CURSOR_H)
        return false;

    return (cursor_bitmap[y] & (0x8000u >> x)) != 0;
}

void cursor_restore_at(int x, int y)
{
    for (int py = 0; py < CURSOR_H; py++) {
        for (int px = 0; px < CURSOR_W; px++) {
            framebuffer_set_pixel(
                x + px,
                y + py,
                cursor_underlay[py * CURSOR_W + px]
            );
        }
    }
}

void cursor_draw_at(int x, int y)
{
    for (int py = 0; py < CURSOR_H; py++) {
        for (int px = 0; px < CURSOR_W; px++) {
            cursor_underlay[py * CURSOR_W + px] =
                framebuffer_get_pixel(x + px, y + py);
        }
    }

    uint32_t outline = framebuffer_rgb(5, 10, 18);
    uint32_t fill = framebuffer_rgb(250, 252, 255);

    for (int py = 0; py < CURSOR_H; py++) {
        for (int px = 0; px < CURSOR_W; px++) {
            if (!cursor_bit(px, py))
                continue;

            bool edge =
                !cursor_bit(px - 1, py) ||
                !cursor_bit(px + 1, py) ||
                !cursor_bit(px, py - 1) ||
                !cursor_bit(px, py + 1);

            framebuffer_set_pixel(
                x + px,
                y + py,
                edge ? outline : fill
            );
        }
    }
}
