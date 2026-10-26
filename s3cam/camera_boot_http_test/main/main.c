/*
 * ESP32-S3-CAM：BOOT 按键拍照 + 浏览器查看最新照片 + MJPEG 实时视频预览
 *
 * 这个程序的目标不是做一个“完整相机产品”，而是把几个基础能力串起来：
 *
 *   1. ESP32-S3-CAM 初始化摄像头；
 *   2. ESP32 自己创建一个 Wi-Fi 热点；
 *   3. ESP32 自己启动 HTTP Server；
 *   4. 浏览器通过 /video 查看实时 MJPEG 视频；
 *   5. 每按一次板载 BOOT 按钮（GPIO0），额外拍一张静态 JPEG；
 *   6. 浏览器通过 /photo.jpg 查看最近一次 BOOT 拍下来的照片。
 *
 * 整体数据流可以理解为：
 *
 *                     ┌──────────────┐
 *                     │   OV3660     │
 *                     │   Camera     │
 *                     └──────┬───────┘
 *                            │ JPEG
 *               ┌────────────┴────────────┐
 *               │                         │
 *         BOOT 按键拍照              MJPEG 实时视频
 *               │                         │
 *        保存最新 JPEG                /video
 *               │                         │
 *          /photo.jpg                     │
 *               └────────────┬────────────┘
 *                            │ HTTP + Wi-Fi
 *                            ▼
 *                         浏览器
 *
 * 使用方式：
 * 1. ESP32-S3-CAM 上电后会创建 Wi-Fi 热点：S3CAM-Photo
 * 2. 电脑连接该热点，密码：12345678
 * 3. 浏览器打开：http://192.168.4.1
 * 4. 页面中会直接显示实时视频预览
 * 5. 每按一次板载 BOOT 按钮（GPIO0），摄像头额外拍一张 JPEG，页面下方显示最新照片
 * 6. 实时视频流也可以直接访问：http://192.168.4.1:81/video
 *
 * 为什么视频流使用 81 端口：
 * - MJPEG 是一个长期保持连接的 HTTP 请求；
 * - ESP-IDF 的 HTTP handler 是同步执行的，若把无限循环的视频 handler 放在 80 端口，
 *   会影响同一个 server 上的 /、/status、/photo.jpg 等普通请求；
 * - 因此 80 端口负责网页和照片 API，81 端口专门负责 /video 实时视频流。
 *
 * 注意：
 * - 麦克风、MAX98357A、喇叭可以继续保持原来的物理接线；
 * - 本程序不会初始化 MIC / Speaker 的 I2S GPIO；
 * - BOOT 按钮在 ESP32-S3 上同时也是下载启动相关按键，正常运行时短按即可；
 * - 不要在按住 BOOT 的同时复位/重新上电，否则可能进入下载模式。
 */

/*
 * ==================== 标准 C 头文件 ====================
 *
 * inttypes.h  : 使用 PRIu32 之类的格式宏，安全打印 uint32_t。
 * stdbool.h   : 使用 bool / true / false。
 * stdint.h    : 使用 uint8_t、uint32_t 等定长整数类型。
 * stdio.h     : 使用 snprintf()。
 * stdlib.h    : 使用 malloc() / realloc() / free()。
 * string.h    : 使用 memcpy() / strlen() / strlcpy()。
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ==================== ESP-IDF / FreeRTOS 头文件 ====================
 */
#include "driver/gpio.h"          /* GPIO 配置与读取 BOOT 按键 */
#include "esp_camera.h"           /* esp32-camera：摄像头初始化、取帧、归还帧 */
#include "esp_err.h"              /* esp_err_t、ESP_OK 等错误码 */
#include "esp_event.h"            /* Wi-Fi 依赖的默认事件循环 */
#include "esp_http_server.h"      /* ESP-IDF 自带 HTTP Server */
#include "esp_log.h"              /* ESP_LOGI / ESP_LOGE / ESP_LOGW */
#include "esp_netif.h"            /* 网络接口初始化 */
#include "esp_wifi.h"             /* Wi-Fi SoftAP */
#include "freertos/FreeRTOS.h"    /* FreeRTOS 基础定义 */
#include "freertos/semphr.h"      /* Mutex / Semaphore */
#include "freertos/task.h"        /* xTaskCreate / vTaskDelay */
#include "nvs_flash.h"            /* Wi-Fi 初始化依赖 NVS */
#include "sdkconfig.h"            /* CONFIG_IDF_TARGET_ESP32S3 等编译配置 */

/*
 * 这个测试工程只针对 ESP32-S3。
 * 如果误把 target 设置成别的芯片，编译阶段直接报错，而不是等运行时才发现问题。
 */
#if !CONFIG_IDF_TARGET_ESP32S3
#error "camera_boot_http_test 只用于 ESP32-S3，请先执行 idf.py set-target esp32s3"
#endif

/*
 * ESP-IDF 日志 TAG。
 * 串口中会看到类似：
 *
 *   I (...) boot_photo: Camera init OK ...
 */
static const char *TAG = "boot_photo";

/* ========================================================================
 * 1. Camera GPIO
 * ========================================================================
 *
 * 下面这组 GPIO 已经在前面的 camera_test 中实际验证通过。
 * 因此这里不再重新猜板型，而是直接复用已确认的 ESP32-S3-CAM 接线。
 *
 * PWDN / RESET 为 -1：说明当前开发板没有单独把这两个 Camera 控制脚接给 ESP32。
 */
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_D0     11
#define CAM_PIN_D1      9
#define CAM_PIN_D2      8
#define CAM_PIN_D3     10
#define CAM_PIN_D4     12
#define CAM_PIN_D5     18
#define CAM_PIN_D6     17
#define CAM_PIN_D7     16
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK   13

