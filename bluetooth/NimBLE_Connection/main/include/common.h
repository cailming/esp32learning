/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef COMMON_H
#define COMMON_H

/*
 * common.h 放置整个示例都会使用的公共头文件和宏定义。
 * main.c、gap.c、led.c 都会包含它。
 */

/* ==================== C 标准库 ==================== */
#include <stdbool.h>  // bool / true / false
#include <stdio.h>    // sprintf 等标准输入输出函数
#include <string.h>   // strlen 等字符串处理函数

/* ==================== ESP-IDF API ==================== */
#include "esp_log.h"   // ESP_LOGI / ESP_LOGE 等日志宏
#include "nvs_flash.h" // NVS Flash 初始化与擦除 API
#include "sdkconfig.h" // menuconfig 生成的配置宏

/* ==================== FreeRTOS API ==================== */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h> // xTaskCreate、vTaskDelete 等任务 API

/* ==================== NimBLE 协议栈 API ==================== */
#include "host/ble_hs.h"              // NimBLE Host 核心定义和配置
#include "host/ble_uuid.h"            // BLE UUID 相关定义
#include "host/util/util.h"            // NimBLE Host 工具函数
#include "nimble/ble.h"                // NimBLE 基础 BLE 定义
#include "nimble/nimble_port.h"        // NimBLE Port 初始化和运行 API
#include "nimble/nimble_port_freertos.h" // NimBLE 与 FreeRTOS 的适配 API

/* 日志 TAG：串口日志中会用它标识日志来自哪个模块 */
#define TAG "NimBLE_Connection"

/* BLE 设备名称：手机扫描时会看到 NimBLE_CONN */
#define DEVICE_NAME "NimBLE_CONN"

#endif // COMMON_H
