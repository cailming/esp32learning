/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef GAP_SVC_H
#define GAP_SVC_H

/*
 * gap.h 声明 GAP 模块对外提供的接口。
 * GAP（Generic Access Profile）主要负责 BLE 的发现、广播和连接过程。
 */

/* NimBLE GAP API */
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"

/* Generic Tag 对应的标准 Appearance 值 */
#define BLE_GAP_APPEARANCE_GENERIC_TAG 0x0200

/* BLE URI 数据格式中 HTTPS 前缀对应的编码 */
#define BLE_GAP_URI_PREFIX_HTTPS 0x17

/* LE Peripheral 角色编码 */
#define BLE_GAP_LE_ROLE_PERIPHERAL 0x00

/*
 * adv_init()：
 * 在 NimBLE Host 与 Controller 同步完成后调用，
 * 获取本机 BLE 地址并开始广播。
 */
void adv_init(void);

/*
 * gap_init()：
 * 初始化标准 GAP Service，并设置 BLE 设备名称。
 */
int gap_init(void);

#endif // GAP_SVC_H