/*
 * ESP32-S3 板载 BOOT 按钮连接 GPIO0。
 *
 * 本程序把 BOOT 当普通按键使用：
 *   未按下：GPIO0 = 1（内部上拉）
 *   按下  ：GPIO0 = 0
 *
 * 注意：GPIO0 同时也是 ESP32-S3 的启动模式相关引脚，
 * 所以“程序已经正常启动以后短按”没有问题，
 * 但不要“按住 BOOT 再复位/重新上电”。
 */
#define BOOT_BUTTON_GPIO GPIO_NUM_0

/* ========================================================================
 * 2. Wi-Fi SoftAP 配置
 * ========================================================================
 *
 * 这里不是让 ESP32 连接家里的路由器，而是让 ESP32 自己变成一个 Wi-Fi 热点。
 *
 * Mac / PC 连接：
 *   SSID     : S3CAM-Photo
 *   Password : 12345678
 *
 * ESP32 SoftAP 默认地址通常是 192.168.4.1。
 */
#define AP_SSID     "S3CAM-Photo"
#define AP_PASSWORD "12345678"
#define AP_CHANNEL  1

/* ========================================================================
 * 3. HTTP / MJPEG 协议常量
 * ========================================================================
 */

/*
 * 普通网页/API 使用 80 端口。
 * MJPEG 视频流单独使用 81 端口。
 */
#define STREAM_PORT 81

/*
 * MJPEG 并不是 H.264 / H.265 那种真正的视频编码。
 * 它本质上就是：连续不断地通过 HTTP 发送一张又一张 JPEG。
 *
 * 浏览器需要知道“一张 JPEG 从哪里开始、在哪里结束”，
 * 因此 HTTP multipart 协议会使用一个 boundary 字符串来分隔每一帧。
 *
 * 这个字符串本身没有特殊含义，只要足够独特、前后保持一致即可。
 */
#define PART_BOUNDARY "123456789000000000000987654321"

/*
 * 整个 /video HTTP Response 的 Content-Type。
 *
 * multipart/x-mixed-replace 表示：
 *   这个 HTTP 响应不是只有一个固定内容，
 *   服务端会持续用新的内容块替换/追加旧内容。
 *
 * 浏览器看到这种格式后，就可以把连续 JPEG 当作 MJPEG 视频显示。
 */
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;

/*
 * 每发送一张 JPEG 之前，先发送 boundary。
 * \r\n 是 HTTP 协议常见的换行格式。
 */
static const char *STREAM_BOUNDARY =
    "\r\n--" PART_BOUNDARY "\r\n";

/*
 * 每一个 multipart 分段自己的 HTTP 头。
 *
 * Content-Type: image/jpeg     -> 告诉浏览器这一段是一张 JPEG
 * Content-Length: %zu         -> 告诉浏览器 JPEG 有多少字节
 */
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n";

/* ========================================================================
 * 4. “最近一次 BOOT 拍照”的内存缓存
 * ========================================================================
 */

/*
 * s_latest_jpeg：指向最近一次 BOOT 拍下来的 JPEG 数据。
 *
 * 这里保存的是我们自己 malloc() 出来的内存，
 * 不是 Camera 驱动内部 framebuffer 的指针。
 */
static uint8_t *s_latest_jpeg = NULL;

/* 最近一张 JPEG 的实际字节数。 */
static size_t s_latest_jpeg_len = 0;

/* BOOT 成功拍照的累计次数。网页通过这个数字判断“有没有新照片”。 */
static uint32_t s_photo_count = 0;

/*
 * ========================================================================
 * 5. 两把 Mutex：为什么要两把锁？
 * ========================================================================
 *
 * s_photo_mutex：
 *   保护“最近一张静态照片缓存”：
 *     - s_latest_jpeg
 *     - s_latest_jpeg_len
 *     - s_photo_count
 *
 * s_camera_mutex：
 *   保护“Camera 硬件 / framebuffer 获取过程”。
 *
 * 为什么需要两个锁？
 *
 * 现在程序里至少有两个执行路径会访问摄像头：
 *
 *   BOOT 按键任务
 *       |
 *       +-- capture_one_photo()
 *       |       |
 *       |       +-- esp_camera_fb_get()
 *       |
 *   /video HTTP handler
 *               |
 *               +-- esp_camera_fb_get()
 *
 * 如果两边同时调用 esp_camera_fb_get()，特别是在 fb_count=1 的情况下，
 * 很容易出现资源竞争、取帧失败甚至异常。
 *
 * 因此：
 *   - “谁要从 Camera 取帧，谁先拿 s_camera_mutex”；
 *   - “谁要读写最近一张静态照片，谁先拿 s_photo_mutex”。
 *
 * 两类资源分开加锁，可以避免一把大锁把所有事情都串行化。
 */
static SemaphoreHandle_t s_photo_mutex = NULL;
static SemaphoreHandle_t s_camera_mutex = NULL;

/* ========================================================================
 * 6. 浏览器首页 HTML
 * ========================================================================
 *
 * 为了让这个测试工程完全独立，HTML / CSS / JavaScript 都直接写在 C 字符串里。
 * ESP32 不需要 SPIFFS / LittleFS，也不需要额外 html 文件。
 *
 * 页面做两件事：
 *
 * ① 实时视频：
 *      <img src="http://192.168.4.1:81/video">
 *
 *    浏览器会一直保持 /video HTTP 连接，连续接收 JPEG 帧。
 *
 * ② BOOT 拍照：
 *      JavaScript 每 500ms 请求一次 /status。
 *
 *    如果 photo_count 变化，就重新请求 /photo.jpg。
 *
 * URL 后额外带时间戳，是为了避免浏览器把旧图片缓存起来。
 */
