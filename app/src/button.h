/*
 * Copyright (c) The Libre Solar Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BUTTON_H_
#define BUTTON_H_

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @brief Button event system for power switch
 *
 * Button events are posted to the state machine event object (sm_events)
 * via k_event_post(). The SM thread consumes them with k_event_wait().
 */

/** Button event bit flags */
#define BTN_EVT_PRESS         BIT(0)
#define BTN_EVT_RELEASE       BIT(1)
#define BTN_EVT_LONG_PRESS_5S BIT(2)
#define BTN_EVT_ALL           (BTN_EVT_PRESS | BTN_EVT_RELEASE | BTN_EVT_LONG_PRESS_5S)

/**
 * Initialize button GPIO, interrupts, debounce, and long-press detection.
 *
 * Must be called after the SM event object (sm_events) is initialized.
 */
void button_init(void);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_H_ */
