#define _XOPEN_SOURCE 500
#ifdef VERBOSE
# include <stdio.h> /* fprintf() */
#endif
#include <stdlib.h> /* EXIT_FAILURE */
#include <errno.h> /* errno */
#include <unistd.h> /* STD*_FILENO, read() */
#include <string.h> /* memcpy() */
#include <linux/input.h> /* KEY_*, struct input_event */
#include <time.h> /* CLOCK_*, clock_gettime() */
#include <limits.h> /* INT_MAX */

/* Config {{{1 */
/* Global config. */
#include "config.h"

#ifndef TYPING_TIMEOUT_MSEC
/* TODO: Find a better name. */
/* What is the maximum time interval between two consecutive key presses that
 * we still consider typing. */
# define TYPING_TIMEOUT_MSEC 192
#endif

#ifndef MAX_EVENTS
/* How many events to buffer internally.
 *
 * Note that it doesn't introduce any delays, just aims reducing the number of
 * read(2)s and write(2)s.
 * */
# define MAX_EVENTS 10
#endif

/* KEY_* codes: /usr/include/linux/input-event-codes.h */

/** Map a key to another. */
static struct map_rule {
    int const from_key; /** Map what? */
    int const to_key; /** To what? */
} MAP_RULES[] = {
#include "map-rules.h.in"
};

/** Bind multiple functions to a single key. */
static struct tap_rule {
    int const base_key; /** Key to override. */
    int const tap_key; /** Act as this key when pressed alone. */
    int const hold_key; /** Act as this key when `base_key` pressed with
                          `action_key`. */
    int const repeat_key; /** Act as this key when pressed alone for longer
                            time. Optional. */
    int const repeat_delay; /** Wait this much repeat events to arrive after
                              acting as repeat key. */
    int const tap_mods; /** Whether to modifier keys apply to `tap_key`. */
    int const action_key; /** See `hold_key`.
                            If unspecified `action_key` means any key. */
    int const hold_immediately: 1; /** Press `hold_key` immediately and release
                                     only if it needs to act as `tap_key` or
                                     `repeat_key`.
                                     If you use it (=1) when `hold_key` is a
                                     modifier, you can achieve modifier+mouse
                                     without any delays. */
    int const tap_typing: 1; /** Unconditionally act as `tap_key` while typing.
                               (Disables `hold_key` and `repeat_key`.) */

    int was_held: 1;
    int act_key; /** How `base_key` acts as actually. */
    /*
     * Special values:
     * - `-1`: Waiting.
     * - `KEY_RESERVED`: Idle.
     **/
    int curr_delay; /** Internal counter for `repeat_delay`. */
} TAP_RULES[] = {
#define TAP(key) .base_key = (key), .tap_key = (key)
#include "tap-rules.h.in"
#undef TAP
};

/** Bind actions to multiple keys.
 *
 * Take care of `down_press` and `up_press` to be balanced.
 */
static struct multi_rule {
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
#define KEY_PAIR(key) { KEY_LEFT##key, KEY_RIGHT##key }
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
#undef KEY_PAIR
};
/* 1}}} */

#define ARRAY_LEN(a) (int)(sizeof(a) / sizeof(*a))

#ifdef VERBOSE
# define dbgprintf(msg, ...) fprintf(stderr, msg "\n", ##__VA_ARGS__)
#else
# define dbgprintf(msg, ...) ((void)0)
#endif

#define SEC_TO_NSEC_APPROX  (1LL << 30)
#define MSEC_TO_NSEC_APPROX (1LL << 20)
#define TV_TO_NSEC(tv) ((tv).tv_sec * SEC_TO_NSEC_APPROX + (tv).tv_nsec)

#ifdef CLOCK_MONOTONIC_COARSE
# define TYPING_CLOCK_SOURCE CLOCK_MONOTONIC_COARSE
#else
# define TYPING_CLOCK_SOURCE CLOCK_MONOTONIC
#endif

enum event_values {
    EVENT_VALUE_KEYUP = 0,
    EVENT_VALUE_KEYDOWN = 1,
    EVENT_VALUE_KEYREPEAT = 2,
};

