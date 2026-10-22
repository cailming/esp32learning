/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * 本文件负责 BLE 的 GAP（Generic Access Profile）部分。
 *
 * GAP 主要解决：
 * 1. ESP32 如何广播自己；
 * 2. 手机如何发现并连接 ESP32；
 * 3. 连接成功、断开、参数更新、订阅等事件如何处理。
 *
 * 与上一节 NimBLE_Connection 相比，本例新增了 GATT 相关事件：
 * - BLE_GAP_EVENT_SUBSCRIBE：客户端订阅 Characteristic；
 * - BLE_GAP_EVENT_NOTIFY_TX：Notification / Indication 发送状态；
 * - BLE_GAP_EVENT_MTU：ATT MTU 更新；
 * - BLE_GAP_EVENT_ADV_COMPLETE：广播结束。
 */

/* 头文件 */
#include "gap.h"
#include "common.h"
#include "gatt_svc.h"

/* 本文件内部函数声明 */
inline static void format_addr(char *addr_str, uint8_t addr[]);
static void print_conn_desc(struct ble_gap_conn_desc *desc);
static void start_advertising(void);
static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* 本机 BLE 地址类型，例如 public/random address */
static uint8_t own_addr_type;

/* 保存 ESP32 当前使用的 6 字节 BLE 地址 */
static uint8_t addr_val[6] = {0};

/*
 * 广播 Scan Response 中携带的 URI：
 * https://espressif.com
 * 第一个字节是 BLE 标准中 HTTPS 的 URI 前缀编码。
 */
static uint8_t esp_uri[] = {BLE_GAP_URI_PREFIX_HTTPS, '/', '/', 'e', 's', 'p', 'r', 'e', 's', 's', 'i', 'f', '.', 'c', 'o', 'm'};

/*
 * 把 6 字节 BLE 地址转换成 AA:BB:CC:DD:EE:FF 这种字符串形式，
 * 方便串口日志查看。
 */
inline static void format_addr(char *addr_str, uint8_t addr[]) {
    sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1],
            addr[2], addr[3], addr[4], addr[5]);
}

/*
 * 打印一条 BLE 连接的详细信息。
 * desc 是 NimBLE 提供的连接描述结构体。
 */
static void print_conn_desc(struct ble_gap_conn_desc *desc) {
    char addr_str[18] = {0};

    /* connection handle：这条 BLE 连接的运行时编号 */
    ESP_LOGI(TAG, "connection handle: %d", desc->conn_handle);

    /* 打印本机 Identity Address */
    format_addr(addr_str, desc->our_id_addr.val);
    ESP_LOGI(TAG, "device id address: type=%d, value=%s",
             desc->our_id_addr.type, addr_str);

    /* 打印对端（例如手机）的 Identity Address */
    format_addr(addr_str, desc->peer_id_addr.val);
    ESP_LOGI(TAG, "peer id address: type=%d, value=%s", desc->peer_id_addr.type,
             addr_str);

    /*
     * 打印连接参数和安全状态：
     * conn_itvl            -> Connection Interval
     * conn_latency         -> Peripheral Latency
     * supervision_timeout  -> 失联超时
     * encrypted            -> 是否已加密
     * authenticated        -> 是否已认证
     * bonded               -> 是否已绑定
     */
    ESP_LOGI(TAG,
             "conn_itvl=%d, conn_latency=%d, supervision_timeout=%d, "
             "encrypted=%d, authenticated=%d, bonded=%d\n",
             desc->conn_itvl, desc->conn_latency, desc->supervision_timeout,
             desc->sec_state.encrypted, desc->sec_state.authenticated,
             desc->sec_state.bonded);
}

/*
 * 真正配置并启动 BLE 广播。
 *
 * 这里需要准备三类数据：
 * 1. adv_fields：主动广播出去的数据；
 * 2. rsp_fields：手机主动扫描时返回的 Scan Response；
 * 3. adv_params：广播模式、是否可连接、广播间隔等参数。
 */
