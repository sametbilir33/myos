#include <stdint.h>
#include <stdbool.h>
#include <limine.h>
#include <graphics.h>
#include <ps2_mouse.h>
#include <serial.h>
#include <gui/cursor.h>
#include <gui/events.h>
#include <interrupts.h>

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

#define RGB(r, g, b) framebuffer_rgb((r), (g), (b))
#define SCREEN_BAR_HEIGHT 38
#define MENU_X 8
#define MENU_Y 42
#define MENU_W 176
#define MENU_H 194
#define CURSOR_W 16
#define CURSOR_H 20
#define WINDOW_W 420
#define WINDOW_H 240
#define MAX_WINDOWS 8
#define MAX_MOUSE_PACKETS_PER_LOOP 32

static const uint32_t COLOR_BACKGROUND = 0x00213B60;
static const uint32_t COLOR_BAR = 0x00132642;
static const uint32_t COLOR_ACCENT = 0x0044CDDC;
static const uint32_t COLOR_TEXT = 0x00F0F6FF;
static const uint32_t COLOR_MUTED = 0x009CB1C9;
static const uint32_t COLOR_PANEL = 0x00202E43;
static const uint32_t COLOR_PANEL_DARK = 0x00172233;
static const uint32_t COLOR_HOVER = 0x00334D6B;
static const uint32_t COLOR_BORDER = 0x00445B78;

struct app_window {
    int app;
    int x;
    int y;
};

static struct app_window windows[MAX_WINDOWS];
static int window_count;
static int next_window_offset;
static bool menu_open;
static int cursor_x = 100;
static int cursor_y = 100;
static char clock_text[9] = "00:00:00";
static int last_clock_second = -1;

static bool dragging;
static int drag_index = -1;
static int drag_start_x, drag_start_y;
static int drag_original_x, drag_original_y;
static int drag_preview_x, drag_preview_y;

/* Şu an aktif olan clip bölgesi (kendi tarafımızda tutuyoruz ki
   bölge dışında kalan çizimleri hiç göndermeyelim). */
