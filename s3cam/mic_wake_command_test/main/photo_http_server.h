#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动用于查看语音拍照结果的 Wi-Fi SoftAP + HTTP Server。
 *
 * SoftAP：
 *   SSID     = S3CAM-Photo
 *   Password = 12345678
 *
 * HTTP：
 *   GET /          -> 查看最新照片网页
 *   GET /status    -> 最新照片元数据 JSON
 *   GET /photo.jpg -> 最新 JPEG
 */
esp_err_t photo_http_server_start(void);

#ifdef __cplusplus
}
#endif
