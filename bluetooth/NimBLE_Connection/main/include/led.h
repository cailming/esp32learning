/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef LED_H
#define LED_H

/*
 * LED 模块头文件：声明 LED 初始化、点亮、熄灭和状态读取接口。
 */

/* ==================== ESP-IDF API ==================== */
#include "driver/gpio.h" // 普通 GPIO LED 驱动 API
#include "led_strip.h"   // 可寻址 RGB LED 驱动 API
#include "sdkconfig.h"   // CONFIG_BLINK_GPIO 等 menuconfig 配置

/* 将 menuconfig 中配置的 LED GPIO 引脚取一个更直观的别名 */
#define BLINK_GPIO CONFIG_BLINK_GPIO

/* 对外提供的 LED 控制函数 */
uint8_t get_led_state(void); // 获取当前保存的 LED 状态
void led_on(void);           // 点亮 LED
void led_off(void);          // 熄灭 LED
void led_init(void);         // 初始化 LED 硬件

#endif // LED_H
