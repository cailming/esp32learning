/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * 本文件负责开发板 LED 的初始化、点亮和熄灭。
 *
 * 本例支持两种 LED：
 * 1. 可寻址 RGB LED（LED Strip，例如 WS2812）；
 * 2. 普通 GPIO LED。
 *
 * 具体使用哪一种由 menuconfig / sdkconfig 中的配置决定。
 * 在 GATT Server 示例中，手机可以通过 LED Characteristic 写入 0/非0，
 * 最终调用这里的 led_off() / led_on() 控制实际 LED。
 */

/* 头文件 */
#include "led.h"
#include "common.h"

/* 保存当前 LED 状态，0/false 表示灭，非0/true 表示亮 */
static uint8_t led_state;

#ifdef CONFIG_BLINK_LED_STRIP
/* 如果使用可寻址 RGB LED，这里保存 LED Strip 驱动句柄 */
static led_strip_handle_t led_strip;
#endif

/* 返回当前记录的 LED 状态 */
uint8_t get_led_state(void) { return led_state; }

#ifdef CONFIG_BLINK_LED_STRIP

/* 点亮可寻址 RGB LED */
void led_on(void) {
    /*
     * 设置第 0 颗 LED 的 RGB 值。
     * 每个颜色通道范围是 0~255，这里使用 16,16,16，
     * 相当于较暗的白色，避免亮度过高。
     */
    led_strip_set_pixel(led_strip, 0, 16, 16, 16);

    /*
     * set_pixel() 只是修改驱动内部缓存，
     * refresh() 才会把数据真正发送给 LED。
     */
    led_strip_refresh(led_strip);

    /* 更新软件记录的 LED 状态 */
    led_state = true;
}

/* 熄灭可寻址 RGB LED */
void led_off(void) {
    /* 清空所有像素数据，使 LED 熄灭 */
    led_strip_clear(led_strip);

    /* 更新软件记录的 LED 状态 */
    led_state = false;
}

/* 初始化可寻址 RGB LED */
void led_init(void) {
    ESP_LOGI(TAG, "example configured to blink addressable led!");

    /*
     * LED Strip 基本配置：
     * strip_gpio_num -> LED 数据线连接的 GPIO；
     * max_leds       -> LED 数量，本例只使用 1 颗。
     */
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_BLINK_GPIO,
        .max_leds = 1, // 开发板上至少有一颗 LED
    };

#if CONFIG_BLINK_LED_STRIP_BACKEND_RMT
    /*
     * 使用 ESP32 RMT 外设驱动 LED Strip。
     * RMT 很适合产生 WS2812 这类 LED 所要求的精确时序信号。
     */
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz 时间分辨率
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(
        led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

#elif CONFIG_BLINK_LED_STRIP_BACKEND_SPI
    /* 如果配置使用 SPI 后端，则通过 SPI2_HOST 驱动 LED Strip */
    led_strip_spi_config_t spi_config = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    ESP_ERROR_CHECK(
        led_strip_new_spi_device(&strip_config, &spi_config, &led_strip));
#else
#error "unsupported LED strip backend"
#endif

    /* 初始化完成后先确保 LED 处于熄灭状态 */
    led_off();
}

#elif CONFIG_BLINK_LED_GPIO

/*
 * 如果开发板使用普通 GPIO LED，控制方式更加直接：
 * GPIO 输出高电平 -> 点亮；
 * GPIO 输出低电平 -> 熄灭。
 */
void led_on(void) { gpio_set_level(CONFIG_BLINK_GPIO, true); }

void led_off(void) { gpio_set_level(CONFIG_BLINK_GPIO, false); }

/* 初始化普通 GPIO LED */
void led_init(void) {
    ESP_LOGI(TAG, "example configured to blink gpio led!");

    /* 将 GPIO 恢复到默认状态 */
    gpio_reset_pin(CONFIG_BLINK_GPIO);

    /* 将该 GPIO 配置为普通推挽输出模式 */
    gpio_set_direction(CONFIG_BLINK_GPIO, GPIO_MODE_OUTPUT);
}

#else
#error "unsupported LED type"
#endif
