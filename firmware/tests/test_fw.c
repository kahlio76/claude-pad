/* Tests for the Pico-side logic (pad_fw.c). Build and run from the repo root:
 *   make fw-test
 * No board needed: everything here is plain C on your laptop. */
#include "pad_fw.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(name, cond)                                      \
    do {                                                       \
        int ok_ = (cond) ? 1 : 0;                              \
        printf("%s  %s\n", ok_ ? "pass" : "FAIL", name);       \
        if (!ok_)                                              \
            failures++;                                        \
    } while (0)

#define BIT(k) ((uint16_t)(1u << (k)))

/* Feed `raw` for `n` samples; return how many edges came out in total and
 * keep the last one in *last. */
static int hold(PadFwDebounce *d, uint16_t raw, int n, PadFwEdge *last) {
    int total = 0;
    for (int i = 0; i < n; ++i) {
        PadFwEdge e[PAD_KEY_COUNT];
        int got = padfw_debounce_step(d, raw, e);
        total += got;
        if (got && last)
            *last = e[got - 1];
    }
    return total;
}

static void test_debounce(void) {
    PadFwDebounce d;
    PadFwEdge e = {PAD_S1, false};

    padfw_debounce_init(&d);
    CHECK("a press does not register before 5 samples", hold(&d, BIT(PAD_APPROVE), 4, &e) == 0);
    CHECK("the 5th sample registers the press", hold(&d, BIT(PAD_APPROVE), 1, &e) == 1 &&
                                                    e.key == PAD_APPROVE && e.down);
    CHECK("holding the key adds no more edges", hold(&d, BIT(PAD_APPROVE), 50, &e) == 0);
    CHECK("a release needs 5 samples too", hold(&d, 0, 4, &e) == 0);
    CHECK("the 5th sample registers the release", hold(&d, 0, 1, &e) == 1 &&
                                                      e.key == PAD_APPROVE && !e.down);

    /* Contact bounce: chatter, then a solid press. Exactly one edge, and only
     * once the press has been steady for the full window. */
    padfw_debounce_init(&d);
    int edges = 0;
    const uint16_t chatter[] = {1, 0, 1, 0, 0, 1, 1, 0, 1};
    for (size_t i = 0; i < sizeof chatter / sizeof chatter[0]; ++i)
        edges += hold(&d, chatter[i] ? BIT(PAD_DENY) : 0, 1, NULL);
    CHECK("bouncing contacts produce no edge while they chatter", edges == 0);
    CHECK("then one clean edge once steady", hold(&d, BIT(PAD_DENY), 5, &e) == 1 && e.down);

    /* A 4-sample glitch that returns to idle must vanish without a trace. */
    padfw_debounce_init(&d);
    edges = hold(&d, BIT(PAD_STOP), 4, NULL) + hold(&d, 0, 20, NULL);
    CHECK("a 4-sample glitch is ignored entirely", edges == 0);

    /* Two keys settle independently. */
    padfw_debounce_init(&d);
    hold(&d, BIT(PAD_S1), 3, NULL);
    int got = hold(&d, BIT(PAD_S1) | BIT(PAD_S2), 2, &e); /* S1 hits 5 here, S2 only 2 */
    CHECK("keys debounce independently", got == 1 && e.key == PAD_S1);
}

static void test_fn_first(void) {
    PadFwDebounce d;
    PadFwEdge e[PAD_KEY_COUNT];
    padfw_debounce_init(&d);

    /* FN and Stop go down in the same sample window. pad_core needs FN first,
     * but Stop sits before FN in the key list, so a plain 0..N scan would
     * emit Stop first and FN + Stop would be read as a bare Stop (interrupt)
     * instead of a rewind. */
    int n = 0;
    for (int i = 0; i < PADFW_DEBOUNCE_SAMPLES; ++i)
        n = padfw_debounce_step(&d, BIT(PAD_STOP) | BIT(PAD_FN), e);
    CHECK("simultaneous FN + Stop emits two edges", n == 2);
    CHECK("FN comes first", e[0].key == PAD_FN && e[0].down);
    CHECK("then the key it modifies", e[1].key == PAD_STOP && e[1].down);
}

