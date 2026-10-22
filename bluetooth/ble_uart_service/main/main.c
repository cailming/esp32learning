/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * ESP-BLE-UART 示例。
 *
 * 这个示例把 BLE 封装成类似“串口”的收发模型：
 * - 手机 / PC 作为 BLE Central（中心设备）连接 ESP32；
 * - Central 向 RX Characteristic 写入数据，相当于“给 ESP32 发串口数据”；
 * - ESP32 通过 TX Characteristic 的 Notify 把数据发回 Central；
 * - 本例收到 RX 数据后，会原样调用 ble_uart_tx() 发回去，因此是一个 echo（回显）示例。
 *
 * 底层 BLE Host 可以在编译时选择 NimBLE 或 Bluedroid。
 * 上层应用不直接依赖 NimBLE / Bluedroid API，而是统一通过 ble_uart.h 使用 BLE UART。
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

/*
 * BLE UART 组件对外提供的统一 API。
 * 这里面定义了 ble_uart_install()、ble_uart_open()、ble_uart_tx()、
 * ble_uart_config_t、各种连接/订阅/安全事件等。
 *
 * 注意：当前仓库中的这个示例通过根 CMakeLists.txt 引用 ../common/ble_uart，
 * 但该共享组件源码目前没有出现在仓库 main 分支中。
 */
#include "ble_uart.h"

#if CONFIG_EXAMPLE_CUSTOM_ADV_DATA
/*
 * 自定义 BLE 广播数据示例。
 *
 * 正常情况下 ble_uart 组件会生成默认广播内容；
 * 如果在 menuconfig 中启用了 CONFIG_EXAMPLE_CUSTOM_ADV_DATA，
 * 就把下面这组原始字节直接交给 ble_uart 作为广播 Payload。
 *
 * BLE 广播数据不是任意字节拼接，而是由一个个 AD Structure 组成：
 *
 *     [长度 length] [AD 类型] [数据 value]
 *
 * 其中 length 表示后面“AD 类型 + value”一共多少字节。
 *
 * 本示例的自定义广播中包含两项：
 * 1. Complete Local Name：完整设备名称 "BleUart"
 * 2. Complete List of 128-bit Service UUIDs：BLE UART Service 的 128 位 UUID
 *
 * ble_uart 组件还会自动在最前面加入 3 字节 Flags AD，
 * 所以这里自定义部分最大只能使用 BLE_UART_ADV_DATA_MAX（28）字节。
 * 如果超过限制，ble_uart_install() 会返回参数错误。
 */
static const uint8_t example_adv_payload[] = {
    /*
     * AD type 0x09：Complete Local Name（完整设备名称）。
     * 0x08 表示后面有 8 字节：1 字节 AD type + 7 字节字符串 "BleUart"。
     */
    0x08, 0x09, 'B', 'l', 'e', 'U', 'a', 'r', 't',

    /*
     * AD type 0x07：Complete List of 128-bit Service UUIDs。
     *
     * 0x11 = 17 字节：
     * 1 字节 AD type + 16 字节 UUID。
     *
     * UUID 在 BLE 空中数据包中按 little-endian（小端）顺序发送，
     * 对应 ble_uart_service_uuid.bytes[]。
     */
    0x11, 0x07,
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e,
};
#endif

/* ESP-IDF 日志 TAG，日志中会显示 "app" */
static const char *TAG = "app";

/*
 * RX 数据回调。
 *
 * 当 Central（例如手机）向 BLE UART 的 RX Characteristic 写入数据后，
 * ble_uart 组件会调用这个函数，并把收到的数据地址和长度传进来。
 *
 * 注意：
 * - data 指向收到的数据；
 * - len 是收到的字节数；
 * - 这个回调运行在 BLE Host Task 上，因此不应该在这里做长时间阻塞操作。
 */
