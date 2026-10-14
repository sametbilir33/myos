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

void gui_events_init(void);
bool gui_event_push(struct gui_event event);
bool gui_event_poll(struct gui_event *event);

#endif