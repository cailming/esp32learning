# speaker_test

MAX98357A + 喇叭独立测试。

## 当前接线

```text
MAX98357A BCLK -> GPIO14
MAX98357A LRC  -> GPIO3
MAX98357A DIN  -> GPIO46
MAX98357A VIN  -> 5V
MAX98357A GND  -> 公共 GND
MAX98357A SD   -> 暂不接
MAX98357A GAIN -> 暂不接

喇叭红线 -> MAX98357A +
喇叭黑线 -> MAX98357A -
```

MIC 继续保持原接线，本程序不会初始化 MIC 的三个 GPIO。

## 运行

```bash
cd s3cam/speaker_test
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

## 测试声音

程序以较低音量播放 3 轮：

```text
440 Hz -> 静音 -> 880 Hz -> 静音
```

左右 I2S 声道发送相同音频，因此 MAX98357A 当前模式无论取左声道还是右声道，都应能听到测试音。

## 通过标志

串口：

```text
SPEAKER TEST PASS: I2S data sent successfully
```

硬件最终通过标准：实际能清楚听到两种不同音高。

## 注意

GPIO3 和 GPIO46 属于 ESP32-S3 启动配置相关引脚。测试时保持 MAX98357A 一直连接；如果出现上电或复位偶发失败，请保留现场并记录启动日志，不要先靠拔线绕过问题。