static bool clip_active;
static int clip_x, clip_y, clip_w, clip_h;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd_to_binary(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static uint8_t rtc_get_time(char output[9])
{
    uint8_t second = 0, minute = 0, hour = 0, mode = 0;

    for (int attempt = 0; attempt < 1000; attempt++) {
        if (cmos_read(0x0A) & 0x80)
            continue;

        second = cmos_read(0x00);
        minute = cmos_read(0x02);
        hour = cmos_read(0x04);
        mode = cmos_read(0x0B);

        if (!(cmos_read(0x0A) & 0x80))
            break;
    }

    bool binary_mode = (mode & 0x04) != 0;
    bool hour24_mode = (mode & 0x02) != 0;
    bool pm = (hour & 0x80) != 0;
    hour &= 0x7F;

    if (!binary_mode) {
        second = bcd_to_binary(second);
        minute = bcd_to_binary(minute);
        hour = bcd_to_binary(hour);
    }

    if (!hour24_mode) {
        hour %= 12;
        if (pm) hour += 12;
    }

    hour %= 24;
    minute %= 60;
    second %= 60;

    output[0] = '0' + hour / 10;
    output[1] = '0' + hour % 10;
    output[2] = ':';
    output[3] = '0' + minute / 10;
    output[4] = '0' + minute % 10;
    output[5] = ':';
    output[6] = '0' + second / 10;
    output[7] = '0' + second % 10;
    output[8] = '\0';

    return second;
}

static bool inside(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ------------------------------------------------------------------ */
/*  Clip'e duyarlı çizim sarmalayıcıları                               */
/* ------------------------------------------------------------------ */

static void fill_rect(int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0)
        return;

    if (clip_active) {
        int x2 = x + w;
        int y2 = y + h;
        int cx2 = clip_x + clip_w;
        int cy2 = clip_y + clip_h;

        if (x < clip_x) x = clip_x;
        if (y < clip_y) y = clip_y;
        if (x2 > cx2) x2 = cx2;
        if (y2 > cy2) y2 = cy2;

        if (x2 <= x || y2 <= y)
            return; /* tamamen bölge dışında, hiç çizme */

        w = x2 - x;
        h = y2 - y;
    }

    framebuffer_draw_rect(x, y, w, h, color);
}

static int text_length(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void draw_text(int x, int y, const char *text, uint32_t color, int scale)
{
    if (clip_active) {
        /* Muhafazakar sınırlayıcı kutu: font boyutu tam bilinmediği için
           biraz bol tutuyoruz. Sadece tamamen dışarıda kalanı atlıyoruz. */
        int w = text_length(text) * 10 * scale;
        int h = 16 * scale;

        if (x + w <= clip_x || x >= clip_x + clip_w ||
            y + h <= clip_y || y >= clip_y + clip_h)
            return;
    }

    framebuffer_draw_text(x, y, text, color, scale);
}

static const char *app_title(int app)
{
    switch (app) {
    case 1: return "About MyOS";
    case 2: return "Notepad";
    case 3: return "File Manager";
    case 4: return "Terminal";
    default: return "Application";
    }
}

static void draw_status_bar(void)
{
    fill_rect(0, 0, (int)framebuffer_width(), SCREEN_BAR_HEIGHT, COLOR_BAR);
    fill_rect(0, SCREEN_BAR_HEIGHT - 1, (int)framebuffer_width(), 1, COLOR_ACCENT);
    fill_rect(8, 5, 92, 27, menu_open ? COLOR_HOVER : COLOR_PANEL);
    fill_rect(8, 5, 3, 27, COLOR_ACCENT);
    draw_text(19, 11, "MyOS", COLOR_TEXT, 1);
    draw_text(57, 11, "[v]", COLOR_MUTED, 1);

    int clock_x = (int)framebuffer_width() - 80;
    fill_rect(clock_x, 0, 80, SCREEN_BAR_HEIGHT, COLOR_BAR);
    draw_text(clock_x + 8, 12, clock_text, COLOR_TEXT, 1);
}

static void draw_desktop_background(void)
{
    fill_rect(0, 0, (int)framebuffer_width(), (int)framebuffer_height(), COLOR_BACKGROUND);
    fill_rect(40, 100, 180, 2, RGB(53, 100, 148));
    draw_text(40, 112, "Welcome to MyOS", COLOR_TEXT, 2);
    draw_text(40, 150, "A small desktop operating system", COLOR_MUTED, 1);
}

static void draw_menu_item(int y, const char *label)
{
    draw_text(MENU_X + 14, y + 5, label, COLOR_TEXT, 1);
}

static void draw_menu(void)
{
    fill_rect(MENU_X, MENU_Y, MENU_W, MENU_H, COLOR_PANEL);
    fill_rect(MENU_X, MENU_Y, MENU_W, 1, COLOR_BORDER);
    fill_rect(MENU_X, MENU_Y + MENU_H - 1, MENU_W, 1, COLOR_BORDER);
    fill_rect(MENU_X, MENU_Y, 1, MENU_H, COLOR_BORDER);
    fill_rect(MENU_X + MENU_W - 1, MENU_Y, 1, MENU_H, COLOR_BORDER);

    draw_menu_item(47, "About");
    draw_menu_item(70, "Notepad");
    draw_menu_item(93, "File Manager");
    draw_menu_item(116, "Terminal");

    fill_rect(MENU_X + 10, 141, MENU_W - 20, 1, COLOR_BORDER);
    draw_menu_item(148, "Shutdown");
    draw_menu_item(171, "Reboot");
}

static void draw_window(const struct app_window *win)
{
    int x = win->x, y = win->y, w = WINDOW_W, h = WINDOW_H;

    /* Pencere bölgeyle hiç kesişmiyorsa tüm çizimi atla. */
    if (clip_active) {
        if (x + w <= clip_x || x >= clip_x + clip_w ||
            y + h <= clip_y || y >= clip_y + clip_h)
            return;
    }

    fill_rect(x, y, w, h, COLOR_PANEL_DARK);
    fill_rect(x, y, w, 30, COLOR_BAR);
    fill_rect(x, y, w, 2, COLOR_ACCENT);
    fill_rect(x, y + h - 1, w, 1, COLOR_BORDER);
    fill_rect(x, y, 1, h, COLOR_BORDER);
    fill_rect(x + w - 1, y, 1, h, COLOR_BORDER);
    draw_text(x + 12, y + 9, app_title(win->app), COLOR_TEXT, 1);
    fill_rect(x + w - 28, y + 4, 22, 22, RGB(130, 45, 55));
    draw_text(x + w - 21, y + 11, "X", COLOR_TEXT, 1);

    switch (win->app) {
    case 1:
        draw_text(x + 18, y + 55, "MyOS", COLOR_ACCENT, 2);
        draw_text(x + 18, y + 92, "64-bit experimental desktop", COLOR_TEXT, 1);
        draw_text(x + 18, y + 116, "Built from scratch in C.", COLOR_MUTED, 1);
        draw_text(x + 18, y + 174, "Version 0.1", COLOR_MUTED, 1);
        break;
    case 2:
        draw_text(x + 18, y + 48, "Notepad is not implemented yet.", COLOR_MUTED, 1);
        draw_text(x + 18, y + 72, "Text editing will come later.", COLOR_MUTED, 1);
        break;
    case 3:
        draw_text(x + 18, y + 48, "File Manager", COLOR_TEXT, 1);
        draw_text(x + 18, y + 72, "Filesystem support is not ready.", COLOR_MUTED, 1);
        break;
    case 4:
        fill_rect(x + 12, y + 43, w - 24, h - 55, RGB(5, 12, 20));
        draw_text(x + 22, y + 54, "MyOS terminal", COLOR_ACCENT, 1);
        draw_text(x + 22, y + 78, "Shell not implemented.", COLOR_TEXT, 1);
        draw_text(x + 22, y + 102, "> _", COLOR_TEXT, 1);
        break;
    }
}

/* Yalnızca belirtilen dikdörtgeni yeniden oluştur. */
static void redraw_region(int x, int y, int width, int height)
{
    /* Ekranla kesiştir. */
    int sw = (int)framebuffer_width();
    int sh = (int)framebuffer_height();
    int x2 = x + width;
    int y2 = y + height;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > sw) x2 = sw;
    if (y2 > sh) y2 = sh;

    if (x2 <= x || y2 <= y)
        return;

    width = x2 - x;
    height = y2 - y;

    clip_active = true;
    clip_x = x;
    clip_y = y;
    clip_w = width;
    clip_h = height;

    framebuffer_reset_clip();
    framebuffer_set_clip(x, y, width, height);

    draw_desktop_background();
    draw_status_bar();

    for (int i = 0; i < window_count; i++)
        draw_window(&windows[i]);

    if (menu_open)
        draw_menu();

    framebuffer_reset_clip();
    clip_active = false;
}

static void draw_drag_outline(int x, int y)
{
    uint32_t color = RGB(240, 248, 255);
    fill_rect(x, y, WINDOW_W, 2, color);
    fill_rect(x, y + WINDOW_H - 2, WINDOW_W, 2, color);
    fill_rect(x, y, 2, WINDOW_H, color);
    fill_rect(x + WINDOW_W - 2, y, 2, WINDOW_H, color);
}

static void bring_to_front(int index)
{
    if (index < 0 || index >= window_count || index == window_count - 1)
        return;

    struct app_window win = windows[index];

    for (int i = index; i < window_count - 1; i++)
        windows[i] = windows[i + 1];

    windows[window_count - 1] = win;
}

static void remove_window(int index)
{
    if (index < 0 || index >= window_count)
        return;

    for (int i = index; i < window_count - 1; i++)
        windows[i] = windows[i + 1];

    window_count--;
}

static void open_app(int app)
{
    if (window_count >= MAX_WINDOWS) {
        serial_write("[GUI] Window limit reached\n");
        return;
    }

    int x = 80 + (next_window_offset % 6) * 28;
    int y = 64 + (next_window_offset % 6) * 24;
    next_window_offset++;

    int max_x = (int)framebuffer_width() - WINDOW_W;
    int max_y = (int)framebuffer_height() - WINDOW_H;

    if (max_x < 0) max_x = 0;
    if (max_y < SCREEN_BAR_HEIGHT + 2) max_y = SCREEN_BAR_HEIGHT + 2;
    if (x > max_x) x = max_x;
    if (y > max_y) y = max_y;

    windows[window_count++] = (struct app_window){ .app = app, .x = x, .y = y };
    redraw_region(x, y, WINDOW_W, WINDOW_H);
}

static void system_shutdown(void)
{
    serial_write("[POWER] Shutdown requested\n");
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);

    for (;;) __asm__ volatile ("hlt");
}

