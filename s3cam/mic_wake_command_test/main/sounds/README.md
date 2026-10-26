# shutter.wav

本目录用于放置拍照快门音资源。

当前工程使用 Mozilla / Firefox DevTools 的 `shutter.wav` 作为真实相机快门音素材。

固定来源：

```text
https://raw.githubusercontent.com/mozilla/gecko-dev/5836a062726f715fda621338a17b51aff30d0a8c/devtools/client/themes/audio/shutter.wav
```

该文件在第一次 CMake 配置时自动下载到：

```text
main/sounds/shutter.wav
```

然后通过 ESP-IDF：

```cmake
EMBED_FILES "sounds/shutter.wav"
```

原样嵌入固件。

当前 WAV 为标准 RIFF/WAVE PCM。播放模块会解析 `fmt ` / `data` chunk，并把原始单声道 PCM 重采样为 MAX98357A 当前使用的 16kHz / 16-bit / stereo I2S 数据。

Mozilla `gecko-dev` 源代码采用 MPL-2.0；本工程只引用固定版本的快门音资源用于测试。