static void ble_uart_on_rx(const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "rx len: %u bytes", (unsigned)len);

    /* 没有数据时直接返回，避免访问空指针 */
    if (data == NULL || len == 0) {
        return;
    }

    /* 以十六进制形式把收到的数据打印到串口日志 */
    ESP_LOG_BUFFER_HEX(TAG, data, len);

    /*
     * 把刚收到的数据原样通过 TX Characteristic 发回 Central。
     *
     * 因此本例形成：
     * Central 写 RX -> ESP32 收到 -> ESP32 Notify TX -> Central 收到原数据。
     * 这就是一个典型的 BLE UART echo（回显）测试。
     */
    ble_uart_tx(data, len);
}

/*
 * BLE UART 生命周期 / 链路状态事件回调。
 *
 * ble_uart 组件内部发生连接、断开、订阅、安全连接、配对等事件时，
 * 都会统一转换成 ble_uart_evt_t，然后调用本函数。
 *
 * 这个回调同样运行在 BLE Host Task 上，因此必须尽量短小：
 * - 不要长时间阻塞；
 * - 不要直接调用 ble_uart_close() / ble_uart_uninstall()，否则可能导致死锁；
 * - 如果确实要从回调里发起关闭，应调用 ble_uart_close_async()，
 *   再由普通应用任务在收到 CLOSED 事件后调用 ble_uart_uninstall()。
 *
 * 安全方面也要注意：
 * BLE_UART_EVT_CONNECTED 只代表物理 BLE 链路已经建立，
 * 并不一定代表链路已经完成加密 / 认证。
 * 如果业务数据必须经过安全链路，应等待 BLE_UART_EVT_LINK_SECURE。
 */
static void ble_uart_on_event(const ble_uart_evt_t *e)
{
    switch (e->id) {

    /* Central 已经和 ESP32 建立 BLE 物理连接 */
    case BLE_UART_EVT_CONNECTED: {
        const uint8_t *b = e->connected.peer.bytes;
        ESP_LOGI(TAG,
                 "evt: connected peer=%02x:%02x:%02x:%02x:%02x:%02x type=%u",
                 b[0], b[1], b[2], b[3], b[4], b[5], e->connected.peer.type);
        break;
    }

    /* BLE 连接已经断开 */
    case BLE_UART_EVT_DISCONNECTED:
        ESP_LOGI(TAG, "evt: disconnected reason=0x%x",
                 e->disconnected.reason);
        break;

    /*
     * Central 对 TX Characteristic 的 CCCD 进行了订阅 / 取消订阅。
     *
     * BLE UART 的 TX 方向使用 Notify：
     * ESP32 -> Central。
     * 只有 Central 订阅后，ESP32 才能正常通过 TX Characteristic 推送数据。
     */
    case BLE_UART_EVT_SUBSCRIBED:
        ESP_LOGI(TAG, "evt: %ssubscribed",
                 e->subscribed.subscribed ? "" : "un");
        break;

    /*
     * BLE 安全链路已经建立。
     * 这里可以查看：
     * - encrypted：是否加密
     * - authenticated：是否完成身份认证（例如 MITM 保护）
     * - bonded：是否建立 Bond，密钥是否可在下次重连时复用
     * - key_size：加密密钥长度
     */
    case BLE_UART_EVT_LINK_SECURE:
        ESP_LOGI(TAG, "evt: link_secure enc=%d auth=%d bond=%d ks=%u",
                 e->link_secure.encrypted, e->link_secure.authenticated,
                 e->link_secure.bonded, e->link_secure.key_size);
        break;

    /*
     * 配对过程中需要 ESP32 向用户显示 6 位 Passkey。
     * 本例只把 Passkey 打到串口日志中。
     */
    case BLE_UART_EVT_PASSKEY_DISPLAY:
        ESP_LOGI(TAG, "evt: passkey=%06" PRIu32, e->passkey.passkey);
        break;

    /*
     * 配对过程中要求 ESP32 输入对方显示的 6 位 Passkey。
     *
     * 只有 security.io_cap 配置为 KEYBOARD_ONLY 或 KEYBOARD_DISPLAY 时
     * 才通常会触发这个事件。
     *
     * 当前示例使用 AUTO，默认行为相当于 DisplayOnly，因此正常情况下不会进入这里。
     * 如果未来产品真的有按键 / 键盘，可以获取用户输入后调用：
     *
     *     ble_uart_passkey_reply(digits);
     */
    case BLE_UART_EVT_PASSKEY_REQUEST:
        ESP_LOGW(TAG, "evt: passkey entry requested — no UI wired in this "
                      "example (see PORTING.md §5.6.1)");
        break;

    /*
     * Numeric Comparison（数字比较）配对事件。
     *
     * 只有 DISPLAY_YES_NO 或 KEYBOARD_DISPLAY 这类 IO 能力才通常会触发。
     * 两端会显示同一个 6 位数字，由用户确认是否一致。
     * 如果产品有“是 / 否”按键，最终可调用：
     *
     *     ble_uart_compare_reply(user_says_match);
     */
    case BLE_UART_EVT_NUMERIC_COMPARE:
        ESP_LOGW(TAG, "evt: numeric compare %06" PRIu32
                      " — no yes/no UI wired (see PORTING.md §5.6.1)",
                 e->numeric_compare.passkey);
        break;

    /* 配对失败，例如用户拒绝、超时或安全协商失败 */
    case BLE_UART_EVT_PAIRING_FAILED:
        ESP_LOGW(TAG, "evt: pairing failed reason=0x%x",
                 e->pairing_failed.reason);
        break;

    /*
     * 异步关闭 ble_uart_close_async() 完成事件。
     *
     * 当前示例没有主动调用 close_async()，保留这个分支主要是为了完整处理所有枚举值。
     * 即使收到 CLOSED，也不应该在这个 BLE Host 回调里直接 uninstall；
     * 正确方式是通知普通应用任务，再由应用任务调用 ble_uart_uninstall()。
     */
    case BLE_UART_EVT_CLOSED:
        if (e->closed.status == BLE_UART_OK) {
            ESP_LOGI(TAG, "evt: closed (async-close succeeded)");
        } else {
            ESP_LOGW(TAG, "evt: closed async-close failed status=%d",
                     e->closed.status);
        }
        break;
    }
}

