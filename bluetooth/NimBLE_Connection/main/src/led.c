/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * 本文件负责开发板 LED 的初始化、点亮和熄灭。
 *
 * 本示例用 LED 直接表示 BLE 连接状态：
 * - 手机连接 ESP32：LED 点亮
 * - 手机断开 ESP32：LED 熄灭
 *
 * ESP-IDF 支持两种常见 LED：
 * 1. 可寻址 RGB LED（LED Strip，例如 WS2812）
 * 2. 普通 GPIO LED
 *
 * 编译时会根据 menuconfig 中的配置选择其中一种实现。
 */

/* 头文件 */
#include "led.h"
#include "common.h"

/* 保存当前 LED 状态：true 表示亮，false 表示灭 */
static uint8_t led_state;

#ifdef CONFIG_BLINK_LED_STRIP
/*
 * 如果使用可寻址 RGB LED，需要保存 LED Strip 设备句柄。
 * 后续点亮、刷新、清空 LED 都通过这个句柄完成。
 */
static led_strip_handle_t led_strip;
#endif

/* ==================== 对外提供的函数 ==================== */

/* 返回当前保存的 LED 状态 */
uint8_t get_led_state(void) { return led_state; }

/*
 * 如果 menuconfig 选择的是可寻址 RGB LED，编译下面这一组实现。
 */
#ifdef CONFIG_BLINK_LED_STRIP

void led_on(void) {
    /*
     * 设置第 0 号 LED 像素的 RGB 值。
     * 每个颜色通道范围是 0~255。
     * 这里 R/G/B 都设置为 16，因此是比较暗的白色。
     */
    led_strip_set_pixel(led_strip, 0, 16, 16, 16);

    /*
     * 刚才只是修改了内存中的像素值，
     * 调用 refresh 后才会真正把数据发送给 LED。
     */
    led_strip_refresh(led_strip);

    /* 更新软件中保存的 LED 状态 */
    led_state = true;
}

void led_off(void) {
    /* 清空 LED Strip 中所有像素，使 LED 熄灭 */
    led_strip_clear(led_strip);

    /* 更新软件中保存的 LED 状态 */
    led_state = false;
}

void led_init(void) {
    ESP_LOGI(TAG, "example configured to blink addressable led!");

    /*
     * 配置 LED Strip：
     * strip_gpio_num：LED 使用的 GPIO，由 CONFIG_BLINK_GPIO 决定
     * max_leds      ：LED 数量，这个示例只使用 1 颗
     */
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_BLINK_GPIO,
        .max_leds = 1, // 开发板上至少使用 1 颗 LED
    };

#if CONFIG_BLINK_LED_STRIP_BACKEND_RMT
    /*
     * 如果 LED Strip 后端选择 RMT：
     * RMT 是 ESP32 的一个硬件外设，适合生成精确时序波形，
     * 经常用于驱动 WS2812 这类可寻址 RGB LED。
     */
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // RMT 分辨率 10 MHz
        .flags.with_dma = false,            // 本示例不使用 DMA
    };

    /* 创建一个基于 RMT 的 LED Strip 设备 */
    ESP_ERROR_CHECK(
        led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

#elif CONFIG_BLINK_LED_STRIP_BACKEND_SPI
    /*
     * 如果 LED Strip 后端选择 SPI，则通过 SPI 总线驱动 LED。
     */
    led_strip_spi_config_t spi_config = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };

    /* 创建一个基于 SPI 的 LED Strip 设备 */
    ESP_ERROR_CHECK(
        led_strip_new_spi_device(&strip_config, &spi_config, &led_strip));

#else
#error "unsupported LED strip backend"
#endif

    /* 初始化完成后，先确保 LED 处于熄灭状态 */
    led_off();
}

/*
 * 如果 menuconfig 选择的是普通 GPIO LED，则编译下面这一组实现。
 */
#elif CONFIG_BLINK_LED_GPIO

void led_on(void) {
    /* GPIO 输出高电平，使 LED 点亮 */
    gpio_set_level(CONFIG_BLINK_GPIO, true);
}

void led_off(void) {
    /* GPIO 输出低电平，使 LED 熄灭 */
    gpio_set_level(CONFIG_BLINK_GPIO, false);
}

void led_init(void) {
    ESP_LOGI(TAG, "example configured to blink gpio led!");

    /* 重置该 GPIO，清除之前可能存在的配置 */
    gpio_reset_pin(CONFIG_BLINK_GPIO);

    /* 将 GPIO 设置成推挽输出模式，用于驱动 LED */
    gpio_set_direction(CONFIG_BLINK_GPIO, GPIO_MODE_OUTPUT);
}

#else
/* 如果既没有选择 LED Strip，也没有选择普通 GPIO LED，就在编译阶段报错 */
#error "unsupported LED type"
#endif
