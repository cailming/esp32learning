/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Includes */
#include "gap.h"
#include "common.h"

/* Private function declarations */
inline static void format_addr(char *addr_str, uint8_t addr[]);     //format_addr：把 6 字节 MAC 地址格式化成 "XX:XX:XX:XX:XX:XX" 字符串，用于日志打印
static void start_advertising(void);        //配置并启动广播

/* Private variables 
    own_addr_type：
        本设备广播时使用的 BLE 地址类型。BLE 地址不是简单的 MAC，有多种类型：
            BLE_ADDR_TYPE_PUBLIC：公共地址，出厂烧录，全球唯一。
            BLE_ADDR_TYPE_RANDOM：随机地址，又细分为静态随机、可解析私有、不可解析私有。
        这个变量在 adv_init() 中通过 ble_hs_id_infer_auto() 推断出来，在 start_advertising() 中传给广播参数和扫描响应。
        为什么需要推断？ ESP32 可能配置为使用 public 地址或 random 地址，取决于 menuconfig 和芯片 efuse。
        ble_hs_id_infer_auto() 根据当前配置自动选择可用的地址类型，应用不需要硬编码。
    addr_val[6]：
        本设备的 6 字节 BLE 地址。在 adv_init() 中从协议栈拷贝出来，用于：
            格式化成字符串打印日志。
            填入扫描响应数据的 device_addr 字段。
        为什么单独保存？ 
            因为 ble_hs_id_copy_addr() 需要输出缓冲区，保存下来后 start_advertising() 可以直接用，不用再次查询。
    esp_uri[]：
        一个硬编码的 URI 字节数组，内容是 https://espressif.com。
        BLE_GAP_URI_PREFIX_HTTPS 是 NimBLE 定义的常量，
            表示 URI 协议前缀 https:// 的编码值（BLE 规范中 URI 前缀用 1 字节表示，节省广播空间）。
        后面的 '/'、'e'、's'……是 URI 的剩余部分。
        这是 Eddystone-URL 风格的 beacon 数据：广播一个网址，扫描设备（如手机）可以解析并显示。
        这是 BLE beacon 的常见用法（如 Google Eddystone、Apple iBeacon 的变体）。
        为什么用数组而不是字符串？ 
            因为 BLE 广播数据是二进制字节流，URI 字段的编码是“前缀字节 + 剩余字符串字节”，不是标准 C 字符串。
            用数组明确表示二进制数据。
*/
static uint8_t own_addr_type;
static uint8_t addr_val[6] = {0};
static uint8_t esp_uri[] = {BLE_GAP_URI_PREFIX_HTTPS, '/', '/', 'e', 's', 'p', 'r', 'e', 's', 's', 'i', 'f', '.', 'c', 'o', 'm'};

/* Private functions */
inline static void format_addr(char *addr_str, uint8_t addr[]) {
    sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1],
            addr[2], addr[3], addr[4], addr[5]);
}

