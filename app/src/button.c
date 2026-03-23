/*
 * Copyright (c) The Libre Solar Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "button.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "helper.h"

LOG_MODULE_REGISTER(button, CONFIG_LOG_DEFAULT_LEVEL);

#define BTN_GPIO  DT_ALIAS(sw_pwr)
#define BTN_CTLR  DT_GPIO_CTLR(BTN_GPIO, gpios)
#define BTN_PIN   DT_GPIO_PIN(BTN_GPIO, gpios)
#define BTN_FLAGS DT_GPIO_FLAGS(BTN_GPIO, gpios)

static const struct device *btn_dev = DEVICE_DT_GET(BTN_CTLR);
static struct gpio_callback btn_cb_data;

/* Forward declaration - defined in bms_state_machine.c */
extern struct k_event sm_events;

static struct k_work_delayable debounce_work;
static struct k_work_delayable long_press_work;

static void debounce_handler(struct k_work *work)
{
    int pin_state = gpio_pin_get(btn_dev, BTN_PIN);

    if (pin_state == 1) {
        /* Button pressed (active) */
        k_event_post(&sm_events, BTN_EVT_PRESS);
        /* Start long press timer (5 seconds) */
        k_work_schedule(&long_press_work, K_MSEC(5000));
        LOG_INF("Button pressed");
    } else {
        /* Button released */
        k_event_post(&sm_events, BTN_EVT_RELEASE);
        /* Cancel long press timer */
        k_work_cancel_delayable(&long_press_work);
        LOG_INF("Button released");
    }
}

static void long_press_handler(struct k_work *work)
{
    /* Verify button is still held down */
    if (gpio_pin_get(btn_dev, BTN_PIN) == 1) {
        k_event_post(&sm_events, BTN_EVT_LONG_PRESS_5S);
        LOG_INF("Button long pressed (5s)");
    }
}

static void gpio_cb(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    /* Reschedule debounce work at 20ms */
    k_work_reschedule(&debounce_work, K_MSEC(20));
}

void button_init(void)
{
    if (!device_is_ready(btn_dev)) {
        LOG_ERR("Button device not found");
        return;
    }

    /* Initialize work items */
    k_work_init_delayable(&debounce_work, debounce_handler);
    k_work_init_delayable(&long_press_work, long_press_handler);

    /* Configure GPIO */
    gpio_pin_configure(btn_dev, BTN_PIN, BTN_FLAGS | GPIO_INPUT);
    gpio_pin_interrupt_configure(btn_dev, BTN_PIN, GPIO_INT_EDGE_BOTH);

    /* Setup callback */
    gpio_init_callback(&btn_cb_data, gpio_cb, BIT(BTN_PIN));
    gpio_add_callback(btn_dev, &btn_cb_data);

    LOG_INF("Button initialized");
}
