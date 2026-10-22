/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * 本文件实现 GATT Server 的核心业务逻辑。
 *
 * 本例一共定义两个 Primary Service：
 * 1. Heart Rate Service（0x180D）
 *    - Heart Rate Measurement Characteristic（0x2A37）
 *    - 支持 READ 和 INDICATE
 *
 * 2. Automation IO Service（0x1815）
 *    - 自定义 LED Characteristic
 *    - 支持 WRITE
 *
 * 可以把 GATT 理解成：
 * Server -> Service -> Characteristic
 * 手机连接 ESP32 后，通过 Characteristic 真正完成数据读写。
 */

/* 头文件 */
#include "gatt_svc.h"
#include "common.h"
#include "heart_rate.h"
#include "led.h"

/* 本文件内部使用的 Characteristic 访问回调函数 */
static int heart_rate_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg);
static int led_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

/* ========================= Heart Rate Service ========================= */

/*
 * 标准 Heart Rate Service UUID：0x180D。
 * 这是 Bluetooth SIG 定义的标准 16-bit UUID，不是本例自定义的。
 */
static const ble_uuid16_t heart_rate_svc_uuid = BLE_UUID16_INIT(0x180D);

/*
 * Heart Rate Measurement 的实际值缓冲区。
 * 本例使用 2 字节：
 *   [0] = Flags
 *   [1] = 当前心率值（uint8_t）
 */
static uint8_t heart_rate_chr_val[2] = {0};

/*
 * Characteristic 的 Value Handle。
 * Handle 是 NimBLE 在注册 GATT 数据库时分配的运行时编号。
 * 后续 READ / INDICATE 等操作都会用到这个 handle。
 */
static uint16_t heart_rate_chr_val_handle;

/* 标准 Heart Rate Measurement Characteristic UUID：0x2A37 */
static const ble_uuid16_t heart_rate_chr_uuid = BLE_UUID16_INIT(0x2A37);

/*
 * 保存订阅 Heart Rate Indication 的客户端连接信息。
 * conn_handle 表示“哪一条 BLE 连接”。
 */
static uint16_t heart_rate_chr_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool heart_rate_chr_conn_handle_inited = false;

/* 当前客户端是否已经打开 Heart Rate Indication 订阅 */
static bool heart_rate_ind_status = false;

/* ========================= Automation IO Service ========================= */

/* 标准 Automation IO Service UUID：0x1815 */
static const ble_uuid16_t auto_io_svc_uuid = BLE_UUID16_INIT(0x1815);

/* LED Characteristic 的 Value Handle */
static uint16_t led_chr_val_handle;

/*
 * LED Characteristic 使用自定义 128-bit UUID。
 * 与 0x180D、0x2A37 这种标准 UUID 不同，这个 UUID 是本例自定义业务使用的。
 */
static const ble_uuid128_t led_chr_uuid =
    BLE_UUID128_INIT(0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15, 0xde, 0xef,
                     0x12, 0x12, 0x25, 0x15, 0x00, 0x00);

/*
 * ========================= GATT Service 表 =========================
 *
 * 这是整个 GATT Server 最核心的数据结构。
 * 可以把它理解为“ESP32 对外提供哪些 BLE 业务接口”的总表。
 *
 * 结构关系如下：
 *
 * GATT Server
 * ├── Heart Rate Service (0x180D)
 * │   └── Heart Rate Measurement (0x2A37)
 * │       ├── READ
 * │       └── INDICATE
 * │
 * └── Automation IO Service (0x1815)
 *     └── LED Characteristic (128-bit UUID)
 *         └── WRITE
 */
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    /* Heart Rate Primary Service */
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &heart_rate_svc_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {
              /* Heart Rate Measurement Characteristic */
              .uuid = &heart_rate_chr_uuid.u,

              /*
               * 当客户端 READ 该 Characteristic 时，
               * NimBLE 会调用 heart_rate_chr_access()。
               */
              .access_cb = heart_rate_chr_access,

              /* 允许客户端读取，并允许 Server 主动发送 Indication */
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_INDICATE,

              /* NimBLE 注册后会把实际 Value Handle 写入该变量 */
              .val_handle = &heart_rate_chr_val_handle},
             {
                 0, /* 0 表示这个 Service 后面没有更多 Characteristic */
             }}},

    /* Automation IO Primary Service */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &auto_io_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                 /* LED Characteristic */
                 .uuid = &led_chr_uuid.u,

                 /* 客户端写入时调用 led_chr_access() */
                 .access_cb = led_chr_access,

                 /* 该 Characteristic 只允许 WRITE */
                 .flags = BLE_GATT_CHR_F_WRITE,

                 /* 保存注册完成后的 Value Handle */
                 .val_handle = &led_chr_val_handle},
                {0}},
    },

    {
        0, /* 0 表示 GATT Service 表结束 */
    },
};

