/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Includes */
#include "common.h"
#include "gap.h"

/* Library function declarations 
这是 NimBLE 库提供的函数，不是本文件实现的。它初始化 BLE 的存储配置（bonding 信息、配对密钥等）。
为什么需要它？
    BLE 配对（bonding）后，设备需要保存配对信息（LTK、IRK、CSRK 等），以便下次连接时免配对。
    NimBLE 用 ble_store 抽象存储层，默认实现把数据存到 NVS。ble_store_config_init() 配置存储层的回调（读、写、删除），
    让 NimBLE 知道怎么持久化配对信息。
不调用会怎样？
    配对信息无法保存，每次连接都要重新配对。对于不需要 bonding 的示例（如 just works 配对），
    可以省略；但标准示例都调用它，保证功能完整。
*/
void ble_store_config_init(void);

/* 
    Private function declarations 
    前两个是协议栈回调，后两个是初始化辅助函数和任务入口。
*/
static void on_stack_reset(int reason);
static void on_stack_sync(void);
static void nimble_host_config_init(void);
static void nimble_host_task(void *param);

/* Private functions */
/*
 *  Stack event callback functions
 *      - on_stack_reset is called when host resets BLE stack due to errors
 * 协议栈重置回调
 * 触发时机：Host 检测到 Controller 异常（如 HCI 超时、Controller 崩溃、硬件错误），主动重置协议栈。
    reason：重置原因码，由 NimBLE 定义（如 BLE_HS_RESET_REASON_HW_FAILURE、BLE_HS_RESET_REASON_HCI_TIMEOUT 等）。
    本示例的处理：只打印日志，不做恢复。为什么？ 示例聚焦于“正常流程”，重置是异常情况。
    产品中可能需要在这里做：重新初始化协议栈、通知应用层、重启设备等。
    回调上下文：运行在 NimBLE Host 任务中（即 nimble_port_run() 所在任务）。不能在这里阻塞太久，否则影响协议栈事件处理。
    设计意图：提供一个“协议栈出问题时”的钩子。即使不处理，打印日志也有助于调试“为什么 BLE 突然不工作了”。
 */
static void on_stack_reset(int reason) {
    /* On reset, print reset reason to console */
    ESP_LOGI(TAG, "nimble stack reset, reset reason: %d", reason);
}

/*
 *      - on_stack_sync is called when host has synced with controller
    协议栈同步完成回调
    触发时机：Host 与 Controller 同步完成。
    具体来说，Host 启动后会向 Controller 发送一系列 HCI 命令（读版本、设事件掩码、设白名单等），全部完成后触发 sync_cb。
    此时协议栈完全就绪，可以开始广播、扫描、连接。
    adv_init()：初始化广播。这个函数在 gap.c 或类似文件中实现，通常做：
        配置广播数据（设备名、服务 UUID、厂商数据等）。
        配置扫描响应数据。
        设置广播参数（间隔、类型）。
        调用 ble_gap_adv_start() 开始广播。
为什么在 sync_cb 里调用而不是在 app_main 里？ 
    因为 app_main 调用 nimble_port_init() 后，协议栈还没完全就绪。
    如果在 app_main 里直接调 adv_init()，可能因为 HCI 通道未就绪而失败。sync_cb 是“协议栈准备好了”的官方信号，
    在这里做广播初始化最可靠。
    回调上下文：同样运行在 NimBLE Host 任务中。
    这是本文件最关键的逻辑：BLE 外设的“开始广播”动作挂载在 sync_cb 上，由协议栈同步完成后自动触发。
*/
static void on_stack_sync(void) {
    /* On stack sync, do advertising initialization */
    adv_init();
}

/*
注册主机回调
    ble_hs_cfg 是 NimBLE Host 的全局配置结构体，定义在 host/ble_hs.h 中。
    它包含大量回调函数指针和配置参数，应用通过赋值来注册自己的处理逻辑。
*/
static void nimble_host_config_init(void) {
    /* Set host callbacks */
    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    /*
        存储状态回调。当 NimBLE 的存储层（ble_store）操作失败或需要应用决策时调用。
ble_store_util_status_rr 是 NimBLE 库提供的默认实现，名字里的 rr 表示 “return reason” 或 “read/remove”。
它的行为：打印存储状态，返回合适的错误码，让协议栈继续运行。
为什么用库提供的实现而不是自己写？ 存储状态处理涉及 bonding 信息的读写失败、存储满、数据损坏等情况，逻辑复杂。
默认实现足够示例使用。产品中如果需要对存储失败做特殊处理（如清理旧配对、提示用户），可以替换成自己的回调。
    */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Store host configuration
    配置存储层的具体实现。NimBLE 的 ble_store 是抽象接口，ble_store_config_init() 把接口绑定到 ESP-IDF 的 NVS 存储后端。
    和 store_status_cb 的区别：
        ble_store_config_init()：配置“存储后端是什么”（NVS）。
        store_status_cb：配置“存储操作出问题时怎么处理”。
        两者配合，让 NimBLE 能持久化配对信息并处理异常。
    调用时机：必须在 nimble_port_init() 之后、nimble_port_run() 之前。
        因为 ble_hs_cfg 在 nimble_port_init() 中初始化，提前赋值可能被覆盖。
    */  
    ble_store_config_init();
}