static void test_format(void) {
    char buf[32];
    CHECK("press line", padfw_format_edge(buf, sizeof buf, PAD_APPROVE, true) == 10 &&
                            strcmp(buf, "D Approve\n") == 0);
    CHECK("release line", padfw_format_edge(buf, sizeof buf, PAD_FN, false) == 5 &&
                              strcmp(buf, "U FN\n") == 0);
    CHECK("S1 line", padfw_format_edge(buf, sizeof buf, PAD_S1, true) == 5 &&
                         strcmp(buf, "D S1\n") == 0);

    /* Every key's line must be accepted by the real parser pad_core exposes
     * and by padd, which uses pad_key_parse on the part after "D "/"U ". */
    int all_roundtrip = 1;
    for (int k = 0; k < PAD_KEY_COUNT; ++k) {
        padfw_format_edge(buf, sizeof buf, (PadKey)k, true);
        char name[16];
        size_t n = strlen(buf) - 3; /* strip "D " and "\n" */
        memcpy(name, buf + 2, n);
        name[n] = '\0';
        if (pad_key_parse(name) != k)
            all_roundtrip = 0;
    }
    CHECK("every key's name round-trips through pad_key_parse", all_roundtrip);

    CHECK("a too-small buffer is refused", padfw_format_edge(buf, 9, PAD_APPROVE, true) == -1);
    CHECK("exactly-fitting buffer works", padfw_format_edge(buf, 11, PAD_APPROVE, true) == 10);
    CHECK("an invalid key is refused", padfw_format_edge(buf, sizeof buf, (PadKey)99, true) == -1);
}

static void test_parse(void) {
    PadFwLeds l = {{PADFW_DONE, PADFW_DONE, PADFW_DONE, PADFW_DONE}, 3};

    CHECK("parses a real line", padfw_parse_leds("L wait work off off 1", &l) &&
                                    l.slot[0] == PADFW_WAIT && l.slot[1] == PADFW_WORK &&
                                    l.slot[2] == PADFW_OFF && l.slot[3] == PADFW_OFF &&
                                    l.selected == 0);
    CHECK("selected is converted to 0-based", padfw_parse_leds("L idle idle idle done 4", &l) &&
                                                   l.selected == 3 && l.slot[3] == PADFW_DONE);

    /* The exact lines padd sends: "L \(states.joined(" ")) \(selected + 1)\n"
     * with states drawn from off|idle|work|wait|done. */
    CHECK("empty daemon line", padfw_parse_leds("L off off off off 1", &l) && l.slot[0] == PADFW_OFF);
    CHECK("busy daemon line", padfw_parse_leds("L work wait off done 2", &l) &&
                                   l.slot[3] == PADFW_DONE && l.selected == 1);

    /* Garbage must be refused, and must not disturb what's already shown. */
    PadFwLeds keep = {{PADFW_WORK, PADFW_WAIT, PADFW_OFF, PADFW_IDLE}, 2};
    l = keep;
    const char *bad[] = {
        "",                          "L",
        "L wait work off off",       /* no selected slot */
        "L wait work off 1",         /* only three states */
        "L wait work off off off 1", /* five states */
        "L wait work off off 0",     /* slot numbers start at 1 */
        "L wait work off off 5",     /* ...and stop at 4 */
        "L wait work off off 12",    /* one digit only */
        "L wait work bogus off 1",   /* unknown state */
        "L WAIT work off off 1",     /* case matters */
        "X wait work off off 1",     /* wrong prefix */
        "L  wait work off off 1",    /* double space */
        "L wait work off off 1 ",    /* trailing junk */
        "D Approve",                 /* a line meant for the other direction */
    };
    int rejected = 1, untouched = 1;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        if (padfw_parse_leds(bad[i], &l))
            rejected = 0;
        if (memcmp(&l, &keep, sizeof l) != 0)
            untouched = 0;
    }
    CHECK("malformed lines are all rejected", rejected);
    CHECK("a rejected line leaves the LEDs as they were", untouched);
    CHECK("NULL arguments are refused", !padfw_parse_leds(NULL, &l) && !padfw_parse_leds("L off off off off 1", NULL));
}

