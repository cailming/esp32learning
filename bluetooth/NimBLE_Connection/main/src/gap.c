/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * 本文件负责 BLE 的 GAP（Generic Access Profile，通用访问规范）相关逻辑。
 *
 * 对这个示例来说，GAP 主要负责 4 件事：
 * 1. 设置 ESP32 的 BLE 设备名称和设备类型；
 * 2. 配置广播数据和扫描响应数据；
 * 3. 启动可连接广播，让手机能发现并连接 ESP32；
 * 4. 处理连接、断开、连接参数更新等 GAP 事件。
 */

/* 头文件 */
#include "gap.h"
#include "common.h"
#include "led.h"

/* 本文件内部函数声明，static 表示只在 gap.c 内部使用 */
inline static void format_addr(char *addr_str, uint8_t addr[]);
static void print_conn_desc(struct ble_gap_conn_desc *desc);
static void start_advertising(void);
static int gap_event_handler(struct ble_gap_event *event, void *arg);

/*
 * 本机用于广播的地址类型。
 * BLE 地址可能是 public address、random address 等不同类型，
 * own_addr_type 用于告诉 NimBLE 当前广播应该使用哪一种地址类型。
 */
static uint8_t own_addr_type;

/* 保存 ESP32 当前使用的 6 字节 BLE 地址 */
static uint8_t addr_val[6] = {0};

/*
 * 广播扫描响应中携带的 URI 数据。
 * BLE_GAP_URI_PREFIX_HTTPS 表示 URI 前缀是 https:，
 * 后面的字符拼起来就是 //espressif.com，
 * 所以最终表示的是 https://espressif.com。
 */
static uint8_t esp_uri[] = {BLE_GAP_URI_PREFIX_HTTPS, '/', '/', 'e', 's', 'p', 'r', 'e', 's', 's', 'i', 'f', '.', 'c', 'o', 'm'};

/*
 * 将 6 字节 BLE 地址转换成人类容易阅读的字符串形式。
 * 例如：AA:BB:CC:DD:EE:FF
 */
inline static void format_addr(char *addr_str, uint8_t addr[]) {
    sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1],
            addr[2], addr[3], addr[4], addr[5]);
}

/*
 * 打印当前 BLE 连接的详细信息。
 * desc 是 NimBLE 提供的“连接描述结构体”，里面保存连接句柄、地址、
 * 连接间隔、延迟、监督超时、安全状态等信息。
 */
static void print_conn_desc(struct ble_gap_conn_desc *desc) {
    /* 用于保存格式化后的 BLE 地址字符串，17 个字符 + 结尾 '\0' */
    char addr_str[18] = {0};

    /*
     * 连接句柄（connection handle）。
     * 可以把它理解为 NimBLE 为当前连接分配的“连接编号”。
     * 后续查连接、更新参数等操作都要通过这个 handle 指定具体连接。
     */
    ESP_LOGI(TAG, "connection handle: %d", desc->conn_handle);

    /* 打印本机的身份地址 */
    format_addr(addr_str, desc->our_id_addr.val);
    ESP_LOGI(TAG, "device id address: type=%d, value=%s",
             desc->our_id_addr.type, addr_str);

    /* 打印对端设备（例如手机）的身份地址 */
    format_addr(addr_str, desc->peer_id_addr.val);
    ESP_LOGI(TAG, "peer id address: type=%d, value=%s", desc->peer_id_addr.type,
             addr_str);

    /*
     * 打印连接参数和安全状态：
     * conn_itvl            ：连接间隔
     * conn_latency         ：Peripheral latency，从机允许跳过的连接事件数量
     * supervision_timeout  ：监督超时，超过该时间收不到有效数据则认为连接断开
     * encrypted            ：连接是否已加密
     * authenticated        ：连接是否已认证
     * bonded               ：双方是否已建立绑定关系
     */
    ESP_LOGI(TAG,
             "conn_itvl=%d, conn_latency=%d, supervision_timeout=%d, "
             "encrypted=%d, authenticated=%d, bonded=%d\n",
             desc->conn_itvl, desc->conn_latency, desc->supervision_timeout,
             desc->sec_state.encrypted, desc->sec_state.authenticated,
             desc->sec_state.bonded);
}

