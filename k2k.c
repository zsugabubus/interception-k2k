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

/* KEY_* codes: /usr/include/linux/input-event-codes.h */

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
    /** Note: If it's a modifier key, it will be pressed down immediately and
     * released if need to act as `tap_key` or `repeat_key`. It makes convenient
     * to use modifier keys with mouse. */
    int const repeat_key; /** Act as this key when pressed alone for longer
                            time. Optional. */
    int const repeat_delay; /** Wait this much repeat events to arrive after
                              acting as repeat key. */
    int const tap_mods; /** Whether to modifier keys apply to `tap_key`. */

    int act_key; /** How `base_key` acts as actually. */
    /*
     * Special values:
     * - `-1`: Waiting.
     * - `KEY_RESERVED`: Idle.
     **/
    int curr_delay; /** Internal counter for `repeat_delay`. */
} TAP_RULES[] = {
#include "tap-rules.h.in"
};

/** Bind actions to multiple keys.
 *
 * Take care of `down_press` and `up_press` to be balanced.
 */
static struct multi_rule_info {
    int const keys[8]; /** Keys to watch. */
    int const down_press[2]; /** Press first key and release second key when
                               toggled down. */
    int const up_press[2]; /** Press first key and release second key when
                             toggled up. */
    int const nbeforedown; /** Only allow toggling down after this many `keys`
                             have been down. Negative value means inequality. */
    int const nbeforeup; /** Only allow toggling up after this many `keys` have
                           been down. Negative value means inequality. */
    int const nup; /** Toggle up when this many `keys` are down together.
                     Negative value means inequality. */

    unsigned keys_down; /** Bitmap of down `keys`. */
    int repeated_key_repeated: 1; /** Did we see `repeated_key` repeating? */
    int is_down: 1; /** Internal key state. */
    int can_toggle: 1; /** Whether we can change toggled state. */
    int repeated_key; /** Which key to override for a repeat action. */
    int repeating_key; /** The key that we saw last time to repeating. */
} MULTI_RULES[] = {
/* Press `key` when toggled down and once again when toggled up. */
#define PRESS_ON_TOGGLE(key) .down_press = { (key),               (key) }, .up_press = { (key),               (key) }
/* Act as `key`. */
#define TO_KEY(key)          .down_press = { (key),        KEY_RESERVED }, .up_press = { KEY_RESERVED,        (key) }
/* Press `key` once when toggled down. */
#define PRESS_ON_DOWN(key)   .down_press = { (key),               (key) }, .up_press = { KEY_RESERVED, KEY_RESERVED }
/* Press `key` once when toggled up. */
#define PRESS_ON_UP(key)     .down_press = { KEY_RESERVED, KEY_RESERVED }, .up_press = {        (key),        (key) }
/* Press once. */
#define PRESS_ONCE(key)      PRESS_ON_DOWN
/* Synonym for `TO_KEY`. */
#define PRESS                TO_KEY

/* Toggle down when all `keys` are down and toggle up as soon as not all `keys`
 * are down. `nkeys` need to specify the number of `keys` for technical
 * limitations. This is the most natural behavior, so you probably will need
 * this the most time.
 *
 * Explanation:
 * As stated above negative values mean inequality, so:
 * - nbeforedown: Allow toggling down when not all keys are down, it's just a
 *   no-op.
 * - (ndown): Toggle down when all keys are down. (Implicit rule.)
 * - nbeforeup: Allow toggling up immediately after we have released some of
 *   the keys.
 * - nup: As soon as we don't press down all keys, toggle up.
 */
#define DOWN_IFF_ALL_DOWN(nkeys) .nbeforedown = -(nkeys), .nbeforeup = -(nkeys), .nup = -(nkeys)
/* Toggling for lock keys.
 *
 * Explanation:
 * - nbeforedown: Act as a no-op.
 * - nbeforeup: Allow toggling up after we have released all keys. We need this
 *   because to toggle down you have to press both keys, but if toggle up is
 *   set to one key then it would toggle up immediately as you release any of
 *   the keys.
 * - nup: After all keys have been released (nbeforeup), it's required only to
 *   press one of the keys. If you press the other key you will be again in
 *   toggled down state. If you want avoid this set `nbeforedown` also 0, so
 *   it's required to release all keys before you can do this. */
#define BOTH_DOWN_ONE_UP() .nbeforedown = -2, .nbeforeup = 0, .nup = 1

#include "multi-rules.h.in"
#undef BOTH_DOWN_ONE_UP
#undef DOWN_IFF_ALL_DOWN

#undef PRESS
#undef PRESS_ONCE
#undef PRESS_ON_UP
#undef PRESS_ON_DOWN
#undef TO_KEY
#undef PRESS_ON_TOGGLE
};

