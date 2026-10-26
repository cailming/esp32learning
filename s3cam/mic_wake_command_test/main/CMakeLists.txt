# -----------------------------------------------------------------------------
# 真实相机快门 WAV
# -----------------------------------------------------------------------------
#
# 第一次配置工程时，从固定 commit 下载 Mozilla / Firefox DevTools 使用的
# shutter.wav，然后通过 EMBED_FILES 原样打进 ESP32 固件。
#
# 这里故意把 WAV 放在当前组件根目录，而不是 sounds/ 子目录：
# 这样 ESP-IDF 生成的链接符号固定为：
#
#   _binary_shutter_wav_start
#   _binary_shutter_wav_end
#
# 可以避免不同 ESP-IDF 版本对带路径 EMBED_FILES 的符号命名差异。
#
set(SHUTTER_SOUND_FILE "${CMAKE_CURRENT_LIST_DIR}/shutter.wav")
set(SHUTTER_SOUND_URL
    "https://raw.githubusercontent.com/mozilla/gecko-dev/5836a062726f715fda621338a17b51aff30d0a8c/devtools/client/themes/audio/shutter.wav")

if(NOT EXISTS "${SHUTTER_SOUND_FILE}")
    message(STATUS "Downloading real camera shutter WAV -> ${SHUTTER_SOUND_FILE}")

    file(DOWNLOAD
        "${SHUTTER_SOUND_URL}"
        "${SHUTTER_SOUND_FILE}"
        STATUS SHUTTER_DOWNLOAD_STATUS
        TLS_VERIFY ON
        SHOW_PROGRESS
    )

    list(GET SHUTTER_DOWNLOAD_STATUS 0 SHUTTER_DOWNLOAD_CODE)
    list(GET SHUTTER_DOWNLOAD_STATUS 1 SHUTTER_DOWNLOAD_MESSAGE)

    if(NOT SHUTTER_DOWNLOAD_CODE EQUAL 0)
        file(REMOVE "${SHUTTER_SOUND_FILE}")
        message(FATAL_ERROR
            "Failed to download shutter.wav: ${SHUTTER_DOWNLOAD_MESSAGE}\n"
            "Please check network access to raw.githubusercontent.com and run idf.py build again.")
    endif()
endif()

idf_component_register(
    SRCS
        "main.c"
        "shutter_wav_player.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2s
    EMBED_FILES
        "shutter.wav"
)
