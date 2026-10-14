
#include <stdint.h>
#include <stdbool.h>
#include <limine.h>
#include <graphics.h>
#include <ps2_mouse.h>
#include <serial.h>

/* Limine framebuffer istegi */

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] =
    LIMINE_REQUESTS_END_MARKER;

/* Renkler */

#define RGB(r, g, b) framebuffer_rgb((r), (g), (b))

#define SCREEN_BAR_HEIGHT 38

#define MENU_X       8
#define MENU_Y       42
#define MENU_W       208
#define MENU_H       226

#define CURSOR_W     16
#define CURSOR_H     20

#define WINDOW_X     100
#define WINDOW_Y     90
#define WINDOW_W     420
#define WINDOW_H     240

static const uint32_t COLOR_BACKGROUND = 0x00213B60;
static const uint32_t COLOR_BAR        = 0x00132642;
static const uint32_t COLOR_ACCENT     = 0x0044CDDC;
static const uint32_t COLOR_TEXT       = 0x00F0F6FF;
static const uint32_t COLOR_MUTED      = 0x009CB1C9;
static const uint32_t COLOR_PANEL      = 0x00202E43;
static const uint32_t COLOR_PANEL_DARK = 0x00172233;
static const uint32_t COLOR_HOVER      = 0x00334D6B;
static const uint32_t COLOR_BORDER     = 0x00445B78;

/* GUI durumu */

static bool menu_open = false;
static int active_app = 0;

static int cursor_x = 100;
static int cursor_y = 100;

static uint32_t cursor_underlay[CURSOR_W * CURSOR_H];

/*
 * 1 = About
 * 2 = Notepad
 * 3 = File Manager
 * 4 = Terminal
 */

/* Bitmap mouse oku: her bit bir piksel */

static const uint16_t cursor_bitmap[CURSOR_H] = {
    0x8000,
    0xC000,
    0xE000,
    0xF000,
    0xF800,
    0xFC00,
    0xFE00,
    0xFF00,
    0xFF80,
    0xFFC0,
    0xFFE0,
    0xFFF0,
    0xFFF8,
    0xF7C0,
    0xE3E0,
    0xC1F0,
    0x80F8,
    0x007C,
    0x0038,
    0x0010
};

static bool cursor_bit(int x, int y)
{
    if (x < 0 || x >= CURSOR_W || y < 0 || y >= CURSOR_H)
        return false;

    return (cursor_bitmap[y] & (0x8000u >> x)) != 0;
}

static void cursor_restore(void)
{
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            framebuffer_set_pixel(
                cursor_x + x,
                cursor_y + y,
                cursor_underlay[y * CURSOR_W + x]
            );
        }
    }
}

static void cursor_draw(void)
{
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            cursor_underlay[y * CURSOR_W + x] =
                framebuffer_get_pixel(cursor_x + x, cursor_y + y);
        }
    }

    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            if (!cursor_bit(x, y))
                continue;

            bool edge =
                !cursor_bit(x - 1, y) ||
                !cursor_bit(x + 1, y) ||
                !cursor_bit(x, y - 1) ||
                !cursor_bit(x, y + 1);

            framebuffer_set_pixel(
                cursor_x + x,
                cursor_y + y,
                edge ? RGB(5, 10, 18) : RGB(250, 252, 255)
            );
        }
    }
}