static const char s_index_html[] =
    "<!doctype html>"
    "<html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-S3-CAM Camera Test</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,Arial,sans-serif;margin:24px;background:#f5f5f5;color:#222;}"
    ".card{max-width:720px;margin:auto;background:white;padding:20px;border-radius:14px;box-shadow:0 2px 12px #0002;}"
    "img{width:100%;height:auto;border-radius:10px;background:#ddd;margin-top:10px;}"
    ".hint{color:#555;line-height:1.6;}"
    ".status{font-weight:600;margin-top:12px;}"
    ".section{margin-top:22px;padding-top:18px;border-top:1px solid #ddd;}"
    "code{background:#eee;padding:2px 5px;border-radius:4px;}"
    "</style></head><body><div class='card'>"
    "<h2>ESP32-S3-CAM 摄像头测试</h2>"
    "<div class='hint'>实时视频使用 MJPEG。每按一次开发板 BOOT 按钮，还会额外保存一张最新照片。</div>"
    "<div class='section'>"
    "<h3>实时视频预览</h3>"
    "<div class='hint'>视频 URL：<code>http://192.168.4.1:81/video</code></div>"
    "<img id='video' src='http://192.168.4.1:81/video' alt='实时视频流'>"
    "</div>"
    "<div class='section'>"
    "<h3>BOOT 拍照</h3>"
    "<div id='status' class='status'>等待第一次按 BOOT 拍照...</div>"
    "<img id='photo' style='display:none' alt='最新 BOOT 照片'>"
    "</div>"
    "</div>"
    "<script>"
    /* lastCount 保存浏览器上一次看到的照片编号。 */
    "let lastCount=0;"
    /* poll() 每 500ms 查询一次 ESP32 当前拍照状态。 */
    "async function poll(){"
    " try{"
    /* Date.now() 用来破坏浏览器缓存，让每次 /status 都真正请求 ESP32。 */
    "  const r=await fetch('/status?ts='+Date.now(),{cache:'no-store'});"
    "  const s=await r.json();"
    "  document.getElementById('status').textContent = s.count>0"
    "    ? ('已拍 '+s.count+' 张，最新 JPEG '+s.bytes+' bytes')"
    "    : '等待第一次按 BOOT 拍照...';"
    /* 如果 count 变大，说明用户刚刚又按了一次 BOOT。 */
    "  if(s.count>0 && s.count!==lastCount){"
    "    lastCount=s.count;"
    "    const img=document.getElementById('photo');"
    /*
     * n 和 ts 都是为了让 URL 每次不同，防止浏览器继续显示缓存的旧 JPEG。
     */
    "    img.src='/photo.jpg?n='+s.count+'&ts='+Date.now();"
    "    img.style.display='block';"
    "  }"
    " }catch(e){document.getElementById('status').textContent='与开发板通信失败';}"
    "}"
    "setInterval(poll,500);poll();"
    "</script></body></html>";

/* ========================================================================
 * 7. 初始化摄像头
 * ========================================================================
 */
