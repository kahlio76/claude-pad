#include "pad_fw.h"

#include <string.h>

/* ---- 1. Debounce ------------------------------------------------------- */

void padfw_debounce_init(PadFwDebounce *d) {
    memset(d, 0, sizeof(*d));
}

int padfw_debounce_step(PadFwDebounce *d, uint16_t raw, PadFwEdge *out) {
    int n = 0;
    /* Visit FN first, then S1..Stop, so a simultaneous FN + key press reaches
     * pad_core in the order it requires. */
    for (int j = 0; j < PAD_KEY_COUNT; ++j) {
        int i = (j == 0) ? PAD_FN : j - 1;
        bool reads_down = ((raw >> i) & 1u) != 0;
        bool is_down = ((d->settled >> i) & 1u) != 0;

        if (reads_down == is_down) {
            d->differing[i] = 0; /* any bounce back to the settled value starts over */
            continue;
        }
        if (++d->differing[i] >= PADFW_DEBOUNCE_SAMPLES) {
            d->settled = (uint16_t)(d->settled ^ (1u << i));
            d->differing[i] = 0;
            out[n].key = (PadKey)i;
            out[n].down = reads_down;
            ++n;
        }
    }
    return n;
}

/* ---- 2. Line protocol -------------------------------------------------- */

int padfw_format_edge(char *buf, size_t cap, PadKey key, bool down) {
    if ((unsigned)key >= (unsigned)PAD_KEY_COUNT) /* also catches negative values */
        return -1;
    const char *name = pad_key_name(key);
    size_t name_len = strlen(name);
    size_t total = 2 + name_len + 1; /* "D " + name + "\n" */
    if (total + 1 > cap)             /* +1 for the NUL */
        return -1;
    buf[0] = down ? 'D' : 'U';
    buf[1] = ' ';
    memcpy(buf + 2, name, name_len);
    buf[2 + name_len] = '\n';
    buf[total] = '\0';
    return (int)total;
}

static bool state_from_word(const char *w, size_t len, PadFwLed *out) {
    static const struct {
        const char *name;
        PadFwLed value;
    } states[] = {
        {"off", PADFW_OFF}, {"idle", PADFW_IDLE}, {"work", PADFW_WORK},
        {"wait", PADFW_WAIT}, {"done", PADFW_DONE},
    };
    for (size_t i = 0; i < sizeof states / sizeof states[0]; ++i) {
        if (strlen(states[i].name) == len && memcmp(states[i].name, w, len) == 0) {
            *out = states[i].value;
            return true;
        }
    }
    return false;
}

bool padfw_parse_leds(const char *line, PadFwLeds *out) {
    if (!line || !out || line[0] != 'L' || line[1] != ' ')
        return false;

    PadFwLeds parsed;
    const char *p = line + 2;
    for (int i = 0; i < PAD_SLOTS; ++i) {
        const char *space = strchr(p, ' ');
        if (!space || space == p)
            return false;
        if (!state_from_word(p, (size_t)(space - p), &parsed.slot[i]))
            return false;
        p = space + 1;
    }
    /* What's left must be exactly one digit, 1 to PAD_SLOTS. */
    if (p[0] < '1' || p[0] > '0' + PAD_SLOTS || p[1] != '\0')
        return false;
    parsed.selected = p[0] - '1';

    *out = parsed; /* only touch the caller's copy once the whole line checked out */
    return true;
}

void padfw_line_init(PadFwLine *l) {
    l->len = 0;
    l->overflow = false;
    l->ready = false;
    l->text[0] = '\0';
}

bool padfw_line_feed(PadFwLine *l, char c) {
    if (l->ready) { /* the caller has had its chance to read the last line */
        l->len = 0;
        l->overflow = false;
        l->ready = false;
    }
    if (c == '\r')
        return false;
    if (c == '\n') {
        if (l->overflow) { /* too long to be ours: drop it, start fresh */
            l->len = 0;
            l->overflow = false;
            return false;
        }
        if (l->len == 0)
            return false;
        l->text[l->len] = '\0';
        l->ready = true;
        return true;
    }
    if (l->len + 1 < sizeof l->text)
        l->text[l->len++] = c;
    else
        l->overflow = true;
    return false;
}

/* ---- 3. LED colors ----------------------------------------------------- */

/* WS2812s are painfully bright at full power, so the brightest channel here
 * is 80 out of 255. */
static uint8_t scale(uint8_t v, unsigned level) {
    return (uint8_t)((v * level) / 255u);
}

PadFwRgb padfw_led_color(PadFwLed state, bool selected, uint32_t now_ms) {
    PadFwRgb c = {0, 0, 0};
    bool must_be_seen = false;

    switch (state) {
    case PADFW_IDLE:
        c = (PadFwRgb){80, 80, 80};
        break;
    case PADFW_WORK:
        c = (PadFwRgb){80, 40, 0};
        break;
    case PADFW_DONE:
        c = (PadFwRgb){0, 80, 0};
        break;
    case PADFW_WAIT:
        /* Blink between full red and a dim ember, never fully dark, so a
         * request waiting on a slot you aren't looking at still stands out. */
        c = ((now_ms / 250u) % 2u == 0) ? (PadFwRgb){80, 0, 0} : (PadFwRgb){10, 0, 0};
        must_be_seen = true;
        break;
    case PADFW_OFF:
    default:
        return c;
    }

    unsigned level = (selected || must_be_seen) ? 255u : 70u;
    c.r = scale(c.r, level);
    c.g = scale(c.g, level);
    c.b = scale(c.b, level);
    return c;
}
