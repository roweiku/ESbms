/*
 * Copyright (c) The Libre Solar Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <bms/bms.h>

#include "bms_state_machine.h"

#include <math.h>

LOG_MODULE_REGISTER(ws2812, CONFIG_LOG_DEFAULT_LEVEL);

#define STRIP_NODE     DT_CHOSEN(zephyr_led_strip)
#define NUM_LEDS       DT_PROP(STRIP_NODE, chain_length)
#define TICK_MS        50
#define TICKS_250MS    5   /* 250ms / 50ms */
#define TICKS_500MS    10  /* 500ms / 50ms */
#define TICKS_3S       60  /* 3s / 50ms */
#define MAX_BRIGHTNESS 128

static const struct device *strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[NUM_LEDS];

extern struct bms_context bms;

/* ── Color helpers ───────────────────────────────────────────── */

static inline struct led_rgb rgb_white(uint8_t brightness)
{
    return (struct led_rgb){ .r = brightness, .g = brightness, .b = brightness };
}

static inline struct led_rgb rgb_green(uint8_t brightness)
{
    return (struct led_rgb){ .r = 0, .g = brightness, .b = 0 };
}

static inline struct led_rgb rgb_red(uint8_t brightness)
{
    return (struct led_rgb){ .r = brightness, .g = 0, .b = 0 };
}

static inline struct led_rgb rgb_off(void)
{
    return (struct led_rgb){ .r = 0, .g = 0, .b = 0 };
}

/* ── Animation: OFF / SHUTDOWN (all dark) ────────────────────── */

static void anim_off(void)
{
    for (int i = 0; i < NUM_LEDS; i++) {
        pixels[i] = rgb_off();
    }
}

/* ── Animation: SOC display (SM_SOC / SM_NORMAL) ─────────────── */

/*
 * SOC level table: each entry defines how many LEDs are solid (A)
 * and which LED blinks (B). Levels map to 12.5% steps.
 *
 * Level 0: =0%       → all off
 * Level 1: 0-12.5%   → LED1=B
 * Level 2: 12.5-25%  → LED1=A
 * Level 3: 25-37.5%  → LED1=A, LED2=B
 * Level 4: 37.5-50%  → LED1-2=A
 * Level 5: 50-62.5%  → LED1-2=A, LED3=B
 * Level 6: 62.5-75%  → LED1-3=A
 * Level 7: 75-87.5%  → LED1-3=A, LED4=B
 * Level 8: 87.5-100% → LED1-4=A
 */
static void anim_soc(uint32_t tick)
{
    float soc = bms.soc;
    int level;

    if (soc <= 0.0f) {
        level = 0;
    }
    else if (soc >= 87.5f) {
        level = 8;
    }
    else {
        level = 1 + (int)(soc / 12.5f);
    }

    int solid = level / 2;       /* Number of solid-on LEDs */
    bool has_blink = (level % 2); /* Is there a blinking LED? */
    int blink_idx = solid;        /* Index of blinking LED (if any) */

    /* Blink phase: 250ms on, 250ms off, starting dark */
    bool blink_on = has_blink && ((tick / TICKS_250MS) % 2 == 1);

    for (int i = 0; i < NUM_LEDS; i++) {
        if (i < solid) {
            pixels[i] = rgb_white(MAX_BRIGHTNESS);
        }
        else if (i == blink_idx && blink_on) {
            pixels[i] = rgb_white(MAX_BRIGHTNESS);
        }
        else {
            pixels[i] = rgb_off();
        }
    }
}

/* ── Animation: OPEN (sequential light-up) ───────────────────── */

static void anim_open(uint32_t tick)
{
    /* Frame index: 0=dark, 1=LED1, 2=LED1-2, 3=LED1-3, 4=all */
    int frame = tick / TICKS_250MS;

    for (int i = 0; i < NUM_LEDS; i++) {
        pixels[i] = (i < frame) ? rgb_white(MAX_BRIGHTNESS) : rgb_off();
    }
}

/* ── Animation: C1 (all flash) ───────────────────────────────── */

static void anim_c1(uint32_t tick)
{
    bool on = ((tick / TICKS_250MS) % 2 == 0);
    struct led_rgb color = on ? rgb_white(MAX_BRIGHTNESS) : rgb_off();

    for (int i = 0; i < NUM_LEDS; i++) {
        pixels[i] = color;
    }
}

/* ── Animation: C2 (sequential turn-off) ─────────────────────── */

static void anim_c2(uint32_t tick)
{
    /* Frame: 0=all on, 1=3 on, 2=2 on, 3=1 on, 4=all off */
    int frame = tick / TICKS_250MS;
    int lit = NUM_LEDS - frame;

    for (int i = 0; i < NUM_LEDS; i++) {
        pixels[i] = (i < lit) ? rgb_white(MAX_BRIGHTNESS) : rgb_off();
    }
}

/* ── Animation: CHARGE (cycling charge) ──────────────────────── */

