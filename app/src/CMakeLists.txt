# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)

target_sources(app PRIVATE
        bms_common.c
        bms_soc.c
        bms_state_machine.c
        button.c
        data_objects.c
        leds.c
        main.c
        ws2812_led.c
)

zephyr_sources_ifdef(CONFIG_SHIELD_UEXT_OLED oled.c)