//广播配置的核心
static void start_advertising(void) {
    /* Local variables */
    int rc = 0;
    const char *name;
    /*
    adv_fields：广播数据字段。设备主动广播时发送的数据，最多 31 字节。
    rsp_fields：扫描响应数据字段。当扫描设备发送 SCAN_REQ 请求更多信息时，设备返回的数据，最多 31 字节。
    adv_params：广播参数。控制广播类型（可连接/不可连接）、发现模式、间隔等。
    广播包放：flags、设备名、发射功率、外观、LE 角色。
    扫描响应放：设备地址、URI。
    这样广播包内容精简，扫描响应补充详细信息。
    */
    struct ble_hs_adv_fields adv_fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    struct ble_gap_adv_params adv_params = {0};

    /* Set advertising flags 
    Flags 字段是广播包的第一个字段，告诉扫描设备本设备的能力：
        BLE_HS_ADV_F_DISC_GEN：通用可发现模式（General Discoverable）。表示设备希望被任何扫描设备发现，没有时间限制。
            另一个选项是 BLE_HS_ADV_F_DISC_LTD（有限可发现），只在一段时间内可被发现。
        BLE_HS_ADV_F_BREDR_UNSUP：不支持 BR/EDR（经典蓝牙）。
            表示这是纯 BLE 设备，不是双模设备。这帮助扫描设备决定用 BLE 还是经典蓝牙连接。
    */
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* Set device name 设备名字段：
    ble_svc_gap_device_name()：从 GAP 服务中读取设备名。
        这个名称在 gap_init() 中通过 ble_svc_gap_device_name_set(DEVICE_NAME) 设置。
    name_is_complete = 1：表示这是完整的设备名，不是缩写。
        扫描设备可以信任这个名称是完整的。如果设备名太长放不下，可以设 name_is_complete = 0 表示缩写。
    为什么从 GAP 服务读而不是直接用 DEVICE_NAME 宏？ 
        因为设备名可能被其他代码动态修改（ble_svc_gap_device_name_set 可以多次调用）。
        从 GAP 服务读保证广播的名字和 GAP 服务中的名字一致。
    */
    name = ble_svc_gap_device_name();
    printf("设备名称：%s \n",name);
    adv_fields.name = (uint8_t *)name;
    adv_fields.name_len = strlen(name);
    adv_fields.name_is_complete = 1;

    /* Set device tx power 发射功率字段：
        BLE_HS_ADV_TX_PWR_LVL_AUTO：让协议栈自动填入当前发射功率（dBm）。
        tx_pwr_lvl_is_present = 1：表示广播包中包含这个字段。
        作用：扫描设备可以用“接收信号强度（RSSI）”减去“广播的发射功率”来估算距离。
            这是 iBeacon/Eddystone 测距的基础。
    */
    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    adv_fields.tx_pwr_lvl_is_present = 1;

    /* Set device appearance  外观字段：
        BLE_GAP_APPEARANCE_GENERIC_TAG：表示这是一个“通用标签”设备。
            BLE 规范定义了多种外观值（如手机、键盘、心率传感器等），扫描设备可以据此显示图标。
        为什么选 TAG？ 因为这是一个 beacon 示例，不是特定类型的设备。
            GENERIC_TAG 是通用标签，适合 beacon。
    */
    adv_fields.appearance = BLE_GAP_APPEARANCE_GENERIC_TAG;
    adv_fields.appearance_is_present = 1;

    /* Set device LE role  LE 角色字段：
        BLE_GAP_LE_ROLE_PERIPHERAL：表示本设备是外设（Peripheral），即从设备。
            它会广播，等待中心设备（手机）连接。
        另一个角色是 BLE_GAP_LE_ROLE_CENTRAL（中心设备），主动扫描和发起连接。
            注意：虽然本示例的广播是“不可连接”的（后面会讲），
                但角色字段仍标为 Peripheral，表示设备的设计角色是从设备。
    */
    adv_fields.le_role = BLE_GAP_LE_ROLE_PERIPHERAL;
    adv_fields.le_role_is_present = 1;

    /* Set advertisement fields 
    ble_gap_adv_set_fields：把 adv_fields 中的字段序列化成 BLE 广播包的二进制格式，存入协议栈。
            失败可能因为字段总长度超过 31 字节，或参数非法。
    为什么失败直接 return？ 广播数据设置失败，后续启动广播没有意义。
        打印错误码便于调试（常见错误：字段太多导致超长）。
    */
    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set advertising data, error code: %d", rc);
        return;
    }

    /* Set device address 
        扫描响应字段. 设备地址字段：
device_addr：指向 addr_val（6 字节地址）。
device_addr_type：地址类型（public/random），来自 own_addr_type。
device_addr_is_present = 1：表示扫描响应中包含地址。
为什么把地址放扫描响应而不是广播包？ 因为广播包空间有限，地址占 6 字节 + 类型 1 字节 = 7 字节。
放扫描响应可以节省广播包空间给设备名等更重要的字段。主动扫描的设备仍能拿到地址。
    */
    rsp_fields.device_addr = addr_val;
    rsp_fields.device_addr_type = own_addr_type;
    rsp_fields.device_addr_is_present = 1;

    /* Set URI */
    rsp_fields.uri = esp_uri;
    rsp_fields.uri_len = sizeof(esp_uri);

    /* Set scan response fields */
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set scan response data, error code: %d", rc);
        return;
    }

    /* Set non-connectable and general discoverable mode to be a beacon */
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /* Start advertising 
        启动广播
    */
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                           NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to start advertising, error code: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising started!");
}