//协议栈事件循环任务。这是 NimBLE Host 的任务入口，由 app_main 用 xTaskCreate 创建
static void nimble_host_task(void *param) {
    /* Task entry log */
    ESP_LOGI(TAG, "nimble host task has been started!");

    /* This function won't return until nimble_port_stop() is executed。
        Host 的事件循环。
            内部是一个无限循环，阻塞等待 HCI 事件（来自 Controller）和应用事件（来自其他任务），分发到对应回调。
        不会返回，直到 nimble_port_stop() 被调用。nimble_port_stop() 通常在关闭 BLE 时调用（如进入低功耗、切换模式）。
        为什么必须独立任务？ 因为它阻塞。
            如果放在 app_main 里，app_main 永远不返回，后续初始化无法进行。
            独立任务让协议栈事件处理与应用主流程并行。
    */
    nimble_port_run();

    /* Clean up at exit 
        删除当前任务。NULL 表示删除自己。
        执行时机：只有 nimble_port_run() 返回后才会执行。正常情况下不会到达，因为 nimble_port_run() 不返回。
        这是防御性清理：如果将来调用 nimble_port_stop() 让循环退出，任务能正确删除自己，释放资源。
        不调用会怎样？ 任务函数返回后，FreeRTOS 的行为未定义（通常触发断言或异常）。必须显式删除。
    */
    vTaskDelete(NULL);
}

void app_main(void) {
    /* Local variables */
    int rc = 0;
    esp_err_t ret = ESP_OK;

    /* NVS flash initialization NVS 初始化 */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nvs flash, error code: %d ", ret);
        return;
    }

    /* NimBLE host stack initialization
        NimBLE 协议栈初始化，nimble_port_init() 做什么：
        1 初始化 NimBLE Host（ble_hs_init()）：创建 Host 内部结构、事件队列、默认配置。
        2 初始化 Controller（esp_bt_controller_init()）：加载 Controller 固件、读校准数据、配置射频。
        3 启用 Controller（esp_bt_controller_enable()）：启动链路层。
        4 建立 Host 和 Controller 之间的 HCI 通道：Host 通过 HCI 命令控制 Controller。
    初始化 ble_hs_cfg 为默认值。
    返回 esp_err_t：失败可能因为 NVS 未初始化、Controller 固件加载失败、内存不足、HCI 通道建立失败。
    失败直接 return：协议栈没起来，后续 GAP 初始化、任务创建都无意义
    */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nimble stack, error code: %d ",
                 ret);
        return;
    }

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    /* GAP service initialization */
    printf("gap 初始化。。。\n");
    rc = gap_init();
    printf("gap 初始化完成\n");
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to initialize GAP service, error code: %d", rc);
        return;
    }
#endif

    /* NimBLE host configuration initialization 
    调用前面定义的函数，注册 reset_cb、sync_cb、store_status_cb，并调用 ble_store_config_init()。
        必须在 nimble_port_init() 之后：因为 ble_hs_cfg 在 nimble_port_init() 中被初始化为默认值，提前赋值会被覆盖。
        必须在 nimble_port_run() 之前：因为 sync_cb 在协议栈同步完成后触发，如果任务启动后回调还没注册，
            会错过 sync_cb，导致 adv_init() 不被调用，BLE 不广播。
    */
    nimble_host_config_init();

    /* Start NimBLE host task thread and return 
        创建 NimBLE Host 任务
        xTaskCreate 参数：
            nimble_host_task：任务入口函数。
            "NimBLE Host"：任务名，调试时在 FreeRTOS 任务列表中可见。
            4*1024：栈大小，4KB。单位是字节（ESP-IDF 中 xTaskCreate 的栈大小单位是字节，不是字）。
                4KB 是 NimBLE Host 任务的典型值——Host 处理 GATT、GAP、SM 等协议，需要一定栈空间。
                太小会栈溢出，太大浪费 RAM。
            NULL：任务参数，传给 nimble_host_task 的 param。
            5：任务优先级。FreeRTOS 优先级数值越大越高。
                5 是中等优先级，高于空闲任务（0），低于系统关键任务（如 WiFi 任务通常 23）。
                BLE 事件处理不需要最高优先级，5 足够。
            NULL：任务句柄输出，本示例不需要后续操作任务，传 NULL。
    */
    xTaskCreate(nimble_host_task, "NimBLE Host", 4*1024, NULL, 5, NULL);
    return;
}
