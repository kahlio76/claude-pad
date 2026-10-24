#ifndef PAD_FW_H
#define PAD_FW_H

/* Pico-side logic for Claude Pad, with no hardware in it.
 *
 * The Pico does three small jobs, and each one lives here so it can be tested
 * on a laptop without a board:
 *
 *   1. Debounce raw switch readings into clean press/release edges.
 *   2. Speak the line protocol the Mac daemon (padd) expects:
 *        pad -> Mac   "D Approve\n"  /  "U Approve\n"
 *        Mac -> pad   "L wait work off off 1\n"
 *   3. Turn each slot's state into an LED color.
 *
 * Key names and numbering come from pad_core.h, the same header the daemon
 * uses, so the pad and the daemon cannot drift apart on what a key is called.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pad_core.h"

/* ---- 1. Debounce ------------------------------------------------------- */

/* A switch must read differently from its settled state for this many samples
 * in a row before the change counts. Sampled at 1 kHz that is 5 ms, long
 * enough to ride out contact bounce, short enough that nobody can feel it. */
enum { PADFW_DEBOUNCE_SAMPLES = 5 };

typedef struct {
    PadKey key;
    bool down; /* true = pressed, false = released */
} PadFwEdge;

typedef struct {
    uint16_t settled;                  /* bit i set = key i is settled as pressed */
    uint8_t differing[PAD_KEY_COUNT];  /* consecutive samples that disagree with `settled` */
} PadFwDebounce;

void padfw_debounce_init(PadFwDebounce *d);

/* Feed one sample. Bit i of `raw` is set while key i is physically pressed.
 * Writes any edges that just settled into `out` (room for PAD_KEY_COUNT) and
 * returns how many. When several keys change in the same sample, FN comes
 * first, because pad_core needs FN to be down before the key it modifies. */
int padfw_debounce_step(PadFwDebounce *d, uint16_t raw, PadFwEdge *out);

/* ---- 2. Line protocol -------------------------------------------------- */

/* Write "D <Key>\n" or "U <Key>\n" into buf. Returns the length, or -1 if the
 * key is invalid or the buffer is too small. buf is always NUL-terminated on
 * success. */
int padfw_format_edge(char *buf, size_t cap, PadKey key, bool down);

typedef enum { PADFW_OFF, PADFW_IDLE, PADFW_WORK, PADFW_WAIT, PADFW_DONE } PadFwLed;

typedef struct {
    PadFwLed slot[PAD_SLOTS];
    int selected; /* 0-based, unlike the 1-based number on the wire */
} PadFwLeds;

/* Parse "L <s1> <s2> <s3> <s4> <selected>" (no newline). On success fills
 * `out` and returns true. On anything malformed returns false and leaves
 * `out` untouched, so a garbled line can never blank the LEDs. */
bool padfw_parse_leds(const char *line, PadFwLeds *out);

/* Collects serial bytes into lines. */
enum { PADFW_LINE_MAX = 48 };
typedef struct {
    char text[PADFW_LINE_MAX];
    size_t len;
    bool overflow; /* current line was too long; throw it away at its newline */
    bool ready;    /* a finished line is waiting in `text` */
} PadFwLine;

void padfw_line_init(PadFwLine *l);

/* Feed one byte. Returns true when `text` now holds a complete line (without
 * its newline, NUL-terminated). Read it before feeding the next byte. */
bool padfw_line_feed(PadFwLine *l, char c);

/* ---- 3. LED colors ----------------------------------------------------- */

typedef struct {
    uint8_t r, g, b;
} PadFwRgb;

/* Color for one slot at time `now_ms`.
 *   idle  dim white      work  amber      done  green
 *   wait  red, blinking at 2 Hz, always full brightness so it can't be missed
 * The selected slot is shown at full brightness, the others dimmed. */
PadFwRgb padfw_led_color(PadFwLed state, bool selected, uint32_t now_ms);

#endif /* PAD_FW_H */