static void start_advertising(void) {
    int rc = 0;
    const char *name;
    struct ble_hs_adv_fields adv_fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    struct ble_gap_adv_params adv_params = {0};

    /*
     * 设置广播 Flags：
     * DISC_GEN     -> 一般可发现模式；
     * BREDR_UNSUP  -> 不支持经典蓝牙 BR/EDR，只使用 BLE。
     */
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /*
     * 把 GAP 中配置的设备名放进 Advertising Data。
     * 本例设备名为 NimBLE_GATT。
     */
    name = ble_svc_gap_device_name();
    adv_fields.name = (uint8_t *)name;
    adv_fields.name_len = strlen(name);
    adv_fields.name_is_complete = 1;

    /* 把设备发送功率信息放进广播包，由 NimBLE 自动填写 */
    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    adv_fields.tx_pwr_lvl_is_present = 1;

    /* 把设备外观类型设置为 Generic Tag */
    adv_fields.appearance = BLE_GAP_APPEARANCE_GENERIC_TAG;
    adv_fields.appearance_is_present = 1;

    /* 声明本设备在 BLE 中扮演 Peripheral（外设）角色 */
    adv_fields.le_role = BLE_GAP_LE_ROLE_PERIPHERAL;
    adv_fields.le_role_is_present = 1;

    /* 把上面准备好的 Advertising Data 交给 NimBLE */
    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set advertising data, error code: %d", rc);
        return;
    }

    /* Scan Response 中加入本机地址 */
    rsp_fields.device_addr = addr_val;
    rsp_fields.device_addr_type = own_addr_type;
    rsp_fields.device_addr_is_present = 1;

    /* Scan Response 中加入 https://espressif.com URI */
    rsp_fields.uri = esp_uri;
    rsp_fields.uri_len = sizeof(esp_uri);

    /* 在 Scan Response 中声明广告间隔为约 500 ms */
    rsp_fields.adv_itvl = BLE_GAP_ADV_ITVL_MS(500);
    rsp_fields.adv_itvl_is_present = 1;

    /* 把 Scan Response 数据交给 NimBLE */
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set scan response data, error code: %d", rc);
        return;
    }

    /*
     * 设置为 Undirected Connectable：
     * 不限定某个特定 Central，周围可发现的 Central 都可以尝试连接。
     */
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;

    /* 一般可发现模式 */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /* 广播间隔设置为约 500~510 ms */
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(500);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(510);

    /*
     * 启动广播。
     *
     * BLE_HS_FOREVER：持续广播，不主动超时停止；
     * gap_event_handler：后续 CONNECT / DISCONNECT / SUBSCRIBE 等 GAP 事件都回调到这里。
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
 * GAP 事件统一回调函数。
 *
 * NimBLE 使用事件驱动模型：
 * 程序不需要自己 while(1) 不断检查“连接了吗、断开了吗、订阅了吗”，
 * 而是当事件真正发生时，由 NimBLE 主动调用这个函数。
 */
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    int rc = 0;
    struct ble_gap_conn_desc desc;

    /* 根据 event->type 判断当前发生了哪类 GAP 事件 */
    switch (event->type) {

    /* ========================= 连接事件 ========================= */
    case BLE_GAP_EVENT_CONNECT:
        /* status == 0 表示连接成功，否则表示连接尝试失败 */
        ESP_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);

        if (event->connect.status == 0) {
            /*
             * 通过 conn_handle 查询这条连接的完整描述信息。
             * conn_handle 可以理解为当前 BLE 连接的 ID。
             */
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc != 0) {
                ESP_LOGE(TAG,
                         "failed to find connection by handle, error code: %d",
                         rc);
                return rc;
            }

            /* 打印连接地址、间隔、超时、安全状态等信息 */
            print_conn_desc(&desc);

            /*
             * 尝试更新连接参数：
             * - 保持当前 Connection Interval；
             * - 把 Peripheral Latency 设置为 3；
             * - 保持当前 supervision timeout。
             */
            struct ble_gap_upd_params params = {.itvl_min = desc.conn_itvl,
                                                .itvl_max = desc.conn_itvl,
                                                .latency = 3,
                                                .supervision_timeout =
                                                    desc.supervision_timeout};
            rc = ble_gap_update_params(event->connect.conn_handle, &params);
            if (rc != 0) {
                ESP_LOGE(
                    TAG,
                    "failed to update connection parameters, error code: %d",
                    rc);
                return rc;
            }
        }
        else {
            /* 连接失败后重新开始广播，继续等待下一次连接 */
            start_advertising();
        }
        return rc;

    /* ========================= 断开事件 ========================= */
    case BLE_GAP_EVENT_DISCONNECT:
        /* 打印断开原因 */
        ESP_LOGI(TAG, "disconnected from peer; reason=%d",
                 event->disconnect.reason);

        /*
         * 连接已经不存在，旧的 Heart Rate 订阅 conn_handle 已失效，
         * 因此清空所有心率订阅状态。
         */
        gatt_svr_reset_heart_rate_subscription();

        /* 重新广播，让手机之后还能再次发现和连接 ESP32 */
        start_advertising();
        return rc;

    /* ========================= 连接参数更新事件 ========================= */
    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "connection updated; status=%d",
                 event->conn_update.status);

        /* 查询并打印更新后的连接参数 */
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        if (rc != 0) {
            ESP_LOGE(TAG, "failed to find connection by handle, error code: %d",
                     rc);
            return rc;
        }
        print_conn_desc(&desc);
        return rc;

    /* ========================= 广播结束事件 ========================= */
    case BLE_GAP_EVENT_ADV_COMPLETE:
        /*
         * 广播如果因为某种原因结束，则记录原因并重新启动广播。
         * 本例正常情况下使用 BLE_HS_FOREVER 持续广播。
         */
        ESP_LOGI(TAG, "advertise complete; reason=%d",
                 event->adv_complete.reason);
        start_advertising();
        return rc;

    /* ========================= Notification / Indication 发送完成事件 ========================= */
    case BLE_GAP_EVENT_NOTIFY_TX:
        /*
         * 虽然事件名叫 NOTIFY_TX，但 NimBLE 也用它报告 Indication 的发送状态。
         * event->notify_tx.indication 可以判断本次是否为 Indication。
         */
        if ((event->notify_tx.status != 0) &&
            (event->notify_tx.status != BLE_HS_EDONE)) {
            ESP_LOGI(TAG,
                     "notify event; conn_handle=%d attr_handle=%d "
                     "status=%d is_indication=%d",
                     event->notify_tx.conn_handle, event->notify_tx.attr_handle,
                     event->notify_tx.status, event->notify_tx.indication);
        }
        return rc;

    /* ========================= Characteristic 订阅事件 ========================= */
    case BLE_GAP_EVENT_SUBSCRIBE:
        /*
         * 手机在 nRF Connect 中点击 Subscribe / Unsubscribe 时会触发该事件。
         * prev_notify / cur_notify 表示 Notification 前后状态；
         * prev_indicate / cur_indicate 表示 Indication 前后状态。
         */
        ESP_LOGI(TAG,
                 "subscribe event; conn_handle=%d attr_handle=%d "
                 "reason=%d prevn=%d curn=%d previ=%d curi=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.reason, event->subscribe.prev_notify,
                 event->subscribe.cur_notify, event->subscribe.prev_indicate,
                 event->subscribe.cur_indicate);

        /*
         * 从软件分层上看，订阅 Heart Rate 属于 GATT Server 业务，
         * 所以这里把事件转交给 gatt_svr_subscribe_cb() 继续处理。
         */
        gatt_svr_subscribe_cb(event);
        return rc;

    /* ========================= ATT MTU 更新事件 ========================= */
    case BLE_GAP_EVENT_MTU:
        /*
         * MTU 可以理解为一次 ATT/GATT 传输能承载的数据大小上限之一。
         * 本例只打印更新后的 MTU，不进一步使用。
         */
        ESP_LOGI(TAG, "mtu update event; conn_handle=%d cid=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.channel_id,
                 event->mtu.value);
        return rc;
    }

    return rc;
}