int
main(void) {
    for (;;) {
        int i;
        struct input_event ev;

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
                        /* A hold modifier keys can be pressed now and released
                         * if need to act as tap key in the future. */
                        if (key_ismod(v->hold_key))
                            write_key_event(v->hold_key, EVENT_VALUE_KEYDOWN);
                        v->curr_delay = v->repeat_delay;
                    }
                    goto ignore_event;
                case EVENT_VALUE_KEYREPEAT:
                    if (v->act_key == -1) {
                        /* Do not repeat this key. */
                        if (v->repeat_key == KEY_RESERVED)
                            goto ignore_event;

                        /* Wait for more key repeats. */
                        if (v->curr_delay-- > 0)
                            goto ignore_event;

                        /* Timeout reached, act as repeat key. */
                        dbgprintf("Tap rule #%d: Act as repeat key.", i);
                        v->act_key = v->repeat_key;
                        if (key_ismod(v->hold_key))
                            write_key_event(v->hold_key, EVENT_VALUE_KEYUP);
                        write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                    }

                    ev.code = v->act_key;
                    break;
                case EVENT_VALUE_KEYUP:
                    if (v->act_key == -1) {
                        dbgprintf("Tap rule #%d: Act as tap key.", i);
                        v->act_key = v->tap_key;
                        if (key_ismod(v->hold_key))
                            write_key_event(v->hold_key, EVENT_VALUE_KEYUP);
                        write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                    }
                    ev.code = v->act_key;

                    v->act_key = KEY_RESERVED;
                    break;
                }
            } else if (v->act_key == -1) {
                /* Key `hold_key` needs to be hold down now. */
                if (ev.value == EVENT_VALUE_KEYDOWN && (!key_ismod(ev.code) || !v->tap_mods)) {
                    dbgprintf("Tap rule #%d: Act as hold key.", i);
                    v->act_key = v->hold_key;
                    /* If `hold_key` was pressed in advance, we don't have to
                     * press it again. */
                    if (!key_ismod(v->hold_key))
                        write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                }
            }
        }

        for (i = 0; i < ARRAY_LEN(MULTI_RULES); ++i) {
            struct multi_rule_info *const v = &MULTI_RULES[i];
            int j, ndown = 0, ntotal;
            int key_matches = 0;
            int nkeys;

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
            if (!key_matches)
                continue;

            ntotal = j;

            if (!v->can_toggle) {
                nkeys = (v->is_down ? v->nbeforeup : v->nbeforedown);
                v->can_toggle = (nkeys >= 0 ? ndown == nkeys : ndown != -nkeys);
            }

            if (v->can_toggle && (!v->is_down
                        ? ndown == ntotal
                        : (v->nup >= 0 ? ndown == v->nup : ndown != -v->nup))) {
                int press[2];

                v->is_down ^= 1;
                memcpy(press, v->is_down ? v->down_press : v->up_press, sizeof press);

                nkeys = (v->is_down ? v->nbeforeup : v->nbeforedown);
                v->can_toggle = (nkeys >= 0 ? ndown == nkeys : ndown != -nkeys);

                dbgprintf("Multi rule #%d: Toggled %s now.", i, (v->is_down ? "down" : "up"));

                if (!v->is_down) {
                    if (press[0] != KEY_RESERVED)
                        write_key_event(press[0], EVENT_VALUE_KEYDOWN);

                    if (press[1] != KEY_RESERVED)
                        write_key_event(press[1], EVENT_VALUE_KEYUP);
                }

                for (j = 0; j < ntotal; ++j) {
                    if ((v->keys_down >> j) & 1) {
                        /* Do not send release event if we will press it immediately (and vica-versa). */
                        if (press[!v->is_down] == v->keys[j]) {
                            press[!v->is_down] = KEY_RESERVED;
                            continue;
                        }

                        write_key_event(v->keys[j], (v->is_down ? EVENT_VALUE_KEYUP : EVENT_VALUE_KEYDOWN));
                    }
                }

                if (v->is_down) {
                    if (press[0] != KEY_RESERVED)
                        write_key_event(press[0], EVENT_VALUE_KEYDOWN);

                    if (press[1] != KEY_RESERVED)
                        write_key_event(press[1], EVENT_VALUE_KEYUP);
                }

                goto ignore_event;
            } else if (v->is_down
                    && ev.code == v->repeated_key
                    && v->down_press[0] != KEY_RESERVED && v->down_press[1] == KEY_RESERVED
                    && v->up_press[0]   == KEY_RESERVED && v->up_press[1]   == v->down_press[0]) {
                dbgprintf("Multi rule #%d: Repeat.", i);
                ev.code = v->down_press[0];
                break;
            } else if (v->is_down) {
                dbgprintf("Multi rule #%d: Ignore matched key.", i);
                goto ignore_event;
            } else if (ndown > 0 || v->can_toggle) {
                dbgprintf("Multi rule #%d: Toggled %s%s. %d down.",
                        i,
                        (v->is_down ? "down" : "up"),
                        (v->can_toggle ? ", can toggle" : ""),
                        ndown);
            }
        }

    write:
        write_event(&ev);
    ignore_event:;
    }
}
/* vi:set ft=c: */
