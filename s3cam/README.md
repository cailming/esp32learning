# ESP32-S3-CAM 硬件测试代码

本目录用于验证 ESP32-S3-CAM 当前整机接线是否正确，以及摄像头、I2S 数字麦克风、MAX98357A + 喇叭是否能独立正常工作。

## 测试原则

**所有硬件保持当前完整接线，不为了单项测试而拔线。**

也就是说，测试 `camera_test`、`mic_test`、`speaker_test` 时，摄像头、麦克风、MAX98357A 和喇叭都保持连接，只是每个测试程序只初始化自己需要的外设。

## 当前外部接线基准

### I2S 数字麦克风

| 麦克风 | ESP32-S3-CAM |
| --- | --- |
| SCK / BCLK | GPIO42 |
| WS / LRCLK | GPIO41 |
| SD | GPIO1 |
| VDD | 3V3 |
| GND | 公共 GND |
| L/R | 公共 GND |

### MAX98357A + 喇叭

| MAX98357A | ESP32-S3-CAM |
| --- | --- |
| BCLK | GPIO14 |
| LRC | GPIO3 |
| DIN | GPIO46 |
| VIN | 5V |
| GND | 公共 GND |
| SD | 暂不接 |
| GAIN | 暂不接 |

喇叭红线接 MAX98357A `+`，黑线接 MAX98357A `-`。喇叭黑线不要接 ESP32 GND。

## 板载摄像头 GPIO

当前摄像头测试按本板已确认的 GPIO 区域配置：

| 信号 | GPIO |
| --- | ---: |
| XCLK | 15 |
| SIOD | 4 |
| SIOC | 5 |
| D0 | 11 |
| D1 | 9 |
| D2 | 8 |
| D3 | 10 |
| D4 | 12 |
| D5 | 18 |
| D6 | 17 |
| D7 | 16 |
| VSYNC | 6 |
| HREF | 7 |
| PCLK | 13 |
| PWDN | 未使用（-1） |
| RESET | 未使用（-1） |

这组摄像头 GPIO 与当前麦克风 `42/41/1`、功放 `14/3/46` 不冲突。

## 测试项目

### 1. camera_test

目标：只验证板载摄像头初始化和连续取帧。

通过条件：
- Camera 初始化成功；
- 能识别 Sensor；
- 连续抓取 30 帧；
- 每帧宽、高、长度均正常；
- 串口最后打印 `CAMERA TEST PASS`。

### 2. mic_test

目标：只验证 I2S 数字麦克风是否有有效 PCM 数据。

程序同时观察左右两个 I2S 声道，不强行假定 L/R 接地后数据一定落在哪一边。

建议：烧录后先保持环境安静约 2 秒，让程序建立噪声基线，然后拍手或说话。

通过条件：声音出现时幅度明显增大，串口打印 `MIC TEST PASS`。

### 3. speaker_test

目标：只验证 ESP32-S3 -> I2S -> MAX98357A -> 喇叭链路。

程序会以较低音量循环播放：

```text
440 Hz -> 静音 -> 880 Hz -> 静音
```

共播放 3 轮。

通过条件：能稳定听到两种不同音高，串口打印 `SPEAKER TEST PASS`。

## 推荐测试顺序

```text
所有线保持接好
    |
    +--> camera_test
    |
    +--> mic_test
    |
    +--> speaker_test
```

三个单项都通过后，再继续做 `mic + speaker` 回环测试和 `camera + mic + speaker` 综合测试。

## 编译、烧录、监控

进入任意一个测试目录，例如：

```bash
cd s3cam/mic_test
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

串口名请替换为电脑上实际识别到的端口。

退出 monitor：

```text
Ctrl + ]
```

## 关于 GPIO3 / GPIO46

GPIO3 和 GPIO46 是 ESP32-S3 的启动配置相关引脚。当前接线保持不变进行测试，正好可以验证最终整机状态下能否稳定启动。

如果出现“接着 MAX98357A 时偶尔无法启动、拔掉功放后恢复”的现象，请保留现场，不要先拆线，记录串口启动日志，再调整这两个 GPIO 的方案。
