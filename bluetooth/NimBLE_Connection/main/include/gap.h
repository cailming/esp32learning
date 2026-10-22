/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef GAP_SVC_H
#define GAP_SVC_H

/*
 * 本头文件声明 GAP 模块对外提供的接口，并定义本示例使用的 GAP 常量。
 */

/* ==================== NimBLE GAP API ==================== */
#include "host/ble_gap.h"              // GAP 广播、连接、事件等 API
#include "services/gap/ble_svc_gap.h" // 标准 GAP Service API

/*
 * BLE Appearance：Generic Tag（通用标签设备）。
 * 0x0200 是 Bluetooth SIG 为 Generic Tag 定义的 Appearance 值。
 */
#define BLE_GAP_APPEARANCE_GENERIC_TAG 0x0200

/* BLE URI 数据类型中 https: 的前缀编码 */
#define BLE_GAP_URI_PREFIX_HTTPS 0x17

/* LE 角色：Peripheral（外设） */
#define BLE_GAP_LE_ROLE_PERIPHERAL 0x00

/*
 * adv_init()：准备本机 BLE 地址并启动广播。
 * gap_init()：初始化标准 GAP 服务，设置设备名称和 Appearance。
 */
void adv_init(void);
int gap_init(void);

#endif // GAP_SVC_H