/*
 * Heart Rate Characteristic 访问回调。
 *
 * conn_handle：当前是哪一条 BLE 连接在访问
 * attr_handle：访问的是哪个 GATT Attribute
 * ctxt：本次访问的上下文，包含 READ/WRITE 类型和数据缓冲区
 * arg：用户自定义参数，本例没有使用
 */
static int heart_rate_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    int rc = 0;

    /*
     * 根据 ctxt->op 判断这次访问是什么类型。
     * 本 Characteristic 只开放 READ，所以只处理 READ_CHR。
     */
    switch (ctxt->op) {

    /* 客户端读取 Heart Rate Characteristic */
    case BLE_GATT_ACCESS_OP_READ_CHR:
        /* 打印是谁在读取，以及读取哪个 Attribute */
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "characteristic read; conn_handle=%d attr_handle=%d",
                     conn_handle, attr_handle);
        } else {
            ESP_LOGI(TAG, "characteristic read by nimble stack; attr_handle=%d",
                     attr_handle);
        }

        /*
         * 确认当前访问的 Attribute 确实是 Heart Rate 的 Value Handle。
         */
        if (attr_handle == heart_rate_chr_val_handle) {
            /*
             * Heart Rate Measurement 第 0 字节是 Flags。
             * heart_rate_chr_val 初始化为 {0, 0}，所以 Flags 保持为 0：
             * - 心率值使用 uint8_t
             * - 不启用额外字段
             *
             * 第 1 字节填入当前模拟心率。
             */
            heart_rate_chr_val[1] = get_heart_rate();

            /*
             * ctxt->om 是 NimBLE 的 mbuf 数据缓冲区。
             * 把 2 字节心率数据追加进去，NimBLE 会把它作为 READ 响应返回给手机。
             */
            rc = os_mbuf_append(ctxt->om, &heart_rate_chr_val,
                                sizeof(heart_rate_chr_val));

            /* mbuf 写入成功返回 0，否则返回 ATT 资源不足错误 */
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        goto error;

    /* 收到本例没有定义的访问操作 */
    default:
        goto error;
    }

error:
    ESP_LOGE(
        TAG,
        "unexpected access operation to heart rate characteristic, opcode: %d",
        ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
}

/*
 * LED Characteristic 访问回调。
 * 本 Characteristic 只支持 WRITE，用于让手机远程控制 ESP32 上的 LED。
 */
static int led_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    /* 根据访问类型处理，本例只允许 WRITE */
    switch (ctxt->op) {

    /* 客户端向 LED Characteristic 写数据 */
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        /* 打印连接和 Attribute 信息 */
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "characteristic write; conn_handle=%d attr_handle=%d",
                     conn_handle, attr_handle);
        } else {
            ESP_LOGI(TAG,
                     "characteristic write by nimble stack; attr_handle=%d",
                     attr_handle);
        }

        /* 确认写入目标确实是 LED Characteristic */
        if (attr_handle == led_chr_val_handle) {
            /*
             * 本例要求客户端恰好写入 1 字节。
             * 注意：这里不是接收字符串 "ON" / "OFF"，因为那分别是 2/3 字节。
             */
            if (ctxt->om->om_len == 1) {
                /*
                 * 读取第 1 个字节：
                 * 0     -> 关灯
                 * 非 0  -> 开灯
                 */
                if (ctxt->om->om_data[0]) {
                    led_on();
                    ESP_LOGI(TAG, "led turned on!");
                } else {
                    led_off();
                    ESP_LOGI(TAG, "led turned off!");
                }
            } else {
                goto error;
            }
            return 0;
        }
        goto error;

    /* 收到本例没有定义的访问操作 */
    default:
        goto error;
    }

error:
    ESP_LOGE(TAG,
             "unexpected access operation to led characteristic, opcode: %d",
             ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
}

