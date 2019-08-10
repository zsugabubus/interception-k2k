#define _XOPEN_SOURCE 500
#ifdef VERBOSE
#include <stdio.h> /* fprintf() */
#endif
#include <stdlib.h> /* EXIT_FAILURE */
#include <errno.h> /* errno */
#include <unistd.h> /* usleep */
#include <linux/input.h> /* struct input_event */

#define ARRAY_LEN(a) (sizeof(a) / sizeof(*a))
#define DELAY() usleep(12000);
#define KEY_PAIR(key) { KEY_LEFT##key, KEY_RIGHT##key }

static char *EVVAL2STR[] = {"up", "down", "repeat"};
#ifdef VERBOSE
#define DPRINT(msg, ...) fprintf(stderr, msg, ##__VA_ARGS__)
#else
#define DPRINT(msg, ...) ((void)0)
#endif

static int
input_event_wait(struct input_event *event) {
    for (;;) {
        switch (read(STDIN_FILENO, event, sizeof *event)) {
        case sizeof *event:
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
write_event(struct input_event const*event) {
    for (;;) {
        switch (write(STDOUT_FILENO, event, sizeof *event)) {
        case sizeof *event:
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
write_sync(void) {
    struct input_event const event = {
        .type = EV_SYN,
        .code = SYN_REPORT,
        .value = 0
    };
    write_event(&event);
}

static void
write_key_event(int code, int value) {
    struct input_event event = {
        .type = EV_KEY,
        .code = code,
        .value = value
    };
    DPRINT("<<< Key %s: %d.\n", EVVAL2STR[value], code);
    write_event(&event);
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

static struct single_rule_info {
    int const from_key;
    int const to_key;
} SINGLE_RULES[] = {
#include "single-rules.h.in"
};

static struct double_rule_info {
    int const keys[2];
    int const actions[2][2];
    size_t const ncancel;
    int is_down[2];
    int locked: 1;
    int locking: 1;
} DOUBLE_RULES[] = {
#include "double-rules.h.in"
};

static struct tap_rule_info {
    int const tap_key;
    int const hold_key;
    int act_key;
} TAP_RULES[] = {
#include "tap-rules.h.in"
};

int
main(void) {
    struct input_event ev;

    (void)EVVAL2STR;

    while (input_event_wait(&ev)) {
        size_t i;
        int skip_write = 0;

        if (ev.type == EV_MSC && ev.code == MSC_SCAN)
            goto skip_write;

        if (ev.type != EV_KEY)
            goto write;

        DPRINT(">>> Key %s: %d.\n", EVVAL2STR[ev.value], ev.code);

        for (i = 0; i < ARRAY_LEN(SINGLE_RULES); ++i) {
            struct single_rule_info *const v = &SINGLE_RULES[i];
            if (ev.code == v->from_key) {
                DPRINT("Translate rule #%zu: Translate key %d -> %d.\n", i, ev.code, v->to_key);
                ev.code = v->to_key;
            }
        }

        for (i = 0; i < ARRAY_LEN(TAP_RULES); ++i) {
            struct tap_rule_info *const v = &TAP_RULES[i];

            if (ev.code == v->tap_key) {
                switch (ev.value) {
                case 1/*down*/:
                    if (v->act_key == KEY_RESERVED) {
                        DPRINT("Tap rule #%zu: Actived.\n", i);
                        v->act_key = v->tap_key;
                        /* Wait for further key chords. */
                case 2/*repeat*/:
                        goto skip_write;
                    }
                    break;
                case 0/*up*/:
                    if (v->act_key == v->tap_key) {
                        DPRINT("Tap rule #%zu: Tap key down.\n", i);
                        write_key_event(v->act_key, 1/*down*/);
                        write_sync();
                        DELAY();
                    }
                    DPRINT("Tap rule #%zu: Translating key up: %d -> %d.\n", i, ev.code, v->act_key);
                    ev.code = v->act_key;
                    v->act_key = KEY_RESERVED;
                    DPRINT("Tap rule #%zu: Deactivated.\n", i);
                    break;
                }
            } else {
                if (ev.value != 0/*up*/ && !key_ismod(ev.code)) {
                    /* Key `hold_key` needs to be hold down now. */
                    if (v->act_key == v->tap_key) {
                        DPRINT("Tap rule #%zu: Hold key down.\n", i);
                        v->act_key = v->hold_key;
                        write_key_event(v->act_key, 1/*down*/);
                        write_sync();
                        DELAY();
                    }
                }
            }
        }

        for (i = 0; i < ARRAY_LEN(DOUBLE_RULES); ++i) {
            struct double_rule_info *const v = &DOUBLE_RULES[i];
            size_t j;
            size_t ndown = 0;

            for (j = 0; j < ARRAY_LEN(v->keys); ++j) {
                if (ev.code == v->keys[j]) {
                    switch (ev.value) {
                    case 0/*up*/:
                        v->is_down[j] = 0;
                        if (v->actions[0][0] == ev.code && v->locked)
                            skip_write = 1;
                        break;
                    case 1/*down*/:
                    case 2/*repet*/:
                        v->is_down[j] = 1;
                        if (v->actions[1][1] == ev.code && v->locked)
                            skip_write = 1;
                        break;
                    }
                }
                ndown += v->is_down[j];
            }

            if (ndown > 0)
                DPRINT("Lock rule #%zu: %s: %zu down.\n", i, (v->locked ? "Locked" : "Unlocked"), ndown);

            if (!v->locking && ndown == (v->locked ? v->ncancel : ARRAY_LEN(v->keys))) {
                int const*const keys = v->actions[!v->locked];
                int delay = 0;

                v->locking = 1, v->locked ^= 1;
                DPRINT("Lock rule #%zu: %s now.\n", i, (v->locked ? "Locked" : "Unlocked"));

                for (j = 0; j < ARRAY_LEN(v->keys); ++j) {
                    if (keys[0] == v->keys[j]
                        || keys[1] == v->keys[j]) {
                        goto filtered;
                    }
                }

                DPRINT("Lock rule #%zu: Key actions.\n", i);
                if (keys[0] != KEY_RESERVED) {
                    write_key_event(keys[0], 1/*down*/);
                    delay = 1;
                }

                if (keys[1] != KEY_RESERVED) {
                    if (delay)
                        DELAY();
                    write_key_event(keys[1], 0/*up*/);
                    write_sync();
                }
            filtered:;
            } else if (ndown == 0) {
                v->locking = 0;
            }
        }

    write:
        if (!skip_write) {
            DPRINT("<<< type: %d value: %d code: %d.\n", ev.type, ev.value, ev.code);
            write_event(&ev);
        }
    skip_write:;
    }
}
