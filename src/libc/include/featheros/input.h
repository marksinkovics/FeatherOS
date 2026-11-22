#ifndef FEATHEROS_LIBC_FEATHEROS_INPUT_H
#define FEATHEROS_LIBC_FEATHEROS_INPUT_H

#include <stdint.h>

// /dev/mouse returns whole struct input_event records, as Linux's evdev does on 64-bit targets,
// with Linux's values. read needs room for at least one (sizeof(struct input_event)), else it
// fails with EINVAL.
struct input_event
{
    int64_t tv_sec;
    int64_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02

#define SYN_REPORT 0
#define SYN_DROPPED 3

#define REL_X 0x00
#define REL_Y 0x01
#define REL_HWHEEL 0x06
#define REL_WHEEL 0x08

#define BTN_MOUSE 0x110
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE 0x113
#define BTN_EXTRA 0x114
#define BTN_FORWARD 0x115
#define BTN_BACK 0x116
#define BTN_TASK 0x117

#endif
