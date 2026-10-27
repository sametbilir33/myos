#ifndef MYOS_GUI_EVENTS_H
#define MYOS_GUI_EVENTS_H

#include <stdbool.h>

enum gui_event_type {
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_MOUSE_BUTTON_DOWN,
    GUI_EVENT_MOUSE_BUTTON_UP
};

struct gui_event {
    enum gui_event_type type;
    int x;
    int y;
    int button;
};

typedef void (*gui_event_handler)(const struct gui_event *event);

void gui_events_init(void);
void gui_events_set_handler(gui_event_handler handler);
void gui_events_dispatch(void);
bool gui_event_push(struct gui_event event);
bool gui_event_poll(struct gui_event *event);

#endif