void app_main(void)
{
    /*
     * ESP-IDF 应用入口。
     *
     * 本例整体流程可以先记成：
     * 1. 初始化 NVS；
     * 2. 根据 BT MAC 生成设备名称；
     * 3. ble_uart_install()：安装 / 初始化 BLE UART 服务；
     * 4. 查询并打印 Flash 中已经 Bond 的设备；
     * 5. ble_uart_open()：启动 BLE，并开始广播等待 Central 连接。
     */

    /*
     * 第 1 步：初始化 NVS。
     *
     * NVS 不只是应用自己的键值存储，这里的 BLE 也需要它：
     * - 蓝牙 Controller PHY 校准信息可能会使用 NVS；
     * - Bonding 后的长期密钥（LTK 等）需要持久化保存。
     *
     * 所以必须在 ble_uart_install() 之前完成 NVS 初始化。
     */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS 空间不足或版本不兼容时，擦除后重新初始化 */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /*
     * 第 2 步：读取 ESP32 的 Bluetooth MAC 地址，并生成一个尽量唯一的设备名称。
     *
     * 最终名称格式：
     *
     *     <配置的前缀>-XXXX
     *
     * 例如：
     *
     *     BleUart-A1B2
     *
     * XXXX 使用 BT MAC 地址最后两个字节。
     * 这样同时开多块 ESP32 时，手机扫描列表里更容易区分。
     */
    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_BT);
    if (mac_err != ESP_OK) {
        /*
         * 如果 MAC 读取失败，mac 数组仍然保持全 0，
         * 最终设备名后缀会退化成 "0000"，因此打印警告方便排查。
         */
        ESP_LOGW(TAG, "esp_read_mac(BT) failed (%s); device name suffix will be 0000",
                 esp_err_to_name(mac_err));
    }

    char name[BLE_UART_DEVICE_NAME_MAX + 1];
    snprintf(name, sizeof(name), "%s-%02X%02X",
             CONFIG_BLE_UART_DEVICE_NAME_PREFIX, mac[4], mac[5]);

    /*
     * 第 3 步：安装 BLE UART 组件。
     *
     * ble_uart_install() 可以理解为：
     * “把 BLE UART 运行所需要的底层设施准备好”。
     *
     * 根据 README，底层会完成类似：
     * - 初始化所选 BLE Host（NimBLE 或 Bluedroid）；
     * - 创建 BLE UART GATT Service；
     * - 创建 RX / TX Characteristic；
     * - 配置安全、配对、Bonding；
     * - 保存应用提供的 RX 回调和事件回调。
     *
     * 注意：install() 主要是安装和初始化，真正开始广播在后面的 ble_uart_open()。
     */
    ESP_ERROR_CHECK(ble_uart_install(&(ble_uart_config_t){
        /*
         * 开启加密模式。
         * 该示例默认要求 BLE UART 的 GATT 数据访问经过安全链路。
         */
        .encrypted      = true,

        /* 设置 GAP Device Name，即连接后 Central 可通过 GAP Service 读取的设备名称 */
        .device_name    = name,

#if CONFIG_EXAMPLE_CUSTOM_ADV_DATA
        /*
         * 如果启用了自定义广播，则把前面定义的原始 AD Structure 字节数组交给组件。
         *
         * 注意：广播中的 Local Name 和 GAP Service 里的 Device Name 是两条独立路径。
         * 即使广播里写的是其他名字，连接后读取 GAP Device Name 仍然是上面的 name。
         */
        .adv_data       = example_adv_payload,
        .adv_data_len   = sizeof(example_adv_payload),

        /*
         * scan_rsp_data 没有显式赋值，因此保持结构体默认值 NULL。
         * ble_uart 仍然会生成自己的默认 Scan Response。
         * 如果以后希望完全控制 Scan Response，也可以通过同类字段传入原始字节。
         */
#endif

        /*
         * 注册 RX 回调：Central 写 RX Characteristic 时调用 ble_uart_on_rx()。
         */
        .ble_uart_on_rx = ble_uart_on_rx,

        /*
         * 注册事件回调：连接、断开、订阅、配对、安全链路等事件统一进入 ble_uart_on_event()。
         */
        .on_event       = ble_uart_on_event,
    }));

    /*
     * 第 4 步：演示 Bond 管理 API。
     *
     * BLE Bonding 的意义是：
     * 第一次配对后把长期密钥等安全信息保存到 NVS，
     * 下次同一个设备重新连接时可以复用安全关系，不必每次重新完整配对。
     *
     * 这里最多准备 8 个地址槽位，把已经 Bond 的 Central 地址读出来并打印。
     */
    size_t total = 0;
    ble_uart_addr_t list[8];
    int rc = ble_uart_get_bonded_peers(list, sizeof(list) / sizeof(list[0]),
                                       &total);

    if (rc == 0) {
        ESP_LOGI(TAG, "%u peer(s) currently bonded", (unsigned)total);

        /*
         * total 可能大于本地 list[8] 的容量，
         * 因此 shown 只表示本次真正能够打印出来的数量。
         */
        size_t shown = total < sizeof(list) / sizeof(list[0])
                       ? total : sizeof(list) / sizeof(list[0]);

        for (size_t i = 0; i < shown; i++) {
            const uint8_t *b = list[i].bytes;
            ESP_LOGI(TAG, "  [%u] %02x:%02x:%02x:%02x:%02x:%02x type=%u",
                     (unsigned)i,
                     b[0], b[1], b[2], b[3], b[4], b[5], list[i].type);
        }

        if (total > shown) {
            ESP_LOGI(TAG, "  (%u more not shown)",
                     (unsigned)(total - shown));
        }
    }

    /*
     * 第 5 步：打开 BLE UART 服务。
     *
     * 按 README 的抽象：
     * ble_uart_install() = 初始化 Host + 创建 GATT 服务；
     * ble_uart_open()    = 启动运行并开始广播，等待 Central 连接。
     *
     * 从这一行之后，程序主要进入“事件驱动”模式：
     * - 连接 / 断开 -> ble_uart_on_event()
     * - Central 写 RX -> ble_uart_on_rx()
     * - ESP32 发数据 -> ble_uart_tx()
     */
    ESP_ERROR_CHECK(ble_uart_open());
}
