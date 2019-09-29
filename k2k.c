#define _XOPEN_SOURCE 500
#ifdef VERBOSE
# include <stdio.h> /* fprintf() */
#endif
#include <stdlib.h> /* EXIT_FAILURE */
#include <errno.h> /* errno */
#include <unistd.h> /* STD*_FILENO, read() */
#include <string.h> /* memcpy() */
#include <linux/input.h> /* KEY_*, struct input_event */

#define ARRAY_LEN(a) (int)(sizeof(a) / sizeof(*a))
#define KEY_PAIR(key) { KEY_LEFT##key, KEY_RIGHT##key }

#ifdef VERBOSE
# define dbgprintf(msg, ...) fprintf(stderr, msg "\n", ##__VA_ARGS__)
#else
# define dbgprintf(msg, ...) ((void)0)
#endif

enum event_values {
    EVENT_VALUE_KEYUP = 0,
    EVENT_VALUE_KEYDOWN = 1,
    EVENT_VALUE_KEYREPEAT = 2,
};

#define MAX_EVENTS 10

static struct input_event revbuf[MAX_EVENTS];
static size_t revlen = 0;
static size_t riev = 0;
static struct input_event wevbuf[MAX_EVENTS];
static size_t wevlen = 0;

static void
read_events(void) {
    for (;;) {
        switch ((revlen = read(STDIN_FILENO, revbuf, sizeof revbuf))) {
        case -1:
            if (errno == EINTR)
                continue;
            /* Fall through. */
        case 0:
            exit(EXIT_FAILURE);
        default:
            revlen /= sizeof *revbuf, riev = 0;
            return;
        }
    }
}

static void
write_events(void) {
    if (0 == wevlen)
        return;

    for (;;) {
        switch (write(STDOUT_FILENO, wevbuf, sizeof *wevbuf * wevlen)) {
        case -1:
            if (errno == EINTR)
                continue;
            exit(EXIT_FAILURE);
        default:
            wevlen = 0;
            return;
        }
    }
}

static void
write_event(struct input_event const *ev) {
    wevbuf[wevlen++] = *ev;
    /* Flush events if buffer is full. */
    if (wevlen == MAX_EVENTS)
        write_events();
}

static void
write_key_event(int code, int value) {
    struct input_event ev = {
        .type = EV_KEY,
        .code = code,
        .value = value
    };
    write_event(&ev);
}

static int
key_ismod(int const keycode) {
    switch (keycode) {
    default:
        return 0;
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
    case KEY_LEFTCTRL:  case KEY_RIGHTCTRL:
    case KEY_LEFTALT:   case KEY_RIGHTALT:
    case KEY_LEFTMETA:  case KEY_RIGHTMETA:
        return 1;
    }
}

/** Map a key to another. */
static struct map_rule_info {
    int const from_key; /** Map what? */
    int const to_key; /** To what? */
} MAP_RULES[] = {
#include "map-rules.h.in"
};

/** Bind multiple actions to a single key. */
static struct tap_rule_info {
    int const base_key; /** Key to override. */
    int const tap_key; /** Act as this key when pressed alone. */
    int const hold_key; /** Act as this key when pressed with others. */
    int const repeat_key; /** Act as this key when pressed alone for longer
                            time. Optional. */
    int const repeat_delay; /** Wait this much repeat events to arrive after
                              acting as repeat key. */
    int const tap_mods; /** Whether to modifier keys apply to `tap_key`. */

    int act_key; /** How `base_key` acts as actually. */
    int curr_delay; /** Internal counter for `repeat_delay`. */
} TAP_RULES[] = {
#include "tap-rules.h.in"
};

/** Bind actions to multiple keys. */
static struct multi_rule_info {
    int const keys[8]; /** Keys to watch. */
    int const down_press[2]; /** Press first key and release second key when
                               toggled down. */
    int const up_press[2]; /** Press first key and release second key when
                             toggled up. */
    int const nhold[2]; /** Specifies how much keys must be hold to stay in
                          toggled down state. */

    unsigned keys_down; /** Current state of `keys`. */
    int repeated_key; /** What key to override. */
    int repeating_key; /** A key that surely repeating now. */
    int is_down: 1; /** Internal toggled state. */
    int was_down: 1; /** Internal repeating state. */
    int repeated_key_repeated: 1; /** Internal repeating state. */
} MULTI_RULES[] = {
/* Press `key` both when toggled down and toggled up. */
#define SWITCH(key) .down_press = { (key), (key) }, .up_press = { (key), (key) }
/* Press down when toggled down and release only when toggled up. */
#define TOGGLE(key) .down_press = { (key), KEY_RESERVED }, .up_press = { KEY_RESERVED, (key) }
/* Press key once, only when toggled down. */
#define PRESS_ONCE(key) .down_press = { (key), (key) }, .up_press = { KEY_RESERVED, KEY_RESERVED }
#define PRESS TOGGLE
#define TO_KEY TOGGLE
#include "multi-rules.h.in"
#undef TO_KEY
#undef PRESS
#undef PRESS_ONCE
#undef TOGGLE
#undef SWITCH
};

