/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * 本文件是整个 NimBLE_Connection 示例的程序入口。
 *
 * 对初学者来说，可以先把整个启动流程记成下面 5 步：
 * 1. 初始化 LED；
 * 2. 初始化 NVS（NimBLE 会用到非易失性存储）；
 * 3. 初始化 NimBLE 协议栈；
 * 4. 初始化 GAP 服务，并配置 NimBLE Host 的回调函数；
 * 5. 创建 NimBLE Host 的 FreeRTOS 任务，让蓝牙协议栈真正开始运行。
 */

/* 头文件 */
#include "common.h"
#include "gap.h"
#include "led.h"

/*
 * NimBLE 内部提供的存储配置初始化函数。
 * 这里没有在本项目中实现，它由 NimBLE/ESP-IDF 提供。
 */
void ble_store_config_init(void);

/* 本文件内部使用的函数声明，static 表示这些函数只在 main.c 中可见 */
static void on_stack_reset(int reason);
static void on_stack_sync(void);
static void nimble_host_config_init(void);
static void nimble_host_task(void *param);

/*
 * NimBLE Host 协议栈事件回调函数：
 *
 * on_stack_reset：
 *   当 NimBLE Host 因错误而重置 BLE 协议栈时被调用。
 *
 * on_stack_sync：
 *   当 NimBLE Host 与蓝牙 Controller 完成同步后被调用。
 *   只有同步完成后，Host 才能正常开始 BLE 广播、扫描、连接等操作。
 */
static void on_stack_reset(int reason) {
    /* 协议栈发生重置时，将重置原因打印到串口日志 */
    ESP_LOGI(TAG, "nimble stack reset, reset reason: %d", reason);
}

static void on_stack_sync(void) {
    /*
     * Host 与 Controller 同步完成后开始初始化广播。
     * adv_init() 会确定本机蓝牙地址，然后调用 start_advertising() 开始广播。
     */
    adv_init();
}

static void nimble_host_config_init(void) {
    /*
     * 设置 NimBLE Host 的几个重要回调函数。
     * ble_hs_cfg 是 NimBLE Host 的全局配置结构体。
     */
    ble_hs_cfg.reset_cb = on_stack_reset;              // Host 重置时回调
    ble_hs_cfg.sync_cb = on_stack_sync;                // Host 与 Controller 同步完成时回调
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr; // BLE 存储状态处理回调

    /*
     * 初始化 NimBLE 的持久化存储配置。
     * 例如配对、绑定等安全信息可能需要保存在 NVS 中。
     */
    ble_store_config_init();
}

static void nimble_host_task(void *param) {
    /* NimBLE Host FreeRTOS 任务启动时打印一条日志 */
    ESP_LOGI(TAG, "nimble host task has been started!");

    /*
     * 进入 NimBLE Host 的主事件循环。
     * 这个函数会一直运行，负责处理 BLE 协议栈事件；
     * 只有调用 nimble_port_stop() 后它才会返回。
     */
    nimble_port_run();

    /*
     * 如果 nimble_port_run() 退出，说明 Host 已停止，
     * 删除当前 FreeRTOS 任务，释放该任务占用的资源。
     */
    vTaskDelete(NULL);
}

void app_main(void) {
    /*
     * ESP-IDF 程序入口。
     * 与普通 C 程序的 main() 不同，ESP-IDF 使用 app_main() 作为应用入口。
     */

    /* 局部变量：rc 通常保存 NimBLE API 返回值，ret 保存 ESP-IDF API 返回值 */
    int rc = 0;
    esp_err_t ret = ESP_OK;

    /* 第 1 步：初始化开发板上的 LED，连接状态会通过 LED 显示 */
    led_init();

    /*
     * 第 2 步：初始化 NVS（Non-Volatile Storage，非易失性存储）。
     * NVS 是 ESP32 Flash 上的一块键值存储区域。
     * NimBLE 的配对/绑定等功能可能需要使用它保存数据。
     */
    ret = nvs_flash_init();

    /*
     * 如果 NVS 空间不足，或者 Flash 中的 NVS 数据版本与当前程序不兼容，
     * 就先擦除 NVS，再重新初始化。
     */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    /* NVS 初始化仍然失败，则记录错误并终止后续 BLE 初始化 */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nvs flash, error code: %d ", ret);
        return;
    }

    /*
     * 第 3 步：初始化 NimBLE 协议栈。
     * 可以把 NimBLE 理解为 ESP32 上负责 BLE 通信的软件协议栈。
     */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nimble stack, error code: %d ",
                 ret);
        return;
    }

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    /*
     * 第 4 步之一：初始化 GAP 服务。
     * GAP（Generic Access Profile）负责 BLE 设备的“发现和连接”相关能力，
     * 这里主要设置设备名称和 Appearance（设备外观类型）。
     */
    rc = gap_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to initialize GAP service, error code: %d", rc);
        return;
    }
#endif

    /*
     * 第 4 步之二：配置 NimBLE Host 回调。
     * 最重要的是 sync_cb：Host 与 Controller 同步完成后，会进入 adv_init() 开始广播。
     */
    nimble_host_config_init();

    /*
     * 第 5 步：创建 NimBLE Host 的 FreeRTOS 任务。
     *
     * 参数依次为：
     * 1. nimble_host_task：任务入口函数
     * 2. "NimBLE Host"：任务名字，主要用于调试
     * 3. 4*1024：任务栈大小，这里是 4 KB
     * 4. NULL：传给任务函数的参数，这里没有参数
     * 5. 5：任务优先级
     * 6. NULL：不需要保存任务句柄
     */
    xTaskCreate(nimble_host_task, "NimBLE Host", 4*1024, NULL, 5, NULL);

    /*
     * app_main() 到这里即可返回。
     * BLE 协议栈后续会继续在刚刚创建的 NimBLE Host 任务中运行。
     */
    return;
}