/*
 * 尝试向已经订阅 Heart Rate Indication 的客户端主动发送心率。
 *
 * 只有同时满足下面两个条件才真正发送：
 * 1. heart_rate_ind_status == true：客户端已开启 Indication；
 * 2. heart_rate_chr_conn_handle_inited == true：已经记录有效连接。
 */
void send_heart_rate_indication(void) {
    if (heart_rate_ind_status && heart_rate_chr_conn_handle_inited) {
        /*
         * 使用当前连接 handle 和 Heart Rate Value Handle 发送 Indication。
         * Indication 与 Notification 的主要区别是：Indication 需要客户端确认。
         */
        ble_gatts_indicate(heart_rate_chr_conn_handle,
                           heart_rate_chr_val_handle);
        ESP_LOGI(TAG, "heart rate indication sent!");
    }
}

/*
 * GATT Attribute 注册回调。
 * NimBLE 在启动时注册 Service、Characteristic、Descriptor，
 * 每注册一个对象都会调用这里，主要用于打印调试信息。
 */
void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg) {
    char buf[BLE_UUID_STR_LEN];

    /* 根据注册对象类型分别处理 */
    switch (ctxt->op) {

    /* 注册 Service */
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGD(TAG, "registered service %s with handle=%d",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                 ctxt->svc.handle);
        break;

    /* 注册 Characteristic */
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGD(TAG,
                 "registering characteristic %s with "
                 "def_handle=%d val_handle=%d",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                 ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;

    /* 注册 Descriptor，例如 Indication/Notification 常会涉及 CCCD */
    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGD(TAG, "registering descriptor %s with handle=%d",
                 ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                 ctxt->dsc.handle);
        break;

    /* 理论上不应该出现其他操作类型 */
    default:
        assert(0);
        break;
    }
}

/*
 * GATT 订阅事件处理函数。
 *
 * 当手机在 nRF Connect 中点击 Subscribe，开启或关闭 Indication 时，
 * GAP 层会收到 BLE_GAP_EVENT_SUBSCRIBE，随后转到这里处理。
 */
void gatt_svr_subscribe_cb(struct ble_gap_event *event) {
    /* 打印订阅事件来自哪一条连接、哪个 Attribute */
    if (event->subscribe.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "subscribe event; conn_handle=%d attr_handle=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle);
    } else {
        ESP_LOGI(TAG, "subscribe by nimble stack; attr_handle=%d",
                 event->subscribe.attr_handle);
    }

    /*
     * 只关心 Heart Rate Characteristic 的订阅状态。
     * attr_handle 表示 GATT 数据库中的具体 Attribute 编号。
     */
    if (event->subscribe.attr_handle == heart_rate_chr_val_handle) {
        /* 记录哪个客户端订阅了心率 */
        heart_rate_chr_conn_handle = event->subscribe.conn_handle;
        heart_rate_chr_conn_handle_inited = true;

        /*
         * cur_indicate 表示客户端当前是否启用了 Indication。
         * true  -> 后台心率任务可以主动发送
         * false -> 不再发送
         */
        heart_rate_ind_status = event->subscribe.cur_indicate;
    }
}

/*
 * 清除 Heart Rate 订阅状态。
 * 连接断开以后，旧的 conn_handle 已经失效，必须把状态清零，
 * 否则后台任务可能还会错误地尝试向旧连接发送 Indication。
 */
void gatt_svr_reset_heart_rate_subscription(void) {
    heart_rate_chr_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    heart_rate_chr_conn_handle_inited = false;
    heart_rate_ind_status = false;
}

/*
 * 初始化 GATT Server。
 *
 * 主要分三步：
 * 1. 初始化标准 GATT Service；
 * 2. 根据 gatt_svr_svcs 统计需要多少 GATT 资源；
 * 3. 把自定义 Service 表加入 NimBLE GATT Server。
 */
int gatt_svc_init(void) {
    int rc = 0;

    /* 第 1 步：初始化 NimBLE 自带的标准 Generic Attribute Service */
    ble_svc_gatt_init();

    /*
     * 第 2 步：统计 gatt_svr_svcs 中 Service / Characteristic / Descriptor 数量，
     * 让 NimBLE 提前准备所需资源。
     */
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    /* 第 3 步：把 Heart Rate 和 Automation IO Service 加入 GATT Server */
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    return 0;
}
