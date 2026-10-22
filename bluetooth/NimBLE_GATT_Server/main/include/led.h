/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef LED_H
#define LED_H

/*
 * led.h 声明 LED 模块对外提供的接口。
 * GATT Server 中的 LED Characteristic 收到手机写入后，
 * 最终会调用这里声明的 led_on() / led_off()。
 */

/* ESP-IDF GPIO 和 LED Strip 驱动 API */
#include "driver/gpio.h"
#include "led_strip.h"
#include "sdkconfig.h"

/* LED 使用的 GPIO，由 menuconfig / sdkconfig 配置 */
#define BLINK_GPIO CONFIG_BLINK_GPIO

/* 获取当前 LED 状态 */
uint8_t get_led_state(void);

/* 点亮 LED */
void led_on(void);

/* 熄灭 LED */
void led_off(void);

/* 根据项目配置初始化普通 GPIO LED 或可寻址 RGB LED */
void led_init(void);

#endif // LED_H
