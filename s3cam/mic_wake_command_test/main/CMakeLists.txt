# -----------------------------------------------------------------------------
# 真实相机快门 WAV
# -----------------------------------------------------------------------------
#
# GitHub Connector 当前只适合直接提交文本文件，因此这里把 WAV 作为构建资源处理：
# 第一次配置工程时，从固定 commit 下载 Mozilla/Firefox DevTools 使用的 shutter.wav，
# 然后通过 EMBED_FILES 原样打进 ESP32 固件。
#
# 固定 commit 的目的是保证以后重新编译时拿到完全相同的声音文件。
# 该资源来自 Mozilla gecko-dev，源码目录采用 MPL-2.0。
#
set(SHUTTER_SOUND_DIR "${CMAKE_CURRENT_LIST_DIR}/sounds")
set(SHUTTER_SOUND_FILE "${SHUTTER_SOUND_DIR}/shutter.wav")
set(SHUTTER_SOUND_URL
    "https://raw.githubusercontent.com/mozilla/gecko-dev/5836a062726f715fda621338a17b51aff30d0a8c/devtools/client/themes/audio/shutter.wav")

if(NOT EXISTS "${SHUTTER_SOUND_FILE}")
    file(MAKE_DIRECTORY "${SHUTTER_SOUND_DIR}")

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
        "sounds/shutter.wav"
)
