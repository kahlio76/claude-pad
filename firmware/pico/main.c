/* Claude Pad firmware for the Raspberry Pi Pico (RP2040).
 *
 * This file is only the glue between the chip and the logic in ../src/pad_fw.c:
 * read the switches, ship debounced edges to the Mac, take LED states back,
 * and push colors out to four WS2812 LEDs. Everything that could be wrong
 * about *what* to send lives in pad_fw.c, where it has tests. What's left here
 * is "which pin is which".
 *
 * Wiring (each switch goes between its GPIO and ground; the internal pull-ups
 * hold the pin high until the switch pulls it low):
 *
 *   GP2  S1       GP6  Approve   GP10 Talk
 *   GP3  S2       GP7  Always    GP11 Stop
 *   GP4  S3       GP8  Deny      GP12 FN
 *   GP5  S4       GP9  Mode
 *   GP16 -> DIN of the first LED; the four LEDs are chained DOUT -> DIN
 *
 * Serial: the same bytes go out over USB (what the Mac sees) and UART0 on
 * GP0/GP1 (what Wokwi and a USB-serial adapter can read).
 */
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "pad_fw.h"
#include "ws2812.pio.h"

static const uint8_t KEY_PIN[PAD_KEY_COUNT] = {
    [PAD_S1] = 2,    [PAD_S2] = 3,     [PAD_S3] = 4,   [PAD_S4] = 5,
    [PAD_APPROVE] = 6, [PAD_ALWAYS] = 7, [PAD_DENY] = 8,
    [PAD_MODE] = 9,  [PAD_TALK] = 10,  [PAD_STOP] = 11, [PAD_FN] = 12,
};

enum {
    LED_PIN = 16,
    SCAN_PERIOD_US = 1000, /* sample the switches at 1 kHz */
    LED_REFRESH_MS = 20,   /* redraw the LEDs at 50 Hz so the blink stays smooth */
};

static PIO led_pio = pio0;
static uint led_sm;

/* Read every switch in one go, so they're all sampled at the same instant. */
static uint16_t read_keys(void) {
    uint32_t pins = gpio_get_all();
    uint16_t raw = 0;
    for (int k = 0; k < PAD_KEY_COUNT; ++k)
        if (!((pins >> KEY_PIN[k]) & 1u)) /* pulled low = pressed */
            raw |= (uint16_t)(1u << k);
    return raw;
}

/* WS2812s take green, red, blue, in that order. The PIO program shifts out the
 * top 24 bits of each 32-bit word, hence the shift by 8. */
static void put_pixel(PadFwRgb c) {
    uint32_t grb = ((uint32_t)c.g << 16) | ((uint32_t)c.r << 8) | c.b;
    pio_sm_put_blocking(led_pio, led_sm, grb << 8u);
}

static void show_leds(const PadFwLeds *leds, uint32_t now_ms) {
    for (int s = 0; s < PAD_SLOTS; ++s)
        put_pixel(padfw_led_color(leds->slot[s], s == leds->selected, now_ms));
}

int main(void) {
    stdio_init_all();

    for (int k = 0; k < PAD_KEY_COUNT; ++k) {
        gpio_init(KEY_PIN[k]);
        gpio_set_dir(KEY_PIN[k], GPIO_IN);
        gpio_pull_up(KEY_PIN[k]);
    }

    uint offset = pio_add_program(led_pio, &ws2812_program);
    led_sm = (uint)pio_claim_unused_sm(led_pio, true);
    ws2812_program_init(led_pio, led_sm, offset, LED_PIN, 800000, false);

    PadFwDebounce debounce;
    PadFwLine line;
    PadFwLeds leds = {{PADFW_OFF, PADFW_OFF, PADFW_OFF, PADFW_OFF}, 0}; /* dark until the Mac says otherwise */
    padfw_debounce_init(&debounce);
    padfw_line_init(&line);

    show_leds(&leds, 0);
    uint32_t next_led_ms = 0;
    bool leds_changed = false;
    absolute_time_t next_scan = get_absolute_time();

    for (;;) {
        sleep_until(next_scan);
        next_scan = delayed_by_us(next_scan, SCAN_PERIOD_US);
        /* If something stalled us (a USB write waiting on the host, say), don't
         * replay the missed samples back to back: that would squeeze a real
         * press's debounce window into microseconds. Just resume the cadence. */
        if (absolute_time_diff_us(get_absolute_time(), next_scan) < -SCAN_PERIOD_US)
            next_scan = get_absolute_time();

        /* Switches -> edges -> the Mac. */
        PadFwEdge edges[PAD_KEY_COUNT];
        int n = padfw_debounce_step(&debounce, read_keys(), edges);
        for (int i = 0; i < n; ++i) {
            char msg[16];
            int len = padfw_format_edge(msg, sizeof msg, edges[i].key, edges[i].down);
            if (len > 0)
                stdio_put_string(msg, len, false, false); /* exactly "D Approve\n", no CRLF rewriting */
        }

        /* The Mac -> LED states. A garbled line is ignored, never half-applied. */
        int c;
        while ((c = getchar_timeout_us(0)) >= 0) {
            PadFwLeds parsed;
            if (padfw_line_feed(&line, (char)c) && padfw_parse_leds(line.text, &parsed)) {
                leds = parsed;
                leds_changed = true;
            }
        }

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (leds_changed || (int32_t)(now_ms - next_led_ms) >= 0) {
            show_leds(&leds, now_ms);
            next_led_ms = now_ms + LED_REFRESH_MS;
            leds_changed = false;
        }
    }
}