static esp_err_t init_camera(void)
{
    /*
     * camera_config_t 把“摄像头硬件接线”和“图像参数”一次性告诉 esp32-camera 驱动。
     */
    camera_config_t config = {
        /* Camera 电源控制 / Reset；当前板子没有独立使用这两个 GPIO。 */
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,

        /* XCLK：ESP32 输出给摄像头的外部时钟。 */
        .pin_xclk = CAM_PIN_XCLK,

        /* SCCB：摄像头寄存器配置总线，可以理解成 Camera 版本的 I2C。 */
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        /* 8 位并行像素数据总线。 */
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,

        /*
         * VSYNC：一帧图像的同步信号。
         * HREF ：一行有效像素的同步信号。
         * PCLK ：像素时钟。
         */
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        /* 给 Camera 的 XCLK 设置为 20MHz。 */
        .xclk_freq_hz = 20000000,

        /* esp32-camera 内部借助 LEDC 外设产生 XCLK。 */
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        /*
         * 直接让 OV3660 输出 JPEG。
         * 这样 ESP32 不需要把 RGB 再软件压缩成 JPEG，节省大量 CPU / RAM。
         */
        .pixel_format = PIXFORMAT_JPEG,

        /* QVGA = 320x240。先用小分辨率保证 MJPEG 实时预览稳定。 */
        .frame_size = FRAMESIZE_QVGA,

        /*
         * JPEG quality：数字越小，图像质量越高、文件通常越大。
         * 12 是当前测试中比较均衡的值。
         */
        .jpeg_quality = 12,

        /*
         * 只使用一个 Camera framebuffer。
         * 所以 BOOT 拍照和实时视频尤其需要 s_camera_mutex 协调访问。
         */
        .fb_count = 1,

        /*
         * framebuffer 放在内部 DRAM。
         * 当前 QVGA JPEG 较小，已经实测可以正常工作。
         */
        .fb_location = CAMERA_FB_IN_DRAM,

        /* 只有 framebuffer 空闲时才抓下一帧。 */
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    /* 真正初始化 Camera 驱动。 */
    esp_err_t err = esp_camera_init(&config);

    if (err == ESP_OK) {
        /*
         * 获取 Sensor 结构体，只用于打印识别结果。
         * 你前面的测试实际识别到的是 OV3660（PID=0x3660）。
         */
        sensor_t *sensor = esp_camera_sensor_get();
        if (sensor != NULL) {
            ESP_LOGI(TAG, "Camera init OK: PID=0x%04x VER=0x%02x", sensor->id.PID, sensor->id.VER);
        }
    }

    return err;
}

/* ========================================================================
 * 8. BOOT 按一次，拍一张静态 JPEG
 * ========================================================================
 *
 * 这个函数和实时 /video 最大的区别是：
 *
 * /video：
 *   抓一帧 -> 发给浏览器 -> 丢掉 -> 再抓下一帧
 *
 * BOOT 拍照：
 *   抓一帧 -> 复制到 s_latest_jpeg -> 长期保存，直到下一次 BOOT 覆盖
 *
 * 因为 /video 同时也会调用 esp_camera_fb_get()，
 * 所以 BOOT 拍照前必须先取得 s_camera_mutex。
 */
static esp_err_t capture_one_photo(void)
{
    ESP_LOGI(TAG, "BOOT pressed -> capturing photo...");

    /*
     * 等待获得 Camera 的独占使用权。
     * portMAX_DELAY 表示：如果 /video 正在抓帧，就一直等到它释放锁。
     */
    xSemaphoreTake(s_camera_mutex, portMAX_DELAY);

    /*
     * 向 Camera 驱动申请一帧 framebuffer。
     * 返回的 fb 中包含：
     *   fb->buf    JPEG 二进制数据地址
     *   fb->len    JPEG 字节数
     *   fb->width  宽度
     *   fb->height 高度
     *   fb->format 图像格式
     */
    camera_fb_t *fb = esp_camera_fb_get();

    if (fb == NULL) {
        /* 获取失败时一定记得释放 Camera 锁。 */
        xSemaphoreGive(s_camera_mutex);
        ESP_LOGE(TAG, "Capture failed: esp_camera_fb_get() returned NULL");
        return ESP_FAIL;
    }

    /*
     * 对 JPEG 做一个最基础的合法性检查。
     * JPEG 文件开头通常是：
     *   0xFF 0xD8
     * 这两个字节叫 SOI（Start Of Image）。
     */
    bool valid_jpeg =
        fb->format == PIXFORMAT_JPEG &&
        fb->buf != NULL &&
        fb->len >= 4 &&
        fb->buf[0] == 0xFF &&
        fb->buf[1] == 0xD8;

    if (!valid_jpeg) {
        ESP_LOGE(TAG, "Capture failed: invalid JPEG frame, format=%d len=%u",
                 (int)fb->format, (unsigned)fb->len);

        /*
         * esp_camera_fb_get() 取得的 framebuffer 必须归还给驱动。
         * 否则 Camera 以后可能再也拿不到可用 framebuffer。
         */
        esp_camera_fb_return(fb);
        xSemaphoreGive(s_camera_mutex);
        return ESP_FAIL;
    }

    /*
     * 这里是一个非常重要的内存生命周期问题。
     *
     * fb->buf 属于 Camera 驱动，不属于我们。
     * 当调用 esp_camera_fb_return(fb) 后，这块内存随时可能被下一帧覆盖。
     *
     * 因此不能：
     *
     *     s_latest_jpeg = fb->buf;   // 错误做法
     *
     * 而必须自己 malloc 一块内存，再把 JPEG 复制过去。
     */
    uint8_t *new_jpeg = malloc(fb->len);

    if (new_jpeg == NULL) {
        ESP_LOGE(TAG, "Capture failed: malloc(%u) failed", (unsigned)fb->len);
        esp_camera_fb_return(fb);
        xSemaphoreGive(s_camera_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* 把 Camera framebuffer 中的 JPEG 完整复制到我们自己的堆内存。 */
    memcpy(new_jpeg, fb->buf, fb->len);

    /* 在归还 fb 前，把后面需要的元数据先保存下来。 */
    size_t new_len = fb->len;
    unsigned width = fb->width;
    unsigned height = fb->height;

    /*
     * JPEG 已经复制完毕，所以尽快归还 Camera framebuffer。
     * 然后释放 Camera 锁，让 /video 可以继续抓下一帧。
     */
    esp_camera_fb_return(fb);
    xSemaphoreGive(s_camera_mutex);

    /*
     * 接下来更新“最近一张 BOOT 照片”。
     * 这时操作的已经不是 Camera，而是我们自己的 s_latest_jpeg 缓存，
     * 所以改用 s_photo_mutex。
     */
    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);

    /*
     * 只保留最新一张照片。
     * 如果不 free 旧照片，每按一次 BOOT 都 malloc 一块新内存，会造成内存泄漏。
     *
     * free(NULL) 在 C 里是安全的，所以第一次拍照时也可以直接调用。
     */
    free(s_latest_jpeg);

    s_latest_jpeg = new_jpeg;
    s_latest_jpeg_len = new_len;

    /* 每成功拍一张，计数器 +1。 */
    ++s_photo_count;

    /* 在锁内先把 count 拷出来，后面日志打印无需继续持锁。 */
    uint32_t count = s_photo_count;

    xSemaphoreGive(s_photo_mutex);

    ESP_LOGI(TAG, "PHOTO #" PRIu32 " OK: %ux%u, %u bytes",
             count, width, height, (unsigned)new_len);
    ESP_LOGI(TAG, "网页会自动刷新 BOOT 拍照结果；实时视频仍继续运行");

    return ESP_OK;
}

/* ========================================================================
 * 9. 初始化 BOOT 按钮 GPIO
 * ========================================================================
 */
static void init_boot_button(void)
{
    gpio_config_t io_conf = {
        /* pin_bit_mask 使用位图表示要配置哪个 GPIO。 */
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,

        /* BOOT 只读取电平，所以设置成输入模式。 */
        .mode = GPIO_MODE_INPUT,

        /*
         * 开启内部上拉：
         *   未按 BOOT -> GPIO0 保持高电平 1
         *   按下 BOOT -> GPIO0 被拉到低电平 0
         */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,

        /*
         * 当前示例不用 GPIO 中断，而是由 FreeRTOS Task 每隔 20ms 轮询一次。
         * 对初学者更直观，也足够完成拍照按钮测试。
         */
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

/* ========================================================================
 * 10. BOOT 按键监听 FreeRTOS Task
 * ========================================================================
 *
 * 这个 Task 一直运行，负责检测 GPIO0：
 *
 *   1 -> 0   表示用户刚刚按下 BOOT
 *
 * 为什么不用简单地“只要 level==0 就拍照”？
 * 因为用户按住按钮 1 秒时，循环会执行很多次，就会连续拍几十张照片。
 *
 * 所以这里同时做两件事：
 *   - 检测下降沿 1 -> 0；
 *   - 拍完后等待用户松手。
 */
static void boot_button_task(void *arg)
{
    /* 当前 Task 不需要外部参数。 */
    (void)arg;

    /* 记录上一次读取到的 GPIO 状态，用于判断 1 -> 0。 */
    int previous = gpio_get_level(BOOT_BUTTON_GPIO);

    while (1) {
        /* 读取当前 BOOT GPIO 电平。 */
        int level = gpio_get_level(BOOT_BUTTON_GPIO);

        /*
         * previous == 1 且 level == 0
         * 表示刚刚发生了一个下降沿，也就是 BOOT 刚被按下。
         */
        if (previous == 1 && level == 0) {

            /*
             * 机械按钮会抖动，例如一次按压可能短时间出现：
             *   1 0 1 0 0 1 0 ...
             *
             * 所以先等待 35ms，再确认一次。
             */
            vTaskDelay(pdMS_TO_TICKS(35));

            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                /* 35ms 后仍然为低电平，认为这次按键有效。 */
                capture_one_photo();

                /*
                 * 等待用户松开按钮。
                 * 只有 GPIO0 恢复为 1 后才继续检测下一次按压。
                 * 这样一次长按也只会拍一张。
                 */
                while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
        }

        /* 保存本轮状态，为下一轮下降沿检测做准备。 */
        previous = gpio_get_level(BOOT_BUTTON_GPIO);

        /*
         * 每 20ms 检查一次按钮，已经足够快，
         * 同时避免这个 Task 一直占满 CPU。
         */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ========================================================================
 * 11. 80 端口 HTTP Handler：网页 / 状态 / 静态照片
 * ========================================================================
 */

/*
 * GET /
 *
 * 浏览器打开 http://192.168.4.1 时进入这里。
 * 服务器把前面定义的 s_index_html 整页发送给浏览器。
 */
static esp_err_t root_handler(httpd_req_t *req)
{
    /* 告诉浏览器返回的是 UTF-8 HTML。 */
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    /* 测试阶段不希望浏览器缓存旧页面。 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* HTTPD_RESP_USE_STRLEN 表示由库自己调用 strlen() 计算字符串长度。 */
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

/*
 * GET /status
 *
 * JavaScript 每 500ms 调一次。
 * 返回类似：
 *
 *   {"count":3,"bytes":3125}
 *
 * count：BOOT 已经成功拍了多少张
 * bytes：最近一张 JPEG 的大小
 */
static esp_err_t status_handler(httpd_req_t *req)
{
    /* 这个局部数组用来拼出一个很小的 JSON 字符串。 */
    char json[96];

    uint32_t count;
    size_t bytes;

    /*
     * s_photo_count / s_latest_jpeg_len 可能正被 BOOT Task 更新，
     * 所以读取时也要拿 s_photo_mutex。
     */
    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);
    count = s_photo_count;
    bytes = s_latest_jpeg_len;
    xSemaphoreGive(s_photo_mutex);

    /* 把当前状态格式化成 JSON。 */
    int len = snprintf(json, sizeof(json),
                       "{\"count\":%" PRIu32 ",\"bytes\":%u}",
                       count, (unsigned)bytes);

    /* 告诉浏览器这是 JSON。 */
    httpd_resp_set_type(req, "application/json");

    /* 禁止缓存，否则网页可能读到旧的 count。 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");

    return httpd_resp_send(req, json, len);
}

/*
 * GET /photo.jpg
 *
 * 返回最近一次 BOOT 拍下来的静态 JPEG。
 */
static esp_err_t photo_handler(httpd_req_t *req)
{
    /*
     * 从开始检查 s_latest_jpeg 到发送结束之间一直持有照片锁。
     * 原因是：BOOT Task 可能随时 free(s_latest_jpeg) 并换成一张新照片。
     */
    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);

    /* 用户还没有按过 BOOT，就没有可返回的照片。 */
    if (s_latest_jpeg == NULL || s_latest_jpeg_len == 0) {
        xSemaphoreGive(s_photo_mutex);

        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "还没有照片，请先按一次 BOOT 按钮。");
    }

    /* 让浏览器把返回的二进制内容当 JPEG 图片显示。 */
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");

    /*
     * 发送期间保持 s_photo_mutex。
     * 否则可能发生：
     *
     *   HTTP 正在读取 s_latest_jpeg
     *              ↓
     *   BOOT Task free(s_latest_jpeg)
     *              ↓
     *   HTTP 继续访问已经释放的内存
     *
     * QVGA JPEG 很小，因此这里持锁时间通常很短。
     */
    esp_err_t err = httpd_resp_send(req,
                                    (const char *)s_latest_jpeg,
                                    s_latest_jpeg_len);

    xSemaphoreGive(s_photo_mutex);
    return err;
}

/* ========================================================================
 * 12. 81 端口：MJPEG 实时视频
 * ========================================================================
 *
 * MJPEG 可以理解成：
 *
 *   HTTP response
 *       |
 *       +-- boundary
 *       +-- JPEG frame #1
 *       +-- boundary
 *       +-- JPEG frame #2
 *       +-- boundary
 *       +-- JPEG frame #3
 *       +-- ...
 *
 * /video 连接不会像普通网页那样“发送完就关闭”，
 * 而是在 while(1) 中不断抓帧、不断发送，直到浏览器断开。
 */
static esp_err_t video_handler(httpd_req_t *req)
{
    /*
     * 先设置整个 HTTP Response 的 Content-Type。
     * 浏览器看到 multipart/x-mixed-replace 后，会按 MJPEG 方式处理后续数据。
     */
    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    /* 实时视频绝不能被浏览器缓存。 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    /*
     * 允许跨域访问。
     * 当前主页和视频虽然都是 192.168.4.1，但端口不同（80 / 81），
     * 从浏览器安全模型看属于不同 Origin，因此显式允许跨域更稳妥。
     */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    ESP_LOGI(TAG, "MJPEG client connected: /video");

    /*
     * frame_copy：video_handler 自己维护的一块 JPEG 临时缓存。
     *
     * 为什么还要复制一份？
     *
     * 如果直接拿着 Camera 的 fb->buf 去慢慢做 Wi-Fi 发送，
     * 那么在整个网络发送期间 Camera framebuffer 都不能归还，
     * BOOT 拍照也要一直等。
     *
     * 因此这里采用：
     *
     *   Camera framebuffer
     *          |
     *          | memcpy（很快）
     *          v
     *     frame_copy
     *          |
     *          +---- Wi-Fi 慢慢发送
     *
     * Camera framebuffer 在 memcpy 完成后立即归还。
     */
    uint8_t *frame_copy = NULL;

    /* 当前 frame_copy 已经分配了多少容量。 */
    size_t frame_capacity = 0;

    /* 每一帧 multipart JPEG 前面的小 HTTP header。 */
    char part_header[96];

    /*
     * 只要浏览器保持 /video 连接，这个循环就一直运行。
     * 浏览器关闭页面或断开 Wi-Fi 后，send_chunk 会失败，然后跳出循环。
     */
    while (1) {
        size_t frame_len = 0;

        /*
         * ===== 第一步：从 Camera 抓一帧 =====
         *
         * 和 BOOT 拍照共用 Camera，所以先获得 s_camera_mutex。
         */
        xSemaphoreTake(s_camera_mutex, portMAX_DELAY);

        camera_fb_t *fb = esp_camera_fb_get();

        if (fb == NULL) {
            xSemaphoreGive(s_camera_mutex);
            ESP_LOGE(TAG, "Video capture failed: esp_camera_fb_get() returned NULL");
            res = ESP_FAIL;
            break;
        }

        /* 同样检查 JPEG 格式和 FF D8 SOI 标记。 */
        bool valid_jpeg =
            fb->format == PIXFORMAT_JPEG &&
            fb->buf != NULL &&
            fb->len >= 4 &&
            fb->buf[0] == 0xFF &&
            fb->buf[1] == 0xD8;

        if (!valid_jpeg) {
            ESP_LOGW(TAG, "Video skipped invalid JPEG frame: format=%d len=%u",
                     (int)fb->format, (unsigned)fb->len);

            esp_camera_fb_return(fb);
            xSemaphoreGive(s_camera_mutex);

            /* 稍等 10ms 再试下一帧，避免异常时疯狂空转。 */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /*
         * ===== 第二步：确保 frame_copy 足够大 =====
         *
         * 不同 JPEG 帧压缩后的字节数并不完全相同：
         *   frame A 可能 3100 bytes
         *   frame B 可能 4200 bytes
         *
         * 如果当前缓存容量不够，就用 realloc() 扩大。
         *
         * realloc 的好处是：
         *   - 容量够时不需要重新申请；
         *   - 容量不够时才扩容；
         *   - 避免每一帧都 malloc/free。
         */
        if (fb->len > frame_capacity) {
            uint8_t *new_buffer = realloc(frame_copy, fb->len);

            if (new_buffer == NULL) {
                ESP_LOGE(TAG, "Video realloc(%u) failed", (unsigned)fb->len);

                esp_camera_fb_return(fb);
                xSemaphoreGive(s_camera_mutex);

                res = ESP_ERR_NO_MEM;
                break;
            }

            frame_copy = new_buffer;
            frame_capacity = fb->len;
        }

        /* 把这一帧 JPEG 复制到 video_handler 自己的缓存。 */
        memcpy(frame_copy, fb->buf, fb->len);
        frame_len = fb->len;

        /*
         * 到这里已经不再需要 Camera 的 framebuffer。
         * 尽快归还并释放锁，让 BOOT 拍照有机会插进来。
         */
        esp_camera_fb_return(fb);
        xSemaphoreGive(s_camera_mutex);

        /*
         * ===== 第三步：把这一帧按 multipart 格式发给浏览器 =====
         */

        /* 3.1 先发送 boundary，表示“新的一帧开始了”。 */
        res = httpd_resp_send_chunk(req,
                                    STREAM_BOUNDARY,
                                    HTTPD_RESP_USE_STRLEN);
        if (res != ESP_OK) {
            break;
        }

        /*
         * 3.2 生成这一帧自己的 HTTP header，例如：
         *
         *   Content-Type: image/jpeg
         *   Content-Length: 3125
         */
        int header_len = snprintf(part_header,
                                  sizeof(part_header),
                                  STREAM_PART,
                                  frame_len);

        if (header_len < 0 || header_len >= (int)sizeof(part_header)) {
            ESP_LOGE(TAG, "MJPEG part header too long");
            res = ESP_FAIL;
            break;
        }

        /* 3.3 把这一帧的 header 发送出去。 */
        res = httpd_resp_send_chunk(req, part_header, header_len);
        if (res != ESP_OK) {
            break;
        }

        /* 3.4 真正发送 JPEG 二进制数据。 */
        res = httpd_resp_send_chunk(req,
                                    (const char *)frame_copy,
                                    frame_len);
        if (res != ESP_OK) {
            break;
        }

        /*
         * 稍微让出 CPU，避免 QVGA 下无限制高速循环抓帧。
         *
         * 20ms 并不意味着一定有 50FPS，实际帧率还取决于：
         *   - OV3660 输出速度
         *   - JPEG 编码速度
         *   - ESP32-S3 CPU
         *   - Wi-Fi 吞吐
         *   - 浏览器接收速度
         */
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /*
     * 浏览器断开以后，释放 video_handler 自己维护的 JPEG 缓存。
     */
    free(frame_copy);

    /*
     * 发送长度为 0 的 chunk，表示 chunked response 结束。
     * 如果客户端已经强制断开，这里返回错误也没有关系。
     */
    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG,
             "MJPEG client disconnected: /video, result=%s",
             esp_err_to_name(res));

    return res;
}

/* ========================================================================
 * 13. 启动两个 HTTP Server
 * ========================================================================
 *
 * Server #1：80 端口
 *   /            -> HTML 首页
 *   /status      -> BOOT 拍照状态 JSON
 *   /photo.jpg   -> 最近一张 BOOT 静态照片
 *
 * Server #2：81 端口
 *   /video       -> MJPEG 实时视频流
 *
 * 为什么不用同一个 server？
 *
 * /video handler 进入以后会在 while(1) 中持续发送帧，是一个长连接。
 * 为了避免它影响普通 HTTP API，所以单独开第二个 HTTP Server。
 */
static void start_http_server(void)
{
    /* --------------------------------------------------------------------
     * 13.1 普通 Web Server：80 端口
     * --------------------------------------------------------------------
     */

    /*
     * HTTPD_DEFAULT_CONFIG() 返回 ESP-IDF 推荐的默认 HTTP Server 配置。
     */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    /*
     * HTTP handler 会处理 HTML、JSON、JPEG 等数据，给 8KB Task 栈更稳妥。
     */
    config.stack_size = 8192;

    /* 最多允许注册多少个 URL handler。当前只用 3 个。 */
    config.max_uri_handlers = 8;

    /* server 是 HTTP Server 的句柄，后面注册 URL 都需要它。 */
    httpd_handle_t server = NULL;

    /* 启动第一个 HTTP Server。默认 server_port 通常是 80。 */
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    /*
     * 定义 GET /
     * 浏览器访问主页时调用 root_handler()。
     */
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };

    /*
     * 定义 GET /status
     * JavaScript 用它轮询 BOOT 拍照次数。
     */
    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };

    /*
     * 定义 GET /photo.jpg
     * 返回最近一张 BOOT 静态照片。
     */
    httpd_uri_t photo_uri = {
        .uri = "/photo.jpg",
        .method = HTTP_GET,
        .handler = photo_handler,
        .user_ctx = NULL,
    };

    /* 把三个 URL 注册到 80 端口的 server。 */
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &photo_uri));

    ESP_LOGI(TAG, "Web server started : http://192.168.4.1");

    /* --------------------------------------------------------------------
     * 13.2 MJPEG Stream Server：81 端口
     * --------------------------------------------------------------------
     */

    /* 再创建一份默认 HTTP Server 配置。 */
    httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();

    /* 外部浏览器访问的 TCP 端口改成 81。 */
    stream_config.server_port = STREAM_PORT;

    /*
     * ESP-IDF HTTP Server 除了对外的 server_port，内部还会使用 ctrl_port。
     * 两个 HTTP Server 如果 ctrl_port 一样，会发生端口冲突。
     *
     * 因此第二个 server 把默认 ctrl_port +1。
     */
    stream_config.ctrl_port += 1;

    stream_config.stack_size = 8192;
    stream_config.max_uri_handlers = 4;

    httpd_handle_t stream_server = NULL;

    /* 启动第二个 HTTP Server。 */
    ESP_ERROR_CHECK(httpd_start(&stream_server, &stream_config));

    /*
     * 定义 GET /video
     * 一旦浏览器连接，就进入 video_handler() 持续发送 MJPEG。
     */
    httpd_uri_t video_uri = {
        .uri = "/video",
        .method = HTTP_GET,
        .handler = video_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(stream_server, &video_uri));

    ESP_LOGI(TAG,
             "Video server started: http://192.168.4.1:%d/video",
             STREAM_PORT);
}

/* ========================================================================
 * 14. 启动 Wi-Fi SoftAP
 * ========================================================================
 *
 * SoftAP = Software Access Point。
 * 也就是让 ESP32-S3 自己成为一个 Wi-Fi 热点，而不是加入已有路由器。
 */
static void start_wifi_ap(void)
{
    /*
     * 初始化 ESP-IDF 网络接口层。
     * Wi-Fi / Ethernet 等网络功能都建立在 esp_netif 之上。
     */
    ESP_ERROR_CHECK(esp_netif_init());

    /*
     * 创建默认事件循环。
     * Wi-Fi 驱动会通过事件系统通知“启动、连接、断开”等状态。
     */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /*
     * 创建默认 Wi-Fi AP 网络接口。
     * 默认 SoftAP IP 通常就是 192.168.4.1。
     */
    esp_netif_create_default_wifi_ap();

    /*
     * 取得官方默认 Wi-Fi 驱动配置，再初始化 Wi-Fi 驱动。
     */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /*
     * 先把整个结构体清零，再逐项填写 AP 参数。
     */
    wifi_config_t wifi_config = {0};

    /* 设置热点名称和密码。 */
    strlcpy((char *)wifi_config.ap.ssid,
            AP_SSID,
            sizeof(wifi_config.ap.ssid));
    strlcpy((char *)wifi_config.ap.password,
            AP_PASSWORD,
            sizeof(wifi_config.ap.password));

    /* SSID 实际长度。 */
    wifi_config.ap.ssid_len = strlen(AP_SSID);

    /* Wi-Fi 信道。 */
    wifi_config.ap.channel = AP_CHANNEL;

    /* 测试时最多允许 2 台设备同时连接。 */
    wifi_config.ap.max_connection = 2;

    /* 使用 WPA2-PSK 密码认证。 */
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    /*
     * PMF（Protected Management Frames）配置。
     * capable=true  表示支持 PMF；
     * required=false 表示不强制客户端必须支持。
     */
    wifi_config.ap.pmf_cfg.capable = true;
    wifi_config.ap.pmf_cfg.required = false;

    /* 把 Wi-Fi 工作模式设置成 AP。 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    /* 把上面的 SSID / 密码 / channel 等配置写入 AP 接口。 */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    /* 真正启动 Wi-Fi。 */
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started");
    ESP_LOGI(TAG, "SSID     : %s", AP_SSID);
    ESP_LOGI(TAG, "Password : %s", AP_PASSWORD);
    ESP_LOGI(TAG, "Web      : http://192.168.4.1");
    ESP_LOGI(TAG, "Video    : http://192.168.4.1:%d/video", STREAM_PORT);
}

/* ========================================================================
 * 15. 程序入口 app_main()
 * ========================================================================
 *
 * ESP-IDF 启动流程大致是：
 *
 *   ROM Bootloader
 *        ↓
 *   Second Stage Bootloader
 *        ↓
 *   FreeRTOS / ESP-IDF 初始化
 *        ↓
 *   app_main()
 *
 * 所以 app_main() 就相当于这个工程的主入口。
 */
void app_main(void)
{
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "ESP32-S3-CAM BOOT PHOTO + MJPEG VIDEO TEST");
    ESP_LOGI(TAG, "BOOT(GPIO0) -> capture JPEG -> browser display");
    ESP_LOGI(TAG, "MJPEG video -> http://192.168.4.1:%d/video", STREAM_PORT);
    ESP_LOGI(TAG, "MIC / MAX98357A / Speaker 保持接线，不需要拔除");
    ESP_LOGI(TAG, "====================================================");

    /*
     * -------------------- 15.1 初始化 NVS --------------------
     *
     * ESP-IDF 的 Wi-Fi 驱动依赖 NVS（Non-Volatile Storage）。
     * 即使我们这个示例没有自己保存业务数据，也需要先初始化 NVS。
     */
    esp_err_t ret = nvs_flash_init();

    /*
     * 如果 NVS 分区格式过旧或没有空闲页，就擦除后重新初始化。
     * 这是 ESP-IDF Wi-Fi 示例中很常见的标准写法。
     */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    /*
     * -------------------- 15.2 创建两把 Mutex --------------------
     */
    s_photo_mutex = xSemaphoreCreateMutex();
    s_camera_mutex = xSemaphoreCreateMutex();

    if (s_photo_mutex == NULL || s_camera_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    /*
     * -------------------- 15.3 初始化 Camera --------------------
     *
     * ESP_ERROR_CHECK：
     * 如果 init_camera() 不是 ESP_OK，会直接打印错误并终止当前程序流程，
     * 避免 Camera 都没起来还继续启动 Web Server。
     */
    ESP_ERROR_CHECK(init_camera());

    /* 初始化 BOOT(GPIO0) 输入。 */
    init_boot_button();

    /*
     * -------------------- 15.4 启动网络 --------------------
     *
     * 先创建 Wi-Fi AP，再启动 HTTP Server。
     * 这样浏览器连接热点后即可访问网页。
     */
    start_wifi_ap();
    start_http_server();

    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "READY");
    ESP_LOGI(TAG, "1) 电脑连接 Wi-Fi: %s", AP_SSID);
    ESP_LOGI(TAG, "2) 密码: %s", AP_PASSWORD);
    ESP_LOGI(TAG, "3) 浏览器主页: http://192.168.4.1");
    ESP_LOGI(TAG, "4) 实时视频URL: http://192.168.4.1:%d/video", STREAM_PORT);
    ESP_LOGI(TAG, "5) 每短按一次 BOOT，还会保存一张最新照片");
    ESP_LOGI(TAG, "====================================================");

    /*
     * -------------------- 15.5 创建 BOOT 监听任务 --------------------
     *
     * 参数含义：
     *
     * xTaskCreate(
     *     boot_button_task,      // Task 入口函数
     *     "boot_button_task",   // Task 名字，调试时可看到
     *     4096,                  // Task 栈大小
     *     NULL,                  // 不传入参数
     *     5,                     // Task 优先级
     *     NULL                   // 不保存 Task Handle
     * );
     *
     * app_main() 执行到这里以后可以返回，
     * 后续 HTTP Server 和 boot_button_task 都由 FreeRTOS 调度运行。
     */
    xTaskCreate(boot_button_task,
                "boot_button_task",
                4096,
                NULL,
                5,
                NULL);
}