int
main(void) {
    for (;;) {
        int i;
        struct input_event ev;
        int ignore_event;

        if (riev == revlen) {
            /* Flush events to be written. */
            write_events();
            /* Read new events. */
            read_events();
        }
        ev = revbuf[riev++];

        if (ev.type != EV_KEY) {
            if (ev.type == EV_MSC && ev.code == MSC_SCAN)
                goto ignore_event;

            goto write;
        }

        for (i = 0; i < ARRAY_LEN(MAP_RULES); ++i) {
            struct map_rule_info *const v = &MAP_RULES[i];
            if (ev.code == v->from_key) {
                if (v->to_key != KEY_RESERVED) {
                    dbgprintf("Map rule #%d: %d -> %d.", i, ev.code, v->to_key);
                    ev.code = v->to_key;
                    break;
                } else {
                    dbgprintf("Map rule #%d: %d -> (ignore).", i, ev.code);
                    goto ignore_event;
                }
            }
        }

        for (i = 0; i < ARRAY_LEN(TAP_RULES); ++i) {
            struct tap_rule_info *const v = &TAP_RULES[i];

            if (ev.code == v->base_key) {
                switch (ev.value) {
                case EVENT_VALUE_KEYDOWN:
                    if (v->act_key == KEY_RESERVED) {
                        dbgprintf("Tap rule #%d: Ignore: Waiting.", i);
                        v->act_key = -1;
                        v->curr_delay = v->repeat_delay;
                    }
                    goto ignore_event;
                case EVENT_VALUE_KEYREPEAT:
                    if (v->act_key == -1) {
                        /* Do not repeat this key. */
                        if (v->repeat_key == KEY_RESERVED)
                            goto ignore_event;

                        if (v->curr_delay-- > 0)
                            goto ignore_event;

                        v->act_key = v->repeat_key;
                        dbgprintf("Tap rule #%d: Act as repeat key.", i);
                        write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                    }

                    ev.code = v->act_key;
                    break;
                case EVENT_VALUE_KEYUP:
                    if (v->act_key == -1) {
                        v->act_key = v->tap_key;
                        dbgprintf("Tap rule #%d: Act as tap key.", i);
                        write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                    }
                    ev.code = v->act_key;

                    v->act_key = KEY_RESERVED;
                    break;
                }
            } else if (v->act_key == -1) {
                /* Key `hold_key` needs to be hold down now. */
                if (ev.value == EVENT_VALUE_KEYDOWN && (!key_ismod(ev.code) || !v->tap_mods)) {
                    v->act_key = v->hold_key;
                    dbgprintf("Tap rule #%d: Act as hold key.", i);
                    write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                }
            }
        }

        ignore_event = 0;
        for (i = 0; i < ARRAY_LEN(MULTI_RULES); ++i) {
            struct multi_rule_info *const v = &MULTI_RULES[i];
            int j, ndown = 0, ntotal;
            int key_matches = 0;

            for (j = 0; j < ARRAY_LEN(v->keys) && v->keys[j] != KEY_RESERVED; ++j) {
                if (ev.code == v->keys[j]) {
                    key_matches = 1;
                    if (v->repeated_key == ev.code)
                        v->repeated_key = KEY_RESERVED;

                    switch (ev.value) {
                    case EVENT_VALUE_KEYUP:
                        v->keys_down &= ~(1 << j);
                        break;
                    case EVENT_VALUE_KEYREPEAT:
                        if (v->repeated_key == KEY_RESERVED || v->repeated_key == ev.code) {
                            v->repeated_key_repeated = 1;
                            v->repeated_key = ev.code;
                        } else if (!v->repeated_key_repeated && v->repeating_key == ev.code) {
                            v->repeated_key_repeated = 1;
                            v->repeated_key = ev.code;
                            dbgprintf("Multi rule #%d: Repeating key changed.", i);
                        } else {
                            v->repeated_key_repeated = 0;
                            v->repeating_key = ev.code;
                        }
                        break;
                    case EVENT_VALUE_KEYDOWN:
                        v->keys_down |= 1 << j;
                        break;
                    }
                }
                ndown += (v->keys_down >> j) & 1;
            }
            ntotal = j;

            v->was_down = v->is_down;
            v->is_down = !v->is_down
                ? ndown == ntotal
                : v->nhold[0] <= ndown && ndown <= v->nhold[1];

            if (v->was_down != v->is_down) {
                int press[2];
                memcpy(press, v->is_down ? v->down_press : v->up_press, sizeof press);

                dbgprintf("Multi rule #%d: Toggled %s.", i, (v->is_down ? "down" : "up"));

                for (j = 0; j < ntotal; ++j) {
                    if ((v->keys_down >> j) & 1) {
                        /* Do not send release event if we will press it immediately (and vica-versa). */
                        if (press[!v->is_down] == v->keys[j]) {
                            press[!v->is_down] = KEY_RESERVED;
                            continue;
                        }
                        write_key_event(v->keys[j], v->is_down ? EVENT_VALUE_KEYUP : EVENT_VALUE_KEYDOWN);
                    }
                }

                if (press[0] != KEY_RESERVED)
                    write_key_event(press[0], EVENT_VALUE_KEYDOWN);

                if (press[1] != KEY_RESERVED)
                    write_key_event(press[1], EVENT_VALUE_KEYUP);

                ignore_event = 1;
            } else if (v->is_down
                    && ev.code == v->repeated_key
                    && v->down_press[0] != KEY_RESERVED && v->down_press[1] == KEY_RESERVED
                    && v->up_press[0]   == KEY_RESERVED && v->up_press[1]   == v->down_press[0]) {
                dbgprintf("Multi rule #%d: Repeat.", i);
                ev.code = v->down_press[0];
            } else {
                if (key_matches && v->is_down) {
                    dbgprintf("Multi rule #%d: Ignore matched key.", i);
                    ignore_event = 1;
                }

            }
        }
        if (ignore_event)
            goto ignore_event;

    write:
        write_event(&ev);
    ignore_event:;
    }
}
/* vi:set ft=c: */
