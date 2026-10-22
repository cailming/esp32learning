/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef COMMON_H
#define COMMON_H

/*
 * common.h 放置整个示例都会使用的公共头文件和宏定义。
 * main.c、gap.c、gatt_svc.c 等文件都会包含它。
 */

/* ========================= C 标准库 ========================= */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ========================= ESP-IDF API ========================= */
#include "esp_log.h"    // ESP_LOGI / ESP_LOGE 等日志接口
#include "nvs_flash.h"  // NVS 非易失性存储初始化
#include "sdkconfig.h"  // menuconfig 生成的项目配置宏

/* ========================= FreeRTOS API ========================= */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ========================= NimBLE 协议栈 API ========================= */
#include "host/ble_hs.h"                 // NimBLE Host 核心接口
#include "host/ble_uuid.h"               // BLE UUID 类型与工具函数
#include "host/util/util.h"               // NimBLE Host 工具函数
#include "nimble/ble.h"
#include "nimble/nimble_port.h"          // NimBLE 初始化、运行、停止接口
#include "nimble/nimble_port_freertos.h" // NimBLE 与 FreeRTOS 的适配接口

/* 日志 TAG，串口日志中会显示为 NimBLE_GATT_Server */
#define TAG "NimBLE_GATT_Server"

/* BLE 设备名称，手机扫描时会看到 NimBLE_GATT */
#define DEVICE_NAME "NimBLE_GATT"

#endif // COMMON_H