static void system_reboot(void)
{
    serial_write("[POWER] Reboot requested\n");

    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0)
            break;
    }

    outb(0x64, 0xFE);

    for (;;) __asm__ volatile ("hlt");
}

static int find_window_at(int x, int y)
{
    for (int i = window_count - 1; i >= 0; i--) {
        if (inside(x, y, windows[i].x, windows[i].y, WINDOW_W, WINDOW_H))
            return i;
    }

    return -1;
}

static void handle_click(int x, int y)
{
    if (inside(x, y, 8, 5, 92, 27)) {
        menu_open = !menu_open;
        redraw_region(MENU_X, MENU_Y, MENU_W, MENU_H);
        redraw_region(8, 5, 92, 27);
        return;
    }

    if (menu_open) {
        if (inside(x, y, MENU_X, MENU_Y, MENU_W, MENU_H)) {
            int app = 0;

            if (inside(x, y, MENU_X, 47, MENU_W, 22)) app = 1;
            else if (inside(x, y, MENU_X, 70, MENU_W, 22)) app = 2;
            else if (inside(x, y, MENU_X, 93, MENU_W, 22)) app = 3;
            else if (inside(x, y, MENU_X, 116, MENU_W, 22)) app = 4;
            else if (inside(x, y, MENU_X, 148, MENU_W, 22)) {
                menu_open = false;
                redraw_region(MENU_X, MENU_Y, MENU_W, MENU_H);
                redraw_region(8, 5, 92, 27);
                system_shutdown();
            } else if (inside(x, y, MENU_X, 171, MENU_W, 22)) {
                menu_open = false;
                redraw_region(MENU_X, MENU_Y, MENU_W, MENU_H);
                redraw_region(8, 5, 92, 27);
                system_reboot();
            }

            menu_open = false;
            redraw_region(MENU_X, MENU_Y, MENU_W, MENU_H);
            redraw_region(8, 5, 92, 27);

            if (app)
                open_app(app);

            return;
        }

        menu_open = false;
        redraw_region(MENU_X, MENU_Y, MENU_W, MENU_H);
        redraw_region(8, 5, 92, 27);
    }

    int index = find_window_at(x, y);

    if (index < 0)
        return;

    int old_x = windows[index].x;
    int old_y = windows[index].y;

    if (inside(x, y, old_x + WINDOW_W - 28, old_y + 4, 22, 22)) {
        remove_window(index);
        redraw_region(old_x, old_y, WINDOW_W, WINDOW_H);
        return;
    }

    if (index != window_count - 1) {
        bring_to_front(index);
        redraw_region(old_x, old_y, WINDOW_W, WINDOW_H);
        index = window_count - 1;
    }

    if (inside(x, y, windows[index].x, windows[index].y, WINDOW_W, 30)) {
        dragging = true;
        drag_index = index;
        drag_start_x = x;
        drag_start_y = y;
        drag_original_x = windows[index].x;
        drag_original_y = windows[index].y;
        drag_preview_x = drag_original_x;
        drag_preview_y = drag_original_y;
        draw_drag_outline(drag_preview_x, drag_preview_y);
    }
}