static int feed_all(PadFwLine *l, const char *s, char lines[][PADFW_LINE_MAX], int max) {
    int n = 0;
    for (; *s; ++s)
        if (padfw_line_feed(l, *s) && n < max)
            strcpy(lines[n++], l->text);
    return n;
}

static void test_lines(void) {
    PadFwLine l;
    char out[4][PADFW_LINE_MAX];

    padfw_line_init(&l);
    CHECK("one line", feed_all(&l, "L off off off off 1\n", out, 4) == 1 &&
                          strcmp(out[0], "L off off off off 1") == 0);

    CHECK("CRLF is handled", feed_all(&l, "L idle off off off 2\r\n", out, 4) == 1 &&
                                 strcmp(out[0], "L idle off off off 2") == 0);

    CHECK("two lines in one burst", feed_all(&l, "L off off off off 1\nL work off off off 1\n", out, 4) == 2 &&
                                        strcmp(out[1], "L work off off off 1") == 0);

    CHECK("a line split across reads", feed_all(&l, "L wa", out, 4) == 0 &&
                                           feed_all(&l, "it off off off 3\n", out, 4) == 1 &&
                                           strcmp(out[0], "L wait off off off 3") == 0);

    CHECK("blank lines are skipped", feed_all(&l, "\n\r\n\n", out, 4) == 0);

    /* A runaway line is dropped whole, and the next good line still works. */
    char junk[200];
    memset(junk, 'x', sizeof junk - 2);
    junk[sizeof junk - 2] = '\n';
    junk[sizeof junk - 1] = '\0';
    CHECK("an overlong line is discarded", feed_all(&l, junk, out, 4) == 0);
    CHECK("and the next line is clean", feed_all(&l, "L done off off off 1\n", out, 4) == 1 &&
                                            strcmp(out[0], "L done off off off 1") == 0);
}

static void test_colors(void) {
    PadFwRgb c;

    c = padfw_led_color(PADFW_OFF, true, 0);
    CHECK("off is dark even when selected", c.r == 0 && c.g == 0 && c.b == 0);

    c = padfw_led_color(PADFW_DONE, true, 0);
    CHECK("done is green", c.g > 0 && c.r == 0 && c.b == 0);
    c = padfw_led_color(PADFW_WORK, true, 0);
    CHECK("work is amber", c.r > c.g && c.g > 0 && c.b == 0);

    PadFwRgb sel = padfw_led_color(PADFW_WORK, true, 0);
    PadFwRgb dim = padfw_led_color(PADFW_WORK, false, 0);
    CHECK("the selected slot is brighter than the others", sel.r > dim.r && dim.r > 0);

    PadFwRgb on = padfw_led_color(PADFW_WAIT, false, 0);
    PadFwRgb off = padfw_led_color(PADFW_WAIT, false, 250);
    CHECK("waiting blinks red", on.r > off.r && on.g == 0 && on.b == 0 && off.r > 0);
    CHECK("the blink period is 500 ms", padfw_led_color(PADFW_WAIT, false, 500).r == on.r);

    PadFwRgb wait_dim = padfw_led_color(PADFW_WAIT, false, 0);
    PadFwRgb wait_sel = padfw_led_color(PADFW_WAIT, true, 0);
    CHECK("a request waiting on an unselected slot is still full brightness",
          wait_dim.r == wait_sel.r);

    /* Never drive a channel past the intended ceiling of 80. */
    int ceiling_ok = 1;
    for (int s = PADFW_OFF; s <= PADFW_DONE; ++s)
        for (uint32_t t = 0; t < 1000; t += 50) {
            PadFwRgb x = padfw_led_color((PadFwLed)s, true, t);
            if (x.r > 80 || x.g > 80 || x.b > 80)
                ceiling_ok = 0;
        }
    CHECK("no channel ever exceeds 80/255", ceiling_ok);

    /* Time wraps cleanly at 32 bits (about 49 days of uptime). */
    CHECK("millisecond counter rollover is harmless",
          padfw_led_color(PADFW_WAIT, false, 0xFFFFFFFFu).r > 0);
}

int main(void) {
    test_debounce();
    test_fn_first();
    test_format();
    test_parse();
    test_lines();
    test_colors();
    printf("\n%s (%d failed)\n", failures ? "FAILURES" : "all passed", failures);
    return failures != 0;
}
