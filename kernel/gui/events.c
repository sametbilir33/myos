#include <gui/events.h>

#define EVENT_QUEUE_CAPACITY 32

static struct gui_event queue[EVENT_QUEUE_CAPACITY];
static unsigned int read_index;
static unsigned int write_index;
static unsigned int event_count;

void gui_events_init(void)
{
    read_index = 0;
    write_index = 0;
    event_count = 0;
}

bool gui_event_push(struct gui_event event)
{
    if (event_count >= EVENT_QUEUE_CAPACITY)
        return false;

    queue[write_index] = event;
    write_index = (write_index + 1) % EVENT_QUEUE_CAPACITY;
    event_count++;

    return true;
}

bool gui_event_poll(struct gui_event *event)
{
    if (event == 0 || event_count == 0)
        return false;

    *event = queue[read_index];
    read_index = (read_index + 1) % EVENT_QUEUE_CAPACITY;
    event_count--;

    return true;
}

static gui_event_handler active_handler;

void gui_events_set_handler(gui_event_handler handler)
{
    active_handler = handler;
}

void gui_events_dispatch(void)
{
    struct gui_event event;

    while (gui_event_poll(&event)) {
        if (active_handler != 0)
            active_handler(&event);
    }
}
