/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef GATT_SVR_H
#define GATT_SVR_H

/*
 * gatt_svc.h 声明 GATT Server 模块对外提供的接口。
 *
 * GATT（Generic Attribute Profile）负责 BLE 连接建立后的数据交换，
 * 核心概念包括 Service、Characteristic、READ、WRITE、Notify/Indicate。
 */

/* NimBLE GATT API */
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"

/*
 * 订阅事件从 GAP 事件回调进入，因此这里也需要 ble_gap_event 类型定义。
 */
#include "host/ble_gap.h"

/*
 * 如果客户端已订阅 Heart Rate Indication，
 * 则向对应连接主动发送一次心率数据。
 */
void send_heart_rate_indication(void);

/*
 * GATT Service / Characteristic / Descriptor 注册回调。
 * 主要用于打印 UUID 和 handle 等调试信息。
 */
void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);

/*
 * 处理客户端 Subscribe / Unsubscribe 事件，
 * 保存 Heart Rate Characteristic 的订阅状态和连接 handle。
 */
void gatt_svr_subscribe_cb(struct ble_gap_event *event);

/*
 * BLE 连接断开后清除 Heart Rate 的旧订阅状态，
 * 避免继续向已经失效的 conn_handle 发送 Indication。
 */
void gatt_svr_reset_heart_rate_subscription(void);

/*
 * 初始化 GATT Server，并把本例定义的 Service 表加入 NimBLE。
 */
int gatt_svc_init(void);

#endif // GATT_SVR_H