/*
 * 配置并启动 BLE 广播。
 *
 * BLE 广播阶段可以先简单理解为：
 * ESP32 不断向周围“喊”：
 * “我是 NimBLE_CONN，我可以被发现，也可以连接。”
 * 手机上的 nRF Connect 扫描到这些广播包后，就能显示这个设备。
 */
static void start_advertising(void) {
    /* rc 保存 NimBLE API 的返回值，0 通常表示成功 */
    int rc = 0;

    /* 指向 GAP 中已经配置好的设备名称 */
    const char *name;

    /*
     * 广播数据字段结构体。
     * 广播包本身容量有限，因此需要把要广播的信息放进这个结构体，
     * 再由 ble_gap_adv_set_fields() 编码成真正的 BLE 广播数据。
     */
    struct ble_hs_adv_fields adv_fields = {0};

    /*
     * 扫描响应数据字段结构体。
     * 当扫描端使用主动扫描（Active Scan）时，会向 ESP32 发 Scan Request，
     * ESP32 可以再返回一份 Scan Response，用来携带额外信息。
     */
    struct ble_hs_adv_fields rsp_fields = {0};

    /* 广播参数，例如是否允许连接、广播间隔等 */
    struct ble_gap_adv_params adv_params = {0};

    /*
     * 设置广播 Flags：
     * BLE_HS_ADV_F_DISC_GEN  ：General Discoverable，一般可发现模式
     * BLE_HS_ADV_F_BREDR_UNSUP：不支持经典蓝牙 BR/EDR，只支持 BLE
     */
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /*
     * 设置广播中的设备名称。
     * ble_svc_gap_device_name() 获取之前在 gap_init() 中设置的 DEVICE_NAME。
     */
    name = ble_svc_gap_device_name();
    adv_fields.name = (uint8_t *)name;
    adv_fields.name_len = strlen(name);

    /* 1 表示这里广播的是完整设备名称，而不是缩写名称 */
    adv_fields.name_is_complete = 1;

    /*
     * 将发送功率（TX Power）字段加入广播数据。
     * AUTO 表示由 NimBLE 自动填写当前发送功率。
     */
    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    adv_fields.tx_pwr_lvl_is_present = 1;

    /*
     * 设置设备 Appearance（外观类型）。
     * 这里把 ESP32 声明为 Generic Tag（通用标签设备）。
     */
    adv_fields.appearance = BLE_GAP_APPEARANCE_GENERIC_TAG;
    adv_fields.appearance_is_present = 1;

    /*
     * 设置设备的 LE Role 为 Peripheral（外设角色）。
     * 在本示例中：
     * ESP32 = Peripheral（外设）
     * 手机   = Central（中心设备）
     */
    adv_fields.le_role = BLE_GAP_LE_ROLE_PERIPHERAL;
    adv_fields.le_role_is_present = 1;

    /*
     * 把上面准备好的字段真正写入广播数据。
     * 如果广播数据超过 BLE 广播包容量等原因导致失败，就打印错误并返回。
     */
    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set advertising data, error code: %d", rc);
        return;
    }

    /*
     * 以下字段放入 Scan Response（扫描响应），而不是主广播包。
     */

    /* 在扫描响应中携带本机 BLE 地址 */
    rsp_fields.device_addr = addr_val;
    rsp_fields.device_addr_type = own_addr_type;
    rsp_fields.device_addr_is_present = 1;

    /* 在扫描响应中携带 URI：https://espressif.com */
    rsp_fields.uri = esp_uri;
    rsp_fields.uri_len = sizeof(esp_uri);

    /*
     * 在扫描响应中声明建议/实际使用的广播间隔为 500 ms。
     * BLE 协议内部的广播间隔单位不是毫秒，这里通过宏转换。
     */
    rsp_fields.adv_itvl = BLE_GAP_ADV_ITVL_MS(500);
    rsp_fields.adv_itvl_is_present = 1;

    /* 将扫描响应字段交给 NimBLE */
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set scan response data, error code: %d", rc);
        return;
    }

    /*
     * 设置广播为 Undirected Connectable（无定向可连接广播）。
     * “无定向”表示不限定某个特定 Central，任何扫描到它的 Central 都可以尝试连接。
     */
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;

    /* 设置为 General Discoverable（一般可发现模式） */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /*
     * 设置实际广播间隔范围：500 ms ~ 510 ms。
     * BLE 控制器会在这个范围内安排广播事件。
     */
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(500);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(510);

    /*
     * 正式启动广播。
     *
     * 参数含义：
     * own_addr_type       ：广播使用的本机地址类型
     * NULL                ：不指定某个固定对端地址，因为是无定向广播
     * BLE_HS_FOREVER      ：一直广播，直到连接成功或代码主动停止
     * &adv_params         ：前面配置的广播参数
     * gap_event_handler   ：GAP 事件回调函数
     * NULL                ：传给回调函数的自定义参数，这里不用
     *
     * 非常关键：
     * 从这一刻开始，连接、断开、参数更新等事件，都会进入 gap_event_handler()。
     */
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                           gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to start advertising, error code: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "advertising started!");
}