static struct input_event revbuf[MAX_EVENTS];
static size_t revlen = 0;
static size_t riev = 0;
static struct input_event wevbuf[MAX_EVENTS];
static size_t wevlen = 0;
static int is_typing = 0;
static struct timespec last_typing;
static unsigned char matrix[KEY_CNT] = {EVENT_VALUE_KEYUP/*Shitty hack!*/};
/* HACK: Keycodes assumed to be fit in `unsigned char`. */
static unsigned char matrix_aliases[KEY_CNT] = {
    [KEY_LEFTSHIFT]  = KEY_RIGHTSHIFT,
    [KEY_RIGHTSHIFT] = KEY_LEFTSHIFT,
    [KEY_LEFTCTRL]   = KEY_RIGHTCTRL,
    [KEY_RIGHTCTRL]  = KEY_LEFTCTRL,
    [KEY_LEFTALT]    = KEY_RIGHTALT,
    [KEY_RIGHTALT]   = KEY_LEFTALT,
    [KEY_LEFTMETA]   = KEY_RIGHTMETA,
    [KEY_RIGHTMETA]  = KEY_LEFTMETA,
};

__attribute__((const))
static int
key_ismod(int code) {
    switch (code) {
    default:
        return 0;
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
    case KEY_LEFTCTRL:  case KEY_RIGHTCTRL:
    case KEY_LEFTALT:   case KEY_RIGHTALT:
    case KEY_LEFTMETA:  case KEY_RIGHTMETA:
        return 1;
    }
}

