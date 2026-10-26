#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 一次正式拍照后的结果摘要。
 *
 * JPEG 原始数据由 camera_capture 模块保留为“最新照片”；
 * 这里返回宽高、JPEG 大小、成功拍照序号和内容指纹，
 * 供业务日志/HTTP/上传逻辑使用。
 *
 * fingerprint 使用轻量 FNV-1a 对整张 JPEG 计算，仅用于排查：
 * - Camera 实际抓到的是哪一帧；
 * - 内存中“最新照片”是否已经替换；
 * - HTTP 最终发送出去的是不是同一张 JPEG。
 */
typedef struct {
    uint32_t sequence;
    uint16_t width;
    uint16_t height;
    size_t jpeg_size;
    uint32_t fingerprint;
} camera_photo_info_t;

/**
 * @brief 初始化板载 OV3660 Camera。
 */
esp_err_t camera_capture_init(void);

/**
 * @brief 真正拍摄一张 JPEG，并把 JPEG 复制保留为“最新照片”。
 *
 * 当前使用 fb_count=1 + CAMERA_GRAB_WHEN_EMPTY。
 * 为避免驱动队列中的旧帧导致“拍一次、下一次才看到上一张”的现象，
 * 本函数会先取出并丢弃一帧，再取第二帧作为本次正式照片。
 *
 * 成功后才更新最新照片和 sequence；失败时保留上一张有效照片不变。
 */
esp_err_t camera_capture_one(camera_photo_info_t *info);

/**
 * @brief 获取当前最新照片的只读指针。
 *
 * 返回的指针由模块持有，下一次成功 camera_capture_one() 后可能失效。
 * 适合只读取元数据或非常短暂的同步访问；HTTP 长时间发送不要直接使用此接口。
 */
const uint8_t *camera_capture_latest(size_t *size,
                                     uint16_t *width,
                                     uint16_t *height,
                                     uint32_t *sequence);

/**
 * @brief 为调用方复制一份“最新照片”。
 *
 * 这是给 HTTP/上传逻辑使用的安全接口：复制期间持有内部 mutex，返回以后
 * 即使语音再次拍照并替换内部最新 JPEG，调用方手里的副本仍然有效。
 *
 * @param jpeg_copy 成功时返回新分配的 JPEG 缓冲区；调用方使用 free() 释放。
 * @param info      可选，返回照片序号/宽高/大小/内容指纹。
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 表示还没有拍过照片。
 */
esp_err_t camera_capture_copy_latest(uint8_t **jpeg_copy,
                                     camera_photo_info_t *info);

#ifdef __cplusplus
}
#endif