static void anim_charge(uint32_t tick)
{
    float soc = bms.soc;

    if (bms.full) {
        anim_off();
        return;
    }

    /* Number of LEDs in the animation range */
    int range;
    if (soc < 25.0f) {
        range = 1;
    }
    else if (soc < 50.0f) {
        range = 2;
    }
    else if (soc < 75.0f) {
        range = 3;
    }
    else {
        range = 4;
    }

    /*
     * Cycle: frame 0 = all dark, frame 1 = LED1 on, ..., frame N = LED1..N on
     * Total frames = range + 1 (including the dark frame)
     */
    int frame = (tick / TICKS_250MS) % (range + 1);

    for (int i = 0; i < NUM_LEDS; i++) {
        if (i < frame) {
            pixels[i] = rgb_green(MAX_BRIGHTNESS);
        }
        else {
            pixels[i] = rgb_off();
        }
    }
}

/* ── Animation: BALANCE (green gradient ping-pong) ───────────── */

static void anim_balance(uint32_t tick)
{
    /*
     * Ping-pong: position moves 0→3→0 over 6 segments.
     * Each segment takes ~20 ticks (1 second), full cycle ~6 seconds.
     */
    #define BALANCE_SEGMENT_TICKS 20
    #define BALANCE_CYCLE_TICKS   (BALANCE_SEGMENT_TICKS * 6)

    int cycle_tick = tick % BALANCE_CYCLE_TICKS;
    /* Triangle wave: 0→3→0 */
    float pos;
    if (cycle_tick < BALANCE_SEGMENT_TICKS * 3) {
        pos = (float)cycle_tick / BALANCE_SEGMENT_TICKS;
    }
    else {
        pos = 6.0f - (float)cycle_tick / BALANCE_SEGMENT_TICKS;
    }

    for (int i = 0; i < NUM_LEDS; i++) {
        float dist = fabsf((float)i - pos);
        int brightness = (int)(MAX_BRIGHTNESS * (1.0f - dist / 1.5f));
        if (brightness < 0) {
            brightness = 0;
        }
        pixels[i] = rgb_green((uint8_t)brightness);
    }
}

/* ── Animation: ERROR (binary error code) ────────────────────── */

static void anim_error(uint32_t tick)
{
    uint32_t flags = bms.error_flags;

    if (flags == 0) {
        anim_off();
        return;
    }

    /* Count active errors and find the one to display */
    int error_count = 0;
    int error_numbers[15];

    for (int bit = 0; bit < 15; bit++) {
        if (flags & BIT(bit)) {
            error_numbers[error_count++] = bit + 1; /* 1-indexed */
        }
    }

    /* Rotate through errors every 3 seconds */
    int idx = (tick / TICKS_3S) % error_count;
    int error_num = error_numbers[idx];

    /* Blink: on/off 250ms */
    bool on = ((tick / TICKS_250MS) % 2 == 0);

    for (int i = 0; i < NUM_LEDS; i++) {
        if (on && (error_num & BIT(i))) {
            pixels[i] = rgb_red(MAX_BRIGHTNESS);
        }
        else {
            pixels[i] = rgb_off();
        }
    }
}

/* ── WS2812 thread ───────────────────────────────────────────── */

static void ws2812_thread(void *p1, void *p2, void *p3)
{
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip device not ready");
        return;
    }

    LOG_INF("WS2812 LED strip initialized (%d LEDs)", NUM_LEDS);

    uint8_t prev_state = SM_STATE_COUNT; /* Invalid initial value to force reset */
    uint32_t tick = 0;

    while (true) {
        uint32_t events = k_event_wait(&sm_state_events, 0xFFFFFFFF, false, K_MSEC(TICK_MS));
        if (events) {
            k_event_clear(&sm_state_events, events);
        }
        // DEBUG
        // LOG_INF("WS: events=0x%04x state=%d prev=%d", events, events ? (__builtin_ffs(events) - 1) : prev_state, prev_state);

        uint8_t state = events ? (__builtin_ffs(events) - 1) : prev_state;

        if (state != prev_state) {
            tick = 0;
            prev_state = state;
        }

        switch (state) {
        case SM_OFF:
        case SM_SHUTDOWN:
        case SM_STANDBY:
            anim_off();
            break;
        case SM_SOC:
        case SM_NORMAL:
            anim_soc(tick);
            break;
        case SM_OPEN:
            anim_open(tick);
            break;
        case SM_C1:
            anim_c1(tick);
            break;
        case SM_C2:
            anim_c2(tick);
            break;
        case SM_CHARGE:
            anim_charge(tick);
            break;
        case SM_BALANCE:
            anim_balance(tick);
            break;
        case SM_ERROR:
            anim_error(tick);
            break;
        default:
            anim_off();
            break;
        }

        led_strip_update_rgb(strip, pixels, NUM_LEDS);
        tick++;
    }
}

K_THREAD_DEFINE(ws2812_tid, 1024, ws2812_thread, NULL, NULL, NULL, 6, 0, 0);