/*
 * NimBLE 使用“事件驱动”模型处理 GAP。
 *
 * 可以把 gap_event_handler() 理解成 BLE 连接状态的统一消息入口：
 * 当发生连接成功/失败、断开、连接参数变化等事情时，
 * NimBLE 会自动调用这个函数，并通过 event->type 告诉我们发生了什么。
 *
 * 这个回调是在 ble_gap_adv_start() 中注册的。
 */
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    /* rc 保存处理结果 */
    int rc = 0;

    /*
     * 用于保存某个连接的完整描述信息。
     * 通过 ble_gap_conn_find()，可以根据 connection handle 获取它。
     */
    struct ble_gap_conn_desc desc;

    /* 根据不同的 GAP 事件类型分别处理 */
    switch (event->type) {

    /* ==================== 1. 连接事件 ==================== */
    case BLE_GAP_EVENT_CONNECT:
        /*
         * 这个事件既可能表示“连接成功”，也可能表示“连接尝试失败”。
         * status == 0 表示成功，非 0 表示失败。
         */
        ESP_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);

        /* 连接成功 */
        if (event->connect.status == 0) {
            /*
             * 根据刚建立连接的 connection handle 查询连接详细信息。
             */
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc != 0) {
                ESP_LOGE(TAG,
                         "failed to find connection by handle, error code: %d",
                         rc);
                return rc;
            }

            /* 打印连接信息，并点亮 LED 表示当前已经连接 */
            print_conn_desc(&desc);
            led_on();

            /*
             * 尝试更新连接参数。
             *
             * itvl_min / itvl_max：
             *   这里保持当前连接间隔不变。
             *
             * latency = 3：
             *   允许 Peripheral 在没有数据发送时跳过最多 3 个连接事件，
             *   有助于降低功耗。
             *
             * supervision_timeout：
             *   保持当前监督超时不变。
             */
            struct ble_gap_upd_params params = {.itvl_min = desc.conn_itvl,
                                                .itvl_max = desc.conn_itvl,
                                                .latency = 3,
                                                .supervision_timeout =
                                                    desc.supervision_timeout};

            /* 向对端请求更新连接参数 */
            rc = ble_gap_update_params(event->connect.conn_handle, &params);
            if (rc != 0) {
                ESP_LOGE(
                    TAG,
                    "failed to update connection parameters, error code: %d",
                    rc);
                return rc;
            }
        }
        /*
         * 如果连接尝试失败，ESP32 已经不再处于原来的广播流程中，
         * 因此重新启动广播，继续等待下一次手机连接。
         */
        else {
            start_advertising();
        }
        return rc;

    /* ==================== 2. 断开事件 ==================== */
    case BLE_GAP_EVENT_DISCONNECT:
        /* 打印连接断开的原因码 */
        ESP_LOGI(TAG, "disconnected from peer; reason=%d",
                 event->disconnect.reason);

        /* 熄灭 LED，表示当前已没有连接 */
        led_off();

        /*
         * 断开后重新广播，这样手机还能再次扫描到 ESP32 并重新连接。
         */
        start_advertising();
        return rc;

    /* ==================== 3. 连接参数更新事件 ==================== */
    case BLE_GAP_EVENT_CONN_UPDATE:
        /*
         * 当连接参数更新完成后，NimBLE 会发送这个事件。
         * status 用于表示参数更新是否成功。
         */
        ESP_LOGI(TAG, "connection updated; status=%d",
                 event->conn_update.status);

        /*
         * 根据连接句柄重新获取最新的连接描述，
         * 这样可以看到新的 conn_itvl、conn_latency、timeout 等值。
         */
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        if (rc != 0) {
            ESP_LOGE(TAG, "failed to find connection by handle, error code: %d",
                     rc);
            return rc;
        }

        /* 打印更新后的连接信息 */
        print_conn_desc(&desc);
        return rc;
    }

    /* 其他本示例没有处理的 GAP 事件直接忽略 */
    return rc;
}