static void update_drag(int mouse_x, int mouse_y)
{
    if (!dragging || drag_index < 0 || drag_index >= window_count)
        return;

    redraw_region(drag_preview_x - 2, drag_preview_y - 2, WINDOW_W + 4, WINDOW_H + 4);

    int new_x = drag_original_x + mouse_x - drag_start_x;
    int new_y = drag_original_y + mouse_y - drag_start_y;
    int max_x = (int)framebuffer_width() - WINDOW_W;
    int max_y = (int)framebuffer_height() - WINDOW_H;

    if (max_x < 0) max_x = 0;
    if (max_y < SCREEN_BAR_HEIGHT + 2) max_y = SCREEN_BAR_HEIGHT + 2;

    if (new_x < 0) new_x = 0;
    if (new_y < SCREEN_BAR_HEIGHT + 2) new_y = SCREEN_BAR_HEIGHT + 2;
    if (new_x > max_x) new_x = max_x;
    if (new_y > max_y) new_y = max_y;

    drag_preview_x = new_x;
    drag_preview_y = new_y;
    draw_drag_outline(drag_preview_x, drag_preview_y);
}

static void finish_drag(void)
{
    if (!dragging || drag_index < 0 || drag_index >= window_count) {
        dragging = false;
        drag_index = -1;
        return;
    }

    int old_x = windows[drag_index].x;
    int old_y = windows[drag_index].y;

    redraw_region(drag_preview_x - 2, drag_preview_y - 2, WINDOW_W + 4, WINDOW_H + 4);

    windows[drag_index].x = drag_preview_x;
    windows[drag_index].y = drag_preview_y;

    dragging = false;
    drag_index = -1;

    redraw_region(old_x, old_y, WINDOW_W, WINDOW_H);
    redraw_region(drag_preview_x, drag_preview_y, WINDOW_W, WINDOW_H);
}

