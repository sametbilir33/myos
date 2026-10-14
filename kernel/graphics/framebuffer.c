#include <graphics.h>
#include <font8x16.h>

static struct limine_framebuffer *framebuffer;
static uint32_t screen_width;
static uint32_t screen_height;
static bool clip_enabled;
static int clip_x1, clip_y1, clip_x2, clip_y2;

bool framebuffer_init(struct limine_framebuffer *fb)
{
    if (!fb || !fb->address || fb->bpp != 32)
        return false;

    framebuffer = fb;
    screen_width = (uint32_t)fb->width;
    screen_height = (uint32_t)fb->height;
    clip_enabled = false;

    return screen_width > 0 && screen_height > 0;
}

uint32_t framebuffer_width(void)
{
    return screen_width;
}

uint32_t framebuffer_height(void)
{
    return screen_height;
}

uint32_t framebuffer_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t red = ((uint32_t)r >> (8 - framebuffer->red_mask_size)) << framebuffer->red_mask_shift;
    uint32_t green = ((uint32_t)g >> (8 - framebuffer->green_mask_size)) << framebuffer->green_mask_shift;
    uint32_t blue = ((uint32_t)b >> (8 - framebuffer->blue_mask_size)) << framebuffer->blue_mask_shift;
    return red | green | blue;
}

void framebuffer_set_clip(int x, int y, int width, int height)
{
    int x2 = x + width;
    int y2 = y + height;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > (int)screen_width) x2 = (int)screen_width;
    if (y2 > (int)screen_height) y2 = (int)screen_height;

    clip_x1 = x;
    clip_y1 = y;
    clip_x2 = x2;
    clip_y2 = y2;
    clip_enabled = true;
}

void framebuffer_reset_clip(void)
{
    clip_enabled = false;
}

void framebuffer_set_pixel(int x, int y, uint32_t color)
{
    if (!framebuffer || x < 0 || y < 0 || (uint32_t)x >= screen_width || (uint32_t)y >= screen_height)
        return;

    if (clip_enabled && (x < clip_x1 || x >= clip_x2 || y < clip_y1 || y >= clip_y2))
        return;

    volatile uint32_t *pixel = (volatile uint32_t *)((uint8_t *)framebuffer->address + (uint64_t)y * framebuffer->pitch + (uint64_t)x * 4);
    *pixel = color;
}

uint32_t framebuffer_get_pixel(int x, int y)
{
    if (!framebuffer || x < 0 || y < 0 || (uint32_t)x >= screen_width || (uint32_t)y >= screen_height)
        return 0;

    volatile uint32_t *pixel = (volatile uint32_t *)((uint8_t *)framebuffer->address + (uint64_t)y * framebuffer->pitch + (uint64_t)x * 4);
    return *pixel;
}

void framebuffer_draw_rect(int x, int y, int width, int height, uint32_t color)
{
    if (!framebuffer || width <= 0 || height <= 0)
        return;

    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + width;
    int y2 = y + height;

    if (x2 > (int)screen_width) x2 = (int)screen_width;
    if (y2 > (int)screen_height) y2 = (int)screen_height;

    if (clip_enabled) {
        if (x1 < clip_x1) x1 = clip_x1;
        if (y1 < clip_y1) y1 = clip_y1;
        if (x2 > clip_x2) x2 = clip_x2;
        if (y2 > clip_y2) y2 = clip_y2;
    }

    if (x1 >= x2 || y1 >= y2)
        return;

    for (int py = y1; py < y2; py++) {
        volatile uint32_t *row = (volatile uint32_t *)((uint8_t *)framebuffer->address + (uint64_t)py * framebuffer->pitch);
        for (int px = x1; px < x2; px++)
            row[px] = color;
    }
}

static uint32_t utf8_next(const char **text)
{
    const uint8_t *s = (const uint8_t *)*text;

    if (s[0] < 0x80) {
        *text += 1;
        return s[0];
    }

    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *text += 2;
        return cp;
    }

    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        *text += 3;
        return cp;
    }

    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        *text += 4;
        return cp;
    }

    *text += 1;
    return '?';
}

void framebuffer_draw_text(int x, int y, const char *text, uint32_t color, int scale)
{
    if (!text || scale < 1)
        return;

    int start_x = x;

    while (*text) {
        uint32_t cp = utf8_next(&text);
        const uint8_t *rows = font8x16_get(cp);

        if (!rows)
            rows = font8x16_get('?');

        if (rows) {
            for (int row = 0; row < 16; row++) {
                uint8_t bits = rows[row];

                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80u >> col))
                        framebuffer_draw_rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }

        x += 8 * scale;

        if (x - start_x > (int)screen_width)
            break;
    }
}

void framebuffer_clear(uint32_t color)
{
    framebuffer_draw_rect(0, 0, (int)screen_width, (int)screen_height, color);
}