/* ==================== 对外提供的函数 ==================== */

/*
 * 广播初始化函数。
 * 这个函数会在 NimBLE Host 与 Controller 完成同步后，由 on_stack_sync() 调用。
 */
void adv_init(void) {
    /* rc 保存 NimBLE API 返回值 */
    int rc = 0;

    /* 保存格式化后的本机 BLE 地址字符串 */
    char addr_str[18] = {0};

    /*
     * 确保设备已经有可用的 BLE Identity Address（身份地址）。
     * 如果没有有效地址，就无法正常进行 BLE 广播和连接。
     */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "device does not have any available bt address!");
        return;
    }

    /*
     * 让 NimBLE 自动判断应该使用哪种本机地址类型进行广播，
     * 并把结果保存到 own_addr_type。
     */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer address type, error code: %d", rc);
        return;
    }

    /*
     * 根据地址类型读取实际的 6 字节 BLE 地址，保存到 addr_val。
     */
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to copy device address, error code: %d", rc);
        return;
    }

    /* 将地址转换成字符串并输出到日志，方便调试 */
    format_addr(addr_str, addr_val);
    ESP_LOGI(TAG, "device address: %s", addr_str);

    /* 地址准备完成后，正式开始广播 */
    start_advertising();
}

/*
 * 初始化 GAP 服务。
 * GAP 是 BLE 中负责设备发现、广播、连接等基础行为的一层。
 */
int gap_init(void) {
    /* rc 保存 NimBLE API 返回值 */
    int rc = 0;

    /* 初始化 NimBLE 自带的 GAP Service */
    ble_svc_gap_init();

    /*
     * 设置 BLE 设备名称。
     * DEVICE_NAME 在 common.h 中定义为 "NimBLE_CONN"。
     * 手机扫描 BLE 设备时通常会看到这个名字。
     */
    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device name to %s, error code: %d",
                 DEVICE_NAME, rc);
        return rc;
    }

    /*
     * 设置设备 Appearance（外观类型）为 Generic Tag。
     * Appearance 是 BLE 标准中用于描述“设备大概是什么类型”的字段。
     */
    rc = ble_svc_gap_device_appearance_set(BLE_GAP_APPEARANCE_GENERIC_TAG);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device appearance, error code: %d", rc);
        return rc;
    }

    return rc;
}
