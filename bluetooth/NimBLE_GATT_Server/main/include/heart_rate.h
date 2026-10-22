/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef HEART_RATE_H
#define HEART_RATE_H

/*
 * heart_rate.h 声明模拟心率模块的接口。
 * 本例没有接真实心率传感器，而是使用随机数模拟 60~80 BPM。
 */

/* esp_random() 用于生成模拟心率随机数 */
#include "esp_random.h"

/*
 * 心率任务周期。
 * 1000 / portTICK_PERIOD_MS 表示约 1000 ms，也就是每 1 秒更新一次心率。
 */
#define HEART_RATE_TASK_PERIOD (1000 / portTICK_PERIOD_MS)

/* 获取当前模拟心率值 */
uint8_t get_heart_rate(void);

/* 重新生成一个 60~80 BPM 的模拟心率值 */
void update_heart_rate(void);

#endif // HEART_RATE_H