/* Basit x86 port I/O */

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile (
        "outw %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

/* CMOS RTC saat okuma */

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
    uint8_t second = 0;
    uint8_t minute = 0;
    uint8_t hour = 0;
    uint8_t mode = 0;

    /* RTC guncellemesi sirasinda tutarsiz veri okumamaya calis. */
    for (int attempt = 0; attempt < 10000; attempt++) {
        if (cmos_read(0x0A) & 0x80)
            continue;

        second = cmos_read(0x00);
        minute = cmos_read(0x02);
        hour   = cmos_read(0x04);
        mode   = cmos_read(0x0B);

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
        hour   = bcd_to_binary(hour);
    }

    if (!hour24_mode) {
        hour %= 12;

        if (pm)
            hour += 12;
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

/* Saat bolgesini guncelle. Tum ekran yeniden cizilmez. */

static uint8_t draw_clock(void)
{
    char time_text[9];

    uint8_t second = rtc_get_time(time_text);

    int x = (int)framebuffer_width() - 80;

    framebuffer_draw_rect(
        x, 0, 80, SCREEN_BAR_HEIGHT,
        COLOR_BAR
    );

    framebuffer_draw_text(
        x + 8, 12,
        time_text,
        COLOR_TEXT,
        1
    );

    return second;
}

/* Ust durum cubugu */

static void draw_status_bar(void)
{
    framebuffer_draw_rect(
        0, 0,
        (int)framebuffer_width(),
        SCREEN_BAR_HEIGHT,
        COLOR_BAR
    );

    framebuffer_draw_rect(
        0, SCREEN_BAR_HEIGHT - 1,
        (int)framebuffer_width(),
        1,
        COLOR_ACCENT
    );

    /* Sol menu butonu */
    framebuffer_draw_rect(
        8, 5, 92, 27,
        menu_open ? COLOR_HOVER : COLOR_PANEL
    );

    framebuffer_draw_rect(
        8, 5, 3, 27,
        COLOR_ACCENT
    );

    framebuffer_draw_text(
        19, 11,
        "MyOS",
        COLOR_TEXT,
        1
    );

    framebuffer_draw_text(
        57, 11,
        "[v]",
        COLOR_MUTED,
        1
    );

    draw_clock();
}

/* Masaustu */

static void draw_desktop(void)
{
    framebuffer_clear(COLOR_BACKGROUND);

    /* Hafif dekoratif masaustu cizgisi */
    framebuffer_draw_rect(
        40, 100, 180, 2,
        RGB(53, 100, 148)
    );

    framebuffer_draw_text(
        40, 112,
        "Welcome to MyOS",
        COLOR_TEXT,
        2
    );

    framebuffer_draw_text(
        40, 150,
        "A small desktop operating system",
        COLOR_MUTED,
        1
    );

    draw_status_bar();
}

/* Dropdown menu */

static void draw_menu_item(int y, const char *label)
{
    framebuffer_draw_text(
        MENU_X + 14,
        y + 7,
        label,
        COLOR_TEXT,
        1
    );
}

static void draw_menu(void)
{
    framebuffer_draw_rect(
        MENU_X, MENU_Y,
        MENU_W, MENU_H,
        COLOR_PANEL
    );

    /* Kenarlik */
    framebuffer_draw_rect(
        MENU_X, MENU_Y,
        MENU_W, 1,
        COLOR_BORDER
    );

    framebuffer_draw_rect(
        MENU_X, MENU_Y + MENU_H - 1,
        MENU_W, 1,
        COLOR_BORDER
    );

    framebuffer_draw_rect(
        MENU_X, MENU_Y,
        1, MENU_H,
        COLOR_BORDER
    );

    framebuffer_draw_rect(
        MENU_X + MENU_W - 1, MENU_Y,
        1, MENU_H,
        COLOR_BORDER
    );

    draw_menu_item(49,  "About");
    draw_menu_item(76,  "Notepad");
    draw_menu_item(103, "File Manager");
    draw_menu_item(130, "Terminal");

    /* Ayirici */
    framebuffer_draw_rect(
        MENU_X + 10, 162,
        MENU_W - 20, 1,
        COLOR_BORDER
    );

    draw_menu_item(168, "Shutdown");
    draw_menu_item(195, "Reboot");
}

/* Dummy uygulama pencereleri */

static const char *app_title(void)
{
    switch (active_app) {
    case 1: return "About MyOS";
    case 2: return "Notepad";
    case 3: return "File Manager";
    case 4: return "Terminal";
    default: return "";
    }
}

static void draw_app_window(void)
{
    if (active_app == 0)
        return;

    int x = WINDOW_X;
    int y = WINDOW_Y;
    int w = WINDOW_W;
    int h = WINDOW_H;

    framebuffer_draw_rect(x, y, w, h, COLOR_PANEL_DARK);

    framebuffer_draw_rect(
        x, y, w, 30,
        COLOR_BAR
    );

    framebuffer_draw_rect(
        x, y, w, 1,
        COLOR_BORDER
    );

    framebuffer_draw_rect(
        x, y + h - 1, w, 1,
        COLOR_BORDER
    );

    framebuffer_draw_rect(
        x, y, 1, h,
        COLOR_BORDER
    );

    framebuffer_draw_rect(
        x + w - 1, y, 1, h,
        COLOR_BORDER
    );

    framebuffer_draw_text(
        x + 12, y + 9,
        app_title(),
        COLOR_TEXT,
        1
    );

    framebuffer_draw_rect(
        x + w - 28, y + 4,
        22, 22,
        RGB(130, 45, 55)
    );

    framebuffer_draw_text(
        x + w - 21, y + 11,
        "X",
        COLOR_TEXT,
        1
    );

    switch (active_app) {
    case 1:
        framebuffer_draw_text(
            x + 18, y + 55,
            "MyOS",
            COLOR_ACCENT,
            2
        );

        framebuffer_draw_text(
            x + 18, y + 92,
            "64-bit experimental desktop",
            COLOR_TEXT,
            1
        );

        framebuffer_draw_text(
            x + 18, y + 116,
            "Built from scratch in C.",
            COLOR_MUTED,
            1
        );

        framebuffer_draw_text(
            x + 18, y + 174,
            "Version 0.1",
            COLOR_MUTED,
            1
        );
        break;

    case 2:
        framebuffer_draw_text(
            x + 18, y + 48,
            "Notepad is not implemented yet.",
            COLOR_MUTED,
            1
        );

        framebuffer_draw_text(
            x + 18, y + 72,
            "Text editing will come later.",
            COLOR_MUTED,
            1
        );
        break;

    case 3:
        framebuffer_draw_text(
            x + 18, y + 48,
            "File Manager",
            COLOR_TEXT,
            1
        );

        framebuffer_draw_text(
            x + 18, y + 72,
            "Filesystem support is not ready.",
            COLOR_MUTED,
            1
        );
        break;

    case 4:
        framebuffer_draw_rect(
            x + 12, y + 43,
            w - 24, h - 55,
            RGB(5, 12, 20)
        );

        framebuffer_draw_text(
            x + 22, y + 54,
            "MyOS terminal",
            COLOR_ACCENT,
            1
        );

        framebuffer_draw_text(
            x + 22, y + 78,
            "Shell not implemented.",
            COLOR_TEXT,
            1
        );

        framebuffer_draw_text(
            x + 22, y + 102,
            "> _",
            COLOR_TEXT,
            1
        );
        break;
    }
}

/* Shutdown ve reboot */

static void system_shutdown(void)
{
    serial_write("[POWER] Shutdown requested\n");

    /*
     * Yaygin emulator ACPI kapatma portlari.
     * Gercek donanimda calismasi garanti degildir.
     */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void system_reboot(void)
{
    serial_write("[POWER] Reboot requested\n");

    /* 8042 klavye denetleyicisi uzerinden reset istegi */
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0)
            break;
    }

    outb(0x64, 0xFE);

    /* Desteklenmezse CPU'yu durdur. */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/* Mouse tiklamalari */

static bool inside(
    int px, int py,
    int x, int y,
    int w, int h
)
{
    return px >= x && px < x + w &&
           py >= y && py < y + h;
}

static void handle_click(int x, int y)
{
    /* Sol ustteki MyOS butonu */
    if (inside(x, y, 8, 5, 92, 27)) {
        menu_open = !menu_open;

        draw_status_bar();

        if (menu_open)
            draw_menu();

        return;
    }

    if (menu_open) {
        if (inside(x, y, MENU_X, MENU_Y, MENU_W, MENU_H)) {
            if (inside(x, y, MENU_X, 49, MENU_W, 25)) {
                active_app = 1;
            } else if (inside(x, y, MENU_X, 76, MENU_W, 25)) {
                active_app = 2;
            } else if (inside(x, y, MENU_X, 103, MENU_W, 25)) {
                active_app = 3;
            } else if (inside(x, y, MENU_X, 130, MENU_W, 25)) {
                active_app = 4;
            } else if (inside(x, y, MENU_X, 168, MENU_W, 25)) {
                menu_open = false;
                draw_status_bar();
                system_shutdown();
            } else if (inside(x, y, MENU_X, 195, MENU_W, 25)) {
                menu_open = false;
                draw_status_bar();
                system_reboot();
            }

            menu_open = false;

            draw_status_bar();
            draw_app_window();

            return;
        }

        menu_open = false;
        draw_status_bar();
    }

    /* Pencerenin X dugmesi */
    if (active_app != 0 &&
        inside(x, y, WINDOW_X + WINDOW_W - 28,
                    WINDOW_Y + 4, 22, 22)) {

        active_app = 0;

        /* Pencere altindaki masaustu alanini yeniden ciz. */
        draw_desktop();

        if (menu_open)
            draw_menu();

        return;
    }
}

/* Kernel giris noktasi */

static void stop_with_error(const char *message)
{
    serial_write("[FATAL] ");
    serial_write(message);
    serial_write("\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kernel_main(void)
{
    serial_init();
    serial_write("\n[BOOT] MyOS desktop starting\n");

    if (framebuffer_request.response == 0)
        stop_with_error("No Limine framebuffer response");

    if (framebuffer_request.response->framebuffer_count == 0)
        stop_with_error("No framebuffer available");

    struct limine_framebuffer *fb =
        framebuffer_request.response->framebuffers[0];

    if (!framebuffer_init(fb))
        stop_with_error("Framebuffer initialization failed");

    serial_write("[BOOT] Framebuffer ready\n");

    draw_desktop();

    cursor_x = (int)framebuffer_width() / 2;
    cursor_y = (int)framebuffer_height() / 2;

    if (cursor_x > (int)framebuffer_width() - CURSOR_W)
        cursor_x = (int)framebuffer_width() - CURSOR_W;

    if (cursor_y > (int)framebuffer_height() - CURSOR_H)
        cursor_y = (int)framebuffer_height() - CURSOR_H;

    cursor_draw();

    bool mouse_available = ps2_mouse_init();

    if (mouse_available)
        serial_write("[BOOT] PS/2 mouse ready\n");
    else
        serial_write("[WARN] PS/2 mouse unavailable\n");

    uint8_t previous_buttons = 0;
    int last_clock_second = -1;

    for (;;) {
        /*
         * RTC saatini kontrol et.
         * Yalnizca saat alanini guncelle.
         */
        char time_text[9];
        uint8_t second = rtc_get_time(time_text);

        if ((int)second != last_clock_second) {
            cursor_restore();
            draw_clock();
            cursor_draw();

            last_clock_second = (int)second;
        }

        if (!mouse_available)
            continue;

        int dx = 0;
        int dy = 0;
        uint8_t buttons = 0;

        if (!ps2_mouse_poll(&dx, &dy, &buttons))
            continue;

        int max_x = (int)framebuffer_width() - CURSOR_W;
        int max_y = (int)framebuffer_height() - CURSOR_H;

        int new_x = cursor_x + dx;
        int new_y = cursor_y + dy;

        if (new_x < 0) new_x = 0;
        if (new_y < 0) new_y = 0;
        if (new_x > max_x) new_x = max_x;
        if (new_y > max_y) new_y = max_y;

        bool moved = new_x != cursor_x || new_y != cursor_y;

        bool left_clicked =
            (buttons & 1u) != 0 &&
            (previous_buttons & 1u) == 0;

        if (moved || left_clicked) {
            cursor_restore();

            cursor_x = new_x;
            cursor_y = new_y;

            if (left_clicked)
                handle_click(cursor_x, cursor_y);

            cursor_draw();
        }

        previous_buttons = buttons;
    }
}
