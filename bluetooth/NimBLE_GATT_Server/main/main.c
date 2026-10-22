/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * 本文件是 NimBLE_GATT_Server 示例的程序入口。
 *
 * 这个示例是在 NimBLE_Connection 的基础上继续增加 GATT Server 功能：
 * 1. 仍然先初始化 LED、NVS、NimBLE 和 GAP；
 * 2. 新增 GATT Server 初始化，注册 Heart Rate 和 Automation IO 两个服务；
 * 3. 创建 NimBLE Host 任务，负责处理 BLE 协议栈事件；
 * 4. 再创建一个 Heart Rate 任务，每 1 秒生成一次模拟心率；
 * 5. 如果手机订阅了心率 Indication，则 ESP32 会主动把最新心率发给手机。
 */

/* 头文件 */
#include "common.h"
#include "gap.h"
#include "gatt_svc.h"
#include "heart_rate.h"
#include "led.h"

/*
 * NimBLE 提供的存储配置初始化函数。
 * 该函数由 ESP-IDF / NimBLE 提供，不在本项目中实现。
 */
void ble_store_config_init(void);

/* 本文件内部函数声明，static 表示这些函数只在 main.c 中使用 */
static void on_stack_reset(int reason);
static void on_stack_sync(void);
static void nimble_host_config_init(void);
static void nimble_host_task(void *param);

/*
 * NimBLE Host 协议栈事件回调函数：
 *
 * on_stack_reset：
 *   当 NimBLE Host 因错误重置 BLE 协议栈时调用。
 *
 * on_stack_sync：
 *   当 NimBLE Host 和蓝牙 Controller 完成同步后调用。
 *   只有同步完成后，才能安全开始广播、扫描、连接等 BLE 操作。
 */
static void on_stack_reset(int reason) {
    /* 协议栈重置时，把原因打印到串口日志 */
    ESP_LOGI(TAG, "nimble stack reset, reset reason: %d", reason);
}

static void on_stack_sync(void) {
    /*
     * Host 和 Controller 已经同步完成，开始初始化 BLE 广播。
     * adv_init() 会确定本机蓝牙地址，然后调用 start_advertising() 开始广播。
     */
    adv_init();
}

static void nimble_host_config_init(void) {
    /*
     * 配置 NimBLE Host 的全局回调函数。
     * ble_hs_cfg 是 NimBLE Host 的配置结构体。
     */
    ble_hs_cfg.reset_cb = on_stack_reset;                 // Host 重置时回调
    ble_hs_cfg.sync_cb = on_stack_sync;                   // Host 与 Controller 同步完成时回调
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;  // GATT Service / Characteristic 注册时回调
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;// BLE 持久化存储状态处理回调

    /*
     * 初始化 NimBLE 的持久化存储配置。
     * 例如配对、绑定等安全信息可能会保存在 NVS 中。
     */
    ble_store_config_init();
}

static void nimble_host_task(void *param) {
    /* NimBLE Host FreeRTOS 任务启动日志 */
    ESP_LOGI(TAG, "nimble host task has been started!");

    /*
     * 进入 NimBLE Host 主事件循环。
     * 这个函数会持续处理 BLE 协议栈事件，直到调用 nimble_port_stop() 才返回。
     */
    nimble_port_run();

    /* 如果 NimBLE Host 已停止，则删除当前 FreeRTOS 任务 */
    vTaskDelete(NULL);
}

static void heart_rate_task(void *param) {
    /* 心率模拟任务启动日志 */
    ESP_LOGI(TAG, "heart rate task has been started!");

    /*
     * 这是一个永久循环任务，每隔 1 秒执行一次：
     * 1. 生成新的模拟心率值；
     * 2. 打印当前心率；
     * 3. 如果手机已订阅 Indication，则主动把心率发给手机。
     */
    while (1) {
        /* 更新模拟心率值 */
        update_heart_rate();
        ESP_LOGI(TAG, "heart rate updated to %d", get_heart_rate());

        /*
         * 尝试发送心率 Indication。
         * 如果当前没有客户端订阅 Indication，这个函数内部不会真正发送。
         */
        send_heart_rate_indication();

        /*
         * 任务休眠 HEART_RATE_TASK_PERIOD。
         * 当前定义为约 1000 ms，也就是每秒更新一次。
         */
        vTaskDelay(HEART_RATE_TASK_PERIOD);
    }

    /* 正常情况下 while(1) 不会退出；这里是任务退出后的清理代码 */
    vTaskDelete(NULL);
}

void app_main(void) {
    /*
     * ESP-IDF 的应用程序入口。
     * 与普通 C 程序的 main() 不同，ESP-IDF 使用 app_main()。
     */

    /*
     * rc 用来保存 FreeRTOS / NimBLE API 的返回值；
     * ret 用来保存 ESP-IDF API 返回的 esp_err_t 错误码。
     */
    BaseType_t rc = 0;
    esp_err_t ret;

    /* 第 1 步：初始化 LED。后面手机可以通过 GATT Write 控制这个 LED */
    led_init();

    /*
     * 第 2 步：初始化 NVS（Non-Volatile Storage，非易失性存储）。
     * NimBLE 的配对、绑定等配置可能需要保存到 Flash 中。
     */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS 空间不足或版本不兼容时，先擦除再重新初始化 */
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nvs flash, error code: %d ", ret);
        return;
    }

    /*
     * 第 3 步：初始化 NimBLE 协议栈。
     * NimBLE 负责 BLE 的 GAP、GATT、ATT、连接、安全等底层协议处理。
     */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nimble stack, error code: %d ",
                 ret);
        return;
    }

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    /*
     * 第 4 步：初始化 GAP Service。
     * GAP 主要负责“设备如何被发现、如何建立连接”等功能。
     * gap_init() 中会设置本机 BLE 设备名称。
     */
    rc = gap_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to initialize GAP service, error code: %d", rc);
        return;
    }
#endif

    /*
     * 第 5 步：初始化 GATT Server。
     *
     * GATT 负责“连接建立后交换什么数据”。
     * 本例注册两个主要业务 Service：
     * 1. Heart Rate Service：支持 READ + INDICATE；
     * 2. Automation IO Service：提供 LED Characteristic，支持 WRITE。
     */
    rc = gatt_svc_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to initialize GATT server, error code: %d", rc);
        return;
    }

    /*
     * 第 6 步：配置 NimBLE Host 回调。
     * 包括协议栈 reset/sync 回调，以及 GATT 属性注册回调。
     */
    nimble_host_config_init();

    /*
     * 第 7 步：创建 NimBLE Host FreeRTOS 任务。
     * 这个任务会运行 nimble_port_run()，持续处理 BLE 事件。
     */
    rc = xTaskCreate(nimble_host_task, "NimBLE Host", 4 * 1024, NULL,
                                5, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to create NimBLE host task");
        return;
    }

    /*
     * 第 8 步：创建模拟心率任务。
     * 该任务每 1 秒生成一个 60~80 之间的模拟心率，
     * 如果手机订阅了心率 Indication，则主动推送给手机。
     */
    rc = xTaskCreate(heart_rate_task, "Heart Rate", 4 * 1024, NULL, 5, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to create heart rate task");
        return;
    }

    /*
     * app_main() 到这里可以返回。
     * 后续 BLE 工作由 NimBLE Host Task 和 Heart Rate Task 持续执行。
     */
    return;
}