static void handle_gui_event(const struct gui_event *event)
{
    switch (event->type) {
    case GUI_EVENT_MOUSE_MOVE:
        if (dragging)
            update_drag(event->x, event->y);
        break;

    case GUI_EVENT_MOUSE_BUTTON_DOWN:
        if (event->button == 1 && !dragging)
            handle_click(event->x, event->y);
        break;

    case GUI_EVENT_MOUSE_BUTTON_UP:
        if (event->button == 1 && dragging)
            finish_drag();
        break;
    }
}

static void update_clock(void)
{
    char new_time[9];
    uint8_t second = rtc_get_time(new_time);

    if ((int)second == last_clock_second)
        return;

    last_clock_second = (int)second;

    for (int i = 0; i < 9; i++)
        clock_text[i] = new_time[i];

    redraw_region((int)framebuffer_width() - 80, 0, 80, SCREEN_BAR_HEIGHT);
    redraw_region(cursor_x, cursor_y, CURSOR_W, CURSOR_H);
    cursor_draw_at(cursor_x, cursor_y);
}

static void stop_with_error(const char *message)
{
    serial_write("[FATAL] ");
    serial_write(message);
    serial_write("\n");

    for (;;) __asm__ volatile ("hlt");
}

void kernel_main(void)
{
    serial_init();

    __asm__ volatile ("cli" ::: "memory");
    interrupts_init();
    serial_write("[IRQ] IDT, PIC and PIT initialized\n");

    serial_write("\n[BOOT] MyOS desktop starting\n");

    if (!framebuffer_request.response)
        stop_with_error("No Limine framebuffer response");

    if (framebuffer_request.response->framebuffer_count == 0)
        stop_with_error("No framebuffer available");

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

    if (!framebuffer_init(fb))
        stop_with_error("Framebuffer initialization failed");

    rtc_get_time(clock_text);
    clip_active = false;
    draw_desktop_background();
    draw_status_bar();

    cursor_x = (int)framebuffer_width() / 2;
    cursor_y = (int)framebuffer_height() / 2;

    if (cursor_x > (int)framebuffer_width() - CURSOR_W)
        cursor_x = (int)framebuffer_width() - CURSOR_W;

    if (cursor_y > (int)framebuffer_height() - CURSOR_H)
        cursor_y = (int)framebuffer_height() - CURSOR_H;

    cursor_draw_at(cursor_x, cursor_y);
    serial_write("[DEBUG] Initial cursor draw completed\n");

    bool mouse_available = ps2_mouse_init();

    if (mouse_available)
        serial_write("[BOOT] PS/2 mouse ready\n");
    else
        serial_write("[WARN] PS/2 mouse unavailable\n");

    gui_events_init();
    gui_events_set_handler(handle_gui_event);

    interrupts_enable(mouse_available);
    serial_write("[IRQ] Interrupts enabled\n");

    uint8_t previous_buttons = 0;
    uint64_t last_clock_tick = 0;

    for (;;) {
        uint64_t now = interrupts_ticks();

if (now - last_clock_tick >= 100) {
    last_clock_tick = now - (now % 100);
    update_clock();
}

        if (!mouse_available) {
    __asm__ volatile ("hlt");
    continue;
}

        /*
         * Bekleyen fare paketlerini topla. Her paket için ayrı ayrı
         * silip çizmek yerine hareketi biriktirip tek seferde işliyoruz.
         * Buton durumu değişirse biriktirmeyi bırakıyoruz ki tıklama
         * olayları kaybolmasın.
         */
        int dx = 0, dy = 0;
        uint8_t buttons = previous_buttons;
        bool got_packet = false;

        for (int n = 0; n < MAX_MOUSE_PACKETS_PER_LOOP; n++) {
            int pdx = 0, pdy = 0;
            uint8_t pbuttons = buttons;

            if (!ps2_mouse_read_packet(&pdx, &pdy, &pbuttons))
    break;

            got_packet = true;
            dx += pdx;
            dy += pdy;

            bool button_changed = (pbuttons & 1u) != (previous_buttons & 1u);
            buttons = pbuttons;

            if (button_changed)
                break;
        }

        if (!got_packet) {
    __asm__ volatile ("hlt");
    continue;
}

        int max_x = (int)framebuffer_width() - CURSOR_W;
        int max_y = (int)framebuffer_height() - CURSOR_H;

        int new_x = cursor_x + dx;
        int new_y = cursor_y + dy;

        if (new_x < 0) new_x = 0;
        if (new_y < 0) new_y = 0;
        if (new_x > max_x) new_x = max_x;
        if (new_y > max_y) new_y = max_y;

        bool moved = new_x != cursor_x || new_y != cursor_y;
        bool left_down = (buttons & 1u) && !(previous_buttons & 1u);
        bool left_up = !(buttons & 1u) && (previous_buttons & 1u);

        if (moved || left_down || left_up) {
            /* Eski imleci masaüstünü o küçük bölgede yeniden çizerek temizle. */
            redraw_region(cursor_x, cursor_y, CURSOR_W, CURSOR_H);

            cursor_x = new_x;
            cursor_y = new_y;

            if (moved) {
                gui_event_push((struct gui_event) {
                    .type = GUI_EVENT_MOUSE_MOVE,
                    .x = cursor_x,
                    .y = cursor_y,
                    .button = 0
                });
            }

            if (left_down) {
                gui_event_push((struct gui_event) {
                    .type = GUI_EVENT_MOUSE_BUTTON_DOWN,
                    .x = cursor_x,
                    .y = cursor_y,
                    .button = 1
                });
            }

            if (left_up) {
                gui_event_push((struct gui_event) {
                    .type = GUI_EVENT_MOUSE_BUTTON_UP,
                    .x = cursor_x,
                    .y = cursor_y,
                    .button = 1
                });
            }

            gui_events_dispatch();

            cursor_draw_at(cursor_x, cursor_y);
        }

        previous_buttons = buttons;
    }
}