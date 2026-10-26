# camera_boot_http_test

这是在前面 Camera / MIC / Speaker 单项及综合测试通过后的一个简单交互测试：

> **浏览器实时预览摄像头视频，同时每按一次 ESP32-S3-CAM 的 BOOT 按钮，还会额外拍一张照片，并在网页里显示最新照片。**

## 当前功能

程序启动后会创建 Wi-Fi 热点：

```text
SSID     : S3CAM-Photo
Password : 12345678
```

电脑连接后可以访问：

```text
主页 / BOOT 照片：
http://192.168.4.1

实时 MJPEG 视频：
http://192.168.4.1:81/video
```

主页已经自动嵌入 `/video`，所以通常只需要打开：

```text
http://192.168.4.1
```

就可以同时看到：

1. 实时摄像头视频；
2. BOOT 按键拍下来的最新一张静态照片。

## 为什么用 Wi-Fi + HTTP

相比把 JPEG 原始字节通过串口输出，这种方式更直观：

- 不需要写 PC 端接收程序；
- 不需要处理串口二进制数据和日志混在一起的问题；
- 浏览器天然就能显示 JPEG / MJPEG；
- 开发板自己创建 Wi-Fi 热点，不依赖家庭/公司路由器。

## 为什么视频使用 81 端口

MJPEG 视频流本质上是一个长期保持的 HTTP 请求：

```text
/video
  ↓
JPEG #1
JPEG #2
JPEG #3
...
```

因此程序使用两个 HTTP server：

```text
80 端口
├── /
├── /status
└── /photo.jpg

81 端口
└── /video
```

这样实时视频不会阻塞主页、状态查询和 BOOT 拍照结果。

## 接线原则

**保持当前完整接线不动。**

- Camera：正常使用；
- MIC：保持 GPIO42 / GPIO41 / GPIO1 接线，但本程序不初始化 MIC；
- MAX98357A + Speaker：保持 GPIO14 / GPIO3 / GPIO46 接线，但本程序不初始化 Speaker；
- BOOT：使用板载 BOOT 按钮，对应 GPIO0，按下为低电平。

## 编译烧录

```bash
cd s3cam/camera_boot_http_test
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

如果当前 shell 再次出现 `xtensa-esp32s3-elf-gcc not found`，先恢复之前已经确认过的 ESP-IDF 6.1 工具链 PATH。

## 使用步骤

程序正常启动后串口会打印类似：

```text
READY
1) 电脑连接 Wi-Fi: S3CAM-Photo
2) 密码: 12345678
3) 浏览器主页: http://192.168.4.1
4) 实时视频URL: http://192.168.4.1:81/video
5) 每短按一次 BOOT，还会保存一张最新照片
```

### 1. 电脑连接开发板 Wi-Fi

在 macOS Wi-Fi 菜单中选择：

```text
S3CAM-Photo
```

密码：

```text
12345678
```

macOS 可能提示“此 Wi-Fi 没有互联网连接”，这是正常的，因为这个热点只用于电脑和 ESP32-S3-CAM 本地通信。

### 2. 打开主页

浏览器输入：

```text
http://192.168.4.1
```

网页上方应该直接出现实时视频预览。

如果只想单独打开视频流，也可以直接访问：

```text
http://192.168.4.1:81/video
```

### 3. 短按 BOOT 拍静态照片

每短按一次 BOOT：

```text
BOOT(GPIO0)
    ↓
和实时视频流协调 Camera 使用权
    ↓
Camera 拍一张 JPEG
    ↓
保存为内存中的“最新照片”
    ↓
网页自动发现 photo_count 变化
    ↓
重新加载 /photo.jpg
    ↓
页面下方显示最新静态照片
```

串口会看到类似：

```text
BOOT pressed -> capturing photo...
PHOTO #1 OK: 320x240, 4xxx bytes
```

实时视频不会因为这次拍照而永久停止。

## Camera 并发保护

现在有两个地方会访问 Camera：

```text
BOOT 拍照任务
      │
      ├── s_camera_mutex ── Camera
      │
/video 实时视频
```

程序使用 `s_camera_mutex` 保证 BOOT 拍照和 MJPEG 流不会同时调用 `esp_camera_fb_get()`。

视频 handler 取得一帧后，会先把 JPEG 复制到自己的缓存，然后立即：

```text
esp_camera_fb_return()
释放 s_camera_mutex
```

之后再慢慢通过 Wi-Fi 把复制出来的 JPEG 发给浏览器。

这样网络传输较慢时，也不会长时间占着 Camera，从而尽量保证 BOOT 拍照仍能及时响应。

## 关于 BOOT 按钮

BOOT 是 GPIO0，也是 ESP32-S3 的启动下载相关引脚。

正常程序已经启动后，**短按 BOOT** 作为普通按键使用没有问题。

但不要：

```text
一直按住 BOOT + RESET
```

或者：

```text
一直按住 BOOT + 重新上电
```

否则芯片可能进入下载模式。

## 当前图像参数

为了继续沿用已经验证稳定的 Camera 配置：

```text
JPEG
320 x 240 (QVGA)
quality = 12
单 framebuffer
DRAM
```

这个配置优先保证实时视频链路稳定。

确认 MJPEG 视频和 BOOT 拍照同时正常以后，再考虑提高到 VGA / SVGA，或者启用 PSRAM、双 framebuffer 来提高画质和帧率。
