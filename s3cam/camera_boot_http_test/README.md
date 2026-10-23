# camera_boot_http_test

这是在前面 Camera / MIC / Speaker 单项及综合测试通过后的一个简单交互测试：

> **每按一次 ESP32-S3-CAM 的 BOOT 按钮，就拍一张照片，并在电脑浏览器里显示最新照片。**

## 为什么用 Wi-Fi + HTTP

相比把 JPEG 原始字节通过串口输出，这种方式更直观：

- 不需要写 PC 端接收程序；
- 不需要处理串口二进制数据和日志混在一起的问题；
- 浏览器天然就能显示 JPEG；
- 开发板自己创建 Wi-Fi 热点，不依赖家庭/公司路由器。

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

程序正常启动后串口会打印：

```text
READY
Wi-Fi SSID : S3CAM-Photo
Password   : 12345678
Browser    : http://192.168.4.1
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

### 2. 打开网页

浏览器输入：

```text
http://192.168.4.1
```

第一次打开时会显示“等待第一次拍照”。

### 3. 短按 BOOT

每短按一次 BOOT：

```text
BOOT(GPIO0)
    ↓
Camera 拍一张 JPEG
    ↓
保存为内存中的“最新照片”
    ↓
网页自动发现 photo_count 变化
    ↓
重新加载 /photo.jpg
    ↓
电脑显示最新照片
```

串口会看到类似：

```text
BOOT pressed -> capturing photo...
PHOTO #1 OK: 320x240, 4xxx bytes
```

再按一次：

```text
PHOTO #2 OK: 320x240, 4xxx bytes
```

浏览器会自动更新，不需要手工刷新页面。

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

## 当前版本照片参数

为了继续沿用已经验证稳定的 Camera 配置：

```text
JPEG
320 x 240 (QVGA)
quality = 12
单 framebuffer
DRAM
```

确认按键拍照和网页传输正常以后，再考虑提高到 VGA / SVGA 或使用 PSRAM 保存更大的 JPEG。
