# mic_test

I2S 数字麦克风独立测试。

## 当前接线

```text
MIC SCK -> GPIO42
MIC WS  -> GPIO41
MIC SD  -> GPIO1
MIC VDD -> 3V3
MIC GND -> 公共 GND
MIC L/R -> 公共 GND
```

MAX98357A 和喇叭继续保持原接线，本测试不会初始化它们的 GPIO。

## 运行

```bash
cd s3cam/mic_test
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

## 怎么测

1. 程序启动后先保持环境安静约 2 秒；
2. 等串口打印 `Quiet baseline = ...`；
3. 拍一下手，或在麦克风附近正常说话；
4. 观察 `L_span`、`R_span` 和 `level: ####` 是否明显增大。

## 通过标志

```text
MIC TEST PASS: detected clear audio level change
```

程序同时观察 LEFT / RIGHT 两个声道，因此即使当前 L/R 接地对应的有效声道与预想不同，也可以直接看到哪个声道真正有数据。