/* Public functions 广播初始化入口 */
void adv_init(void) {
    /* Local variables */
    int rc = 0;
    char addr_str[18] = {0};

    /* Make sure we have proper BT identity address set
        确保有可用地址
        ble_hs_util_ensure_addr(0)：确保协议栈有一个可用的 BLE 地址。参数 0 表示“不要求可解析地址”。
        为什么需要这一步？ BLE 地址可能来自：
            芯片 efuse 中烧录的 public 地址。
            协议栈生成的 random 地址。
        如果两者都没有（如 efuse 未烧录且 random 地址未配置），需要生成一个。
            ensure_addr 会检查并确保至少有一种地址可用。
            失败说明芯片配置异常（如 efuse 损坏且无法生成 random 地址）。
    */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "device does not have any available bt address!");
        return;
    }

    /* Figure out BT address to use while advertising 
        推断地址类型:ble_hs_id_infer_auto(0, &own_addr_type)：
            自动推断本设备应该使用哪种地址类型。参数 0 表示“不要求可解析地址”。
            输出到 own_addr_type，可能是 BLE_ADDR_TYPE_PUBLIC 或 BLE_ADDR_TYPE_RANDOM。
        推断逻辑：
            如果 efuse 中有有效的 public 地址，优先用 public。
            否则用 random 地址。
            具体行为取决于 menuconfig 中的 CONFIG_BT_NIMBLE_NVS_PERSIST 等配置。
    */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer address type, error code: %d", rc);
        return;
    }

    /* Copy device address to addr_val 
        拷贝地址值. ble_hs_id_copy_addr：
            把当前使用的 BLE 地址拷贝到 addr_val（6 字节）。
            第三个参数传 NULL，表示不需要输出地址类型（已经知道是 own_addr_type）。
            失败可能因为地址类型无效或协议栈状态异常。
    */
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to copy device address, error code: %d", rc);
        return;
    }
    format_addr(addr_str, addr_val);
    ESP_LOGI(TAG, "device address: %s", addr_str);

    /* Start advertising. 
    调用 start_advertising() 配置并启动广播。
        adv_init 的整体逻辑：确保地址可用 → 推断类型 → 拷贝地址 → 打印 → 启动广播。这是 BLE 广播的标准准备流程
    */
    start_advertising();
}

// GAP 服务初始化  gap_init() 在 app_main 中调用，负责初始化 GAP 服务并设置设备名和外观。
int gap_init(void) {
    /* Local variables */
    int rc = 0;

    /* Initialize GAP service 
    ble_svc_gap_init()：
        初始化 NimBLE 的 GAP 服务。GAP 服务是 BLE 规范强制要求的服务，包含：
        Device Name 特征：设备名，可读可写。
        Appearance 特征：设备外观，只读。
        Peripheral Preferred Connection Parameters 特征：连接参数偏好。
        这个函数注册 GAP 服务的 GATT 特征，让其他设备可以读取设备名和外观。
        必须在设置设备名之前调用，因为 ble_svc_gap_device_name_set 需要 GAP 服务已初始化
    */
    ble_svc_gap_init();

    /* Set GAP device name 设置设备名
        DEVICE_NAME 是 common.h 中定义的宏（如 "NimBLE_Beacon"）。
        设置后，GAP 服务的 Device Name 特征值变为这个名称，同时 ble_svc_gap_device_name() 会返回它。
        start_advertising() 中的 ble_svc_gap_device_name() 读取的就是这里设置的值。所以设备名在广播包和 GAP 服务中保持一致。
        失败可能因为名称太长（超过 248 字节）或 GAP 服务未初始化。
    */
    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device name to %s, error code: %d",
                 DEVICE_NAME, rc);
        return rc;
    }

    /* Set GAP device appearance 设置设备外观
    设置 GAP 服务的 Appearance 特征值为“通用标签”。
        和 start_advertising() 中的 adv_fields.appearance 对应：
            这里设置 GAP 服务中的外观，start_advertising() 把同样的值放进广播数据。
            两处一致，扫描设备从广播包和 GAP 服务读到的外观相同。
        为什么两处都要设？
            广播包中的外观让扫描设备在未连接时就能看到设备类型；GAP 服务中的外观让连接后可以读取。两者互补。
    */
    rc = ble_svc_gap_device_appearance_set(BLE_GAP_APPEARANCE_GENERIC_TAG);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device appearance, error code: %d", rc);
        return rc;
    }
    return rc;
}
