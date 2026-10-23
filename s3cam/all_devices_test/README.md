# all_devices_test

ESP32-S3-CAM 三设备并行综合测试：

- 板载 Camera 持续抓取 JPEG；
- I2S MIC 持续采集 PCM；
- MAX98357A + Speaker 同时播放 440 Hz / 880 Hz 测试音。

三个设备会在不同 FreeRTOS 任务中并行工作，用于检查 GPIO、I2S、DMA、内存和并发资源是否存在冲突。

## 接线

保持已经验证通过的完整接线，不拆任何线。

### MIC

```text
SCK  -> GPIO42
WS   -> GPIO41
SD   -> GPIO1
VDD  -> 3V3
GND  -> 公共 GND
L/R  -> 公共 GND
```

### MAX98357A

```text
BCLK -> GPIO14
LRC  -> GPIO3
DIN  -> GPIO46
VIN  -> 5V
GND  -> 公共 GND
```

### Speaker

```text
红线 -> MAX98357A +
黑线 -> MAX98357A -
```

## 测试过程

程序启动后：

1. 初始化 Camera；
2. 初始化 MIC（I2S0 RX）；
3. 初始化 Speaker（I2S1 TX）；
4. 同时启动三个 FreeRTOS 任务；
5. Camera 连续抓取 30 帧；
6. MIC 持续读取 PCM；
7. Speaker 播放 3 轮 440 Hz / 880 Hz 测试音；
8. 汇总结果。

## 运行

```bash
cd s3cam/all_devices_test
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

如果已经对同目录执行过 `set-target`，后续只需要：

```bash
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

## 通过标准

串口最终应出现：

```text
ALL DEVICES TEST PASS
```

同时还要人工确认：

- 喇叭实际能听到 440 Hz / 880 Hz 两种不同音高；
- Camera 30/30 帧成功；
- MIC 在 Speaker 播放期间仍持续输出非零 PCM span；
- ESP32 没有重启、崩溃、看门狗或 I2S / Camera 错误。

注意：综合测试的重点是验证三个已经分别通过单项测试的设备能否**同时稳定运行**，不是重新验证音质或画质。