/*
 * 广播初始化入口。
 * 在 NimBLE Host 与 Controller 完成同步后，由 on_stack_sync() 调用。
 */
void adv_init(void) {
    int rc = 0;
    char addr_str[18] = {0};

    /*
     * 确保本设备存在可用的 BLE Identity Address。
     * 参数 0 表示这里不要求生成随机静态地址。
     */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "device does not have any available bt address!");
        return;
    }

    /* 让 NimBLE 自动推断广播时应该使用哪种本机地址类型 */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer address type, error code: %d", rc);
        return;
    }

    /* 把当前 BLE 地址复制到 addr_val，并打印出来 */
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to copy device address, error code: %d", rc);
        return;
    }
    format_addr(addr_str, addr_val);
    ESP_LOGI(TAG, "device address: %s", addr_str);

    /* 地址准备好后正式开始广播 */
    start_advertising();
}

/*
 * 初始化标准 GAP Service。
 * 本例主要在这里设置 BLE 设备名称。
 */
int gap_init(void) {
    int rc = 0;

    /* 初始化 NimBLE 自带的 Generic Access Service */
    ble_svc_gap_init();

    /*
     * 设置设备名为 DEVICE_NAME。
     * common.h 中定义为 "NimBLE_GATT"，手机扫描时会看到这个名称。
     */
    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device name to %s, error code: %d",
                 DEVICE_NAME, rc);
        return rc;
    }
    return rc;
}
