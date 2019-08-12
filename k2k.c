#define _XOPEN_SOURCE 500
#ifdef VERBOSE
# include <stdio.h> /* fprintf() */
#endif
#include <stdlib.h> /* EXIT_FAILURE */
#include <errno.h> /* errno */
#include <unistd.h> /* STD*_FILENO, read() */
#include <linux/input.h> /* struct input_event */

#ifndef TOGGLE_RULE_MAXKEYS
# define TOGGLE_RULE_MAXKEYS 10
#endif

#define ARRAY_LEN(a) (sizeof(a) / sizeof(*a))
#define KEY_PAIR(key) { KEY_LEFT##key, KEY_RIGHT##key }

#ifdef VERBOSE
# define dbgprintf(msg, ...) fprintf(stderr, msg, ##__VA_ARGS__)
#else
# define dbgprintf(msg, ...) ((void)0)
#endif

enum event_values {
    EVENT_VALUE_KEYUP = 0,
    EVENT_VALUE_KEYDOWN = 1,
    EVENT_VALUE_KEYREPEAT = 2,
};

static int
input_event_wait(struct input_event *ev) {
    for (;;) {
        switch (read(STDIN_FILENO, ev, sizeof *ev)) {
        case sizeof *ev:
            return 1;
        case -1:
            if (errno == EINTR)
                continue;
            /* Fall through. */
        default:
            return 0;
        }
    }
}

static void
write_event(struct input_event const*ev) {
    for (;;) {
        switch (write(STDOUT_FILENO, ev, sizeof *ev)) {
        case sizeof *ev:
            return;
        case -1:
            if (errno == EINTR)
                continue;
            /* Fall through. */
        default:
            exit(EXIT_FAILURE);
        }
    }
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

static struct map_rule_info {
    int const from_key; /** Map what? */
    int const to_key; /** To what? */
} MAP_RULES[] = {
#include "map-rules.h.in"
};

static struct toggle_rule_info {
    int const keys[TOGGLE_RULE_MAXKEYS]; /** Keys to watch. */
    int const actions[2][2]; /** Up & down actinons to take when toggled down
                               and up. */
    size_t const ntoggle_down; /** Number of `keys` to press to toggle down.
                                 Defaults to count of `keys`. */
    size_t const ntoggle_up; /** Number of `keys` to press to toggle up.
                               Defaults to count of `keys`. */
    char keys_down[TOGGLE_RULE_MAXKEYS]; /** Array that keeps counting pressed
                                           down `keys`. */
    int is_down: 1; /** Internal toggled state. */
    int ignore_change: 1; /** Whether to ignore state changings until all
                            `keys` are up. */
} TOGGLE_RULES[] = {
#include "toggle-rules.h.in"
};

static struct tap_rule_info {
    int const base_key; /** Key to override. */
    int const tap_key; /** Act as this key when pressed alone. */
    int const hold_key; /** Act as this key when pressed with others. */
    int const repeat_key; /** Act as this key when pressed alone for longer
                            time. Optional. */
    int const repeat_delay;/** Wait this much repeat events to arrive after
                             acting as repeat key. */
    int act_key; /** How `base_key` acts as actually. */
    int curr_delay; /** Internal counter for `repeat_delay`. */
} TAP_RULES[] = {
#include "tap-rules.h.in"
};

int
main(void) {
    struct input_event ev;

    while (input_event_wait(&ev)) {
        size_t i;

        if (ev.type != EV_KEY) {
            if (ev.type == EV_MSC && ev.code == MSC_SCAN)
                goto ignore_event;

            goto write;
        }

        for (i = 0; i < ARRAY_LEN(MAP_RULES); ++i) {
            struct map_rule_info *const v = &MAP_RULES[i];
            if (ev.code == v->from_key) {
                dbgprintf("Map rule #%zu: %d -> %d.\n", i, ev.code, v->to_key);
                ev.code = v->to_key;
            }
        }

        for (i = 0; i < ARRAY_LEN(TAP_RULES); ++i) {
            struct tap_rule_info *const v = &TAP_RULES[i];

            if (ev.code == v->base_key) {
                switch (ev.value) {
                case EVENT_VALUE_KEYDOWN:
                    if (v->act_key == KEY_RESERVED) {
                        dbgprintf("Tap rule #%zu: Ignore: Waiting.\n", i);
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
                        dbgprintf("Tap rule #%zu: Act as repeat key.\n", i);
                        write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                    }

                    ev.code = v->act_key;
                    break;
                case EVENT_VALUE_KEYUP:
                    if (v->act_key == -1) {
                        v->act_key = v->tap_key;
                        dbgprintf("Tap rule #%zu: Act as tap key.\n", i);
                        write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                    }
                    ev.code = v->act_key;

                    v->act_key = KEY_RESERVED;
                    break;
                }
            } else if (v->act_key == -1) {
                /* Key `hold_key` needs to be hold down now. */
                if (ev.value == EVENT_VALUE_KEYDOWN && !key_ismod(ev.code)) {
                    v->act_key = v->hold_key;
                    dbgprintf("Tap rule #%zu: Act as hold key.\n", i);
                    write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                }
            }
        }

        for (i = 0; i < ARRAY_LEN(TOGGLE_RULES); ++i) {
            struct toggle_rule_info *const v = &TOGGLE_RULES[i];
            size_t j, ndown = 0, ntotal = 0;
            int ignore_event = 0;

            for (j = 0; j < ARRAY_LEN(v->keys); ++ntotal, ++j) {
                if (v->keys[j] == KEY_RESERVED)
                    break;

                if (ev.code == v->keys[j]) {
                    switch (ev.value) {
                    case EVENT_VALUE_KEYUP:
                        v->keys_down[j] = 0;
                        if (v->actions[0][0] == ev.code && v->is_down)
                            ignore_event = 1;
                        break;
                    case EVENT_VALUE_KEYDOWN:
                    case EVENT_VALUE_KEYREPEAT:
                        v->keys_down[j] = 1;
                        if (v->actions[1][1] == ev.code && v->is_down)
                            ignore_event = 1;
                        break;
                    }
                }
                ndown += v->keys_down[j];
            }

            if (ndown > 0)
                dbgprintf("Toggle rule #%zu: %zu down%s.\n",
                        i, ndown,
                        (v->ignore_change ? ", ignore change" : ""));

            if (!v->ignore_change
                && ndown == (v->is_down
                    ? (v->ntoggle_up   > 0 ? v->ntoggle_up   : ntotal)
                    : (v->ntoggle_down > 0 ? v->ntoggle_down : ntotal))) {
                int const*const keys = v->actions[!v->is_down];

                v->ignore_change = 1, v->is_down ^= 1;
                dbgprintf("Toggle rule #%zu: Toggled %s now.\n", i, (v->is_down ? "down" : "up"));

                for (j = 0; j < ntotal; ++j) {
                    if (keys[0] == v->keys[j]
                        || keys[1] == v->keys[j]) {
                        goto filtered;
                    }
                }

                if (keys[0] != KEY_RESERVED)
                    write_key_event(keys[0], EVENT_VALUE_KEYDOWN);

                if (keys[1] != KEY_RESERVED)
                    write_key_event(keys[1], EVENT_VALUE_KEYUP);
            filtered:;
            } else if (ndown == 0) {
                v->ignore_change = 0;
            }

            if (ignore_event)
                goto ignore_event;
        }

    write:
        write_event(&ev);
    ignore_event:;
    }
}
