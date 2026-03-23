/*
 * Copyright (c) The Libre Solar Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BMS_STATE_MACHINE_H_
#define BMS_STATE_MACHINE_H_

#include <zephyr/kernel.h>

#include <stdint.h>

/**
 * Hierarchical State Machine with independent charging session
 *
 * SM_STANDBY (parent: user OFF)
 *   ├── SM_OFF     (initial: all FETs off)
 *   ├── SM_SOC     (SOC display on button press)
 *   └── SM_OPEN    (button held, pending power-on)
 *
 * SM_ON (parent: user ON)
 *   ├── SM_NORMAL  (initial: all FETs on)
 *   ├── SM_C1      (shutdown confirm step 1)
 *   └── SM_C2      (shutdown confirm step 2)
 *
 * SM_CHARGING (parent: charging session)
 *   ├── SM_CHARGE  (initial: all FETs on, actively charging)
 *   └── SM_BALANCE (DIS+PDSG only, CHG off for balancing)
 *
 * SM_SHUTDOWN (leaf: shutdown sequence)
 * SM_ERROR    (leaf: hardware error, all FETs off)
 */
/**
 * State machine leaf states (maps 1:1 to SMF state table indices).
 */
enum sm_state
{
    SM_STANDBY,
    SM_OFF,
    SM_SOC,
    SM_OPEN,
    SM_ON,
    SM_NORMAL,
    SM_C1,
    SM_C2,
    SM_CHARGING,
    SM_CHARGE,
    SM_BALANCE,
    SM_SHUTDOWN,
    SM_ERROR,
    SM_STATE_COUNT,
};

/**
 * @brief State machine state change events
 * 
 * Each state transition sets the corresponding bit (BIT(state)).
 * Subscribers can extract the state using: state = __builtin_ffs(events) - 1
 * 
 * Bit mapping:
 *   BIT(0)  = SM_STANDBY
 *   BIT(1)  = SM_OFF
 *   BIT(2)  = SM_SOC
 *   BIT(3)  = SM_OPEN
 *   BIT(4)  = SM_ON
 *   BIT(5)  = SM_NORMAL
 *   BIT(6)  = SM_C1
 *   BIT(7)  = SM_C2
 *   BIT(8)  = SM_CHARGING
 *   BIT(9)  = SM_CHARGE
 *   BIT(10) = SM_BALANCE
 *   BIT(11) = SM_SHUTDOWN
 *   BIT(12) = SM_ERROR
 */
extern struct k_event sm_state_events;

#endif /* BMS_STATE_MACHINE_H_ */
