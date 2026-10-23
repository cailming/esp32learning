# camera_test

ESP32-S3-CAM 板载摄像头最小化测试。

## 特点

- MIC、MAX98357A、喇叭保持完整接线；
- 本程序不初始化音频 GPIO；
- Camera 使用 QVGA JPEG；
- framebuffer 放在 DRAM，减少第一轮测试对 PSRAM 配置的依赖；
- 连续抓取 30 帧。

## 运行

```bash
cd s3cam/camera_test
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

第一次构建会通过 ESP-IDF Component Manager 下载 `espressif/esp32-camera`。

## 通过标志

```text
CAMERA TEST PASS: 30/30 frames captured successfully
```

如果 `esp_camera_init()` 失败，先记录完整串口错误，不要拔掉 MIC 或 MAX98357A 接线。
