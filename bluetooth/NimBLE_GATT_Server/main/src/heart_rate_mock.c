/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * 本文件不读取真实心率传感器，而是生成一个模拟心率值。
 * 这样可以把重点放在 BLE GATT 的 READ / INDICATE 流程上，
 * 不需要额外连接硬件传感器。
 */

/* 头文件 */
#include "common.h"
#include "heart_rate.h"

/* 保存当前模拟心率值，类型为 uint8_t */
static uint8_t heart_rate;

/* 返回当前心率值 */
uint8_t get_heart_rate(void) { return heart_rate; }

/*
 * 更新模拟心率。
 * esp_random() 生成随机数，% 21 得到 0~20，
 * 再加 60，因此最终得到 60~80 BPM 之间的模拟值。
 */
void update_heart_rate(void) { heart_rate = 60 + (uint8_t)(esp_random() % 21); }