static void
flush_events(void) {
    if (wevlen == 0)
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

__attribute__((const))
static int
should_check_typing(void) {
    int i;

    for (i = 0; i < ARRAY_LEN(TAP_RULES); ++i) {
        struct tap_rule *const v = &TAP_RULES[i];
        if (v->tap_typing)
            return 1;
    }
    return 0;
}

static void
write_event(struct input_event const *e) {
    if (e->type == EV_KEY) {
        matrix[e->code] = e->value;
        if (should_check_typing()) {
            if (!is_typing && e->value == EVENT_VALUE_KEYUP && !key_ismod(e->code)) {
                is_typing = 1;
                clock_gettime(TYPING_CLOCK_SOURCE, &last_typing);
                dbgprintf("Typing: Yes.");
            }
        }
    }

    wevbuf[wevlen++] = *e;
    if (wevlen == MAX_EVENTS)
        flush_events();
}

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
write_key_event(int code, int value) {
    struct input_event e = {
        .type = EV_KEY,
        .code = code,
        .value = value
    };
    write_event(&e);
}

static int
matrix_iskeydown(int code) {
    return matrix[code] != EVENT_VALUE_KEYUP
        || matrix[matrix_aliases[code]] != EVENT_VALUE_KEYUP;
}

int
main(void) {
    for (;;) {
        int i;
        struct input_event e;
        int ignore = 0;

        /* No more input event to read from the buffer. */
        if (riev == revlen) {
            flush_events();
            read_events();
        }
        e = revbuf[riev++];

        if (e.type != EV_KEY) {
            /* We don't care about scan codes. */
            if (e.type == EV_MSC && e.code == MSC_SCAN)
                goto ignore_event;
            goto write;
        }

#if 0
        dbgprintf("Code: %3d Value: %d", e.code, e.value);
#endif

        for (i = 0; i < ARRAY_LEN(MAP_RULES); ++i) {
            struct map_rule *const v = &MAP_RULES[i];
            if (e.code == v->from_key) {
                if (v->to_key != KEY_RESERVED) {
                    dbgprintf("Map rule #%d: %d -> %d.", i, e.code, v->to_key);
                    e.code = v->to_key;
                    break;
                } else {
                    dbgprintf("Map rule #%d: %d -> (ignore).", i, e.code);
                    goto ignore_event;
                }
            }
        }

        /* Check if user is typing. */
        if (should_check_typing()) {
            if (is_typing && e.value != EVENT_VALUE_KEYUP) {
                struct timespec now;
                clock_gettime(TYPING_CLOCK_SOURCE, &now);
                time_t const elapsed_msec = (TV_TO_NSEC(now) - TV_TO_NSEC(last_typing)) / MSEC_TO_NSEC_APPROX;
                memcpy(&last_typing, &now, sizeof last_typing);
                is_typing = (elapsed_msec <= TYPING_TIMEOUT_MSEC);
                if (!is_typing)
                    dbgprintf("Typing: No; elapsed: %ld ms.", elapsed_msec);
            }
        }

        for (i = 0; i < ARRAY_LEN(TAP_RULES); ++i) {
            struct tap_rule *const v = &TAP_RULES[i];

            if (e.code == v->base_key) {
                switch (e.value) {
                case EVENT_VALUE_KEYDOWN:
                    if (v->act_key == KEY_RESERVED) {
                        v->was_held = 0;
                        if ((is_typing && v->tap_typing) || matrix_iskeydown(v->hold_key)) {
                            dbgprintf("Tap rule #%d: Tapped immediately.", i);
                            v->act_key = v->tap_key;
                            write_key_event(v->tap_key, EVENT_VALUE_KEYDOWN);
                        } else {
                        tap_rearm:
                            dbgprintf("Tap rule #%d: Armed.", i);
                            v->act_key = -1;
                            /* A hold modifier keys can be pressed now and released
                             * if need to act as tap key in the future. */
                            if (v->hold_immediately)
                                write_key_event(v->hold_key, EVENT_VALUE_KEYDOWN);
                            v->curr_delay = v->repeat_delay;
                        }
                    }
                    ignore = 1;
                    continue;
                case EVENT_VALUE_KEYREPEAT:
                    switch (v->act_key) {
                    case KEY_RESERVED:
                        /* Do nothing. */
                        break;
                    case -1:
                        /* Do not repeat this key. */
                        if (v->repeat_key == KEY_RESERVED) {
                            ignore = 1;
                            continue;
                        }

                        /* Wait for more key repeats. */
                        if (v->curr_delay-- > 0) {
                            ignore = 1;
                            continue;
                        }

                        /* Timeout reached, act as repeat key. */
                        dbgprintf("Tap rule #%d: Repeated.", i);
                        if (v->hold_immediately)
                            write_key_event(v->hold_key, EVENT_VALUE_KEYUP);
                        v->act_key = v->repeat_key;
                        write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                        break;
                    default:
                        ignore = 1;
                        write_key_event(v->act_key, EVENT_VALUE_KEYREPEAT);
                        break;
                    }
                    break;
                case EVENT_VALUE_KEYUP:
                    switch (v->act_key) {
                    case KEY_RESERVED:
                        /* Do nothing. */
                        break;
                    case -1:
                        /* We've been already hold down with other keys, so we
                         * mustn't tap now. */
                        if (!v->was_held) {
                            int j;
                            for (j = i; j < ARRAY_LEN(TAP_RULES); ++j) {
                                struct tap_rule *const w = &TAP_RULES[j];
                                if (w->base_key == v->base_key
                                    && w->tap_key == v->tap_key)
                                    w->was_held = 1;
                            }
                            /* We aren't up until now how this key should act. */
                            dbgprintf("Tap rule #%d: Tapped.", i);
                            v->act_key = v->tap_key;
                            if (v->hold_immediately)
                                write_key_event(v->hold_key, EVENT_VALUE_KEYUP);
                            write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                        } else {
                            dbgprintf("Tap rule #%d: Tap ignored.", i);
                            /* Fall through. */
                    default:
                            if (v->action_key != KEY_RESERVED && v->act_key == v->hold_key) {
                                dbgprintf("Tap rule #%d: Action key up.", i);
                                write_key_event(v->action_key, EVENT_VALUE_KEYDOWN);
                            }
                        }

                        dbgprintf("Tap rule #%d: Up.", i);
                        ignore = 1;
                        if (v->act_key != -1)
                            write_key_event(v->act_key, EVENT_VALUE_KEYUP);
                        v->act_key = KEY_RESERVED;
                        break;
                    }
                    break;
                }
            } else if (v->act_key == -1
                    && e.value == EVENT_VALUE_KEYDOWN
                    && (v->action_key == KEY_RESERVED
                        || (e.code == v->action_key && (!key_ismod(e.code) || !v->tap_mods)))) {
                if (v->action_key != KEY_RESERVED)
                    ignore = 1;
                /* User started typing meanwhile. */
                if ((is_typing && v->tap_typing) && !v->was_held) {
                    dbgprintf("Tap rule #%d: Late tap.", i);
                    v->act_key = v->tap_key;
                    write_key_event(v->tap_key, EVENT_VALUE_KEYDOWN);
                } else {
                    int j;
                    dbgprintf("Tap rule #%d: Held.", i);
                    v->act_key = v->hold_key;
                    /* v->was_held = 1; */
                    for (j = 0; j < ARRAY_LEN(TAP_RULES); ++j) {
                        struct tap_rule *const w = &TAP_RULES[j];
                        if (w->base_key == v->base_key
                            && w->tap_key == v->tap_key)
                            w->was_held = 1;
                    }
                    /* If `hold_key` was pressed in advance, we don't have to
                     * press it again. */
                    if (!v->hold_immediately)
                        write_key_event(v->act_key, EVENT_VALUE_KEYDOWN);
                }
            } else if (v->act_key > 0 && v->action_key != KEY_RESERVED) {
                if (e.value == EVENT_VALUE_KEYUP) {
                    dbgprintf("Tap rule #%d: Dearm.", i);
                    write_key_event(v->act_key, EVENT_VALUE_KEYUP);
                    goto tap_rearm;
                } else {
                    dbgprintf("Tap rule #%d: Action key ignored.", i);
                    ignore = 1;
                }
            }
        }
        if (ignore)
            goto ignore_event;

        for (i = 0; i < ARRAY_LEN(MULTI_RULES); ++i) {
            struct multi_rule *const v = &MULTI_RULES[i];
            int j, ndown = 0, ntotal;
            int key_matches = 0;
            int nkeys;

            for (j = 0; j < ARRAY_LEN(v->keys) && v->keys[j] != KEY_RESERVED; ++j) {
                if (e.code == v->keys[j]) {
                    key_matches = 1;
                    if (v->repeated_key == e.code)
                        v->repeated_key = KEY_RESERVED;

                    switch (e.value) {
                    case EVENT_VALUE_KEYUP:
                        v->keys_down &= ~(1 << j);
                        break;
                    case EVENT_VALUE_KEYREPEAT:
                        if (v->repeated_key == KEY_RESERVED || v->repeated_key == e.code) {
                            v->repeated_key_repeated = 1;
                            v->repeated_key = e.code;
                        } else if (!v->repeated_key_repeated && v->repeating_key == e.code) {
                            v->repeated_key_repeated = 1;
                            v->repeated_key = e.code;
                            dbgprintf("Multi rule #%d: Repeating key changed.", i);
                        } else {
                            v->repeated_key_repeated = 0;
                            v->repeating_key = e.code;
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

                dbgprintf("Multi rule #%d: %s now.", i, (v->is_down ? "Down" : "Up"));

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

                ignore = 1;
                continue;
            } else if (v->is_down
                    && e.code == v->repeated_key
                    && v->down_press[0] != KEY_RESERVED && v->down_press[1] == KEY_RESERVED
                    && v->up_press[0]   == KEY_RESERVED && v->up_press[1]   == v->down_press[0]) {
                dbgprintf("Multi rule #%d: Repeated.", i);
                e.code = v->down_press[0];
                break;
            } else if (v->is_down) {
                dbgprintf("Multi rule #%d: Ignored matched key.", i);
                ignore = 1;
                continue;
            }
        }
        if (ignore)
            goto ignore_event;

    write:
        write_event(&e);
    ignore_event:;
    }
}
/* vi:set ft=c: */
