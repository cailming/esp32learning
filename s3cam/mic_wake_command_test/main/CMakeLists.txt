# -----------------------------------------------------------------------------
# 真实相机快门 WAV
# -----------------------------------------------------------------------------
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
        "main_asr.c"
        "asr_client.c"
        "camera_capture.c"
        "shutter_wav_player.c"
        "photo_http_server.c"
        "tts_client.c"
        "tts_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2s
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_http_client
        nvs_flash
        cjson
    PRIV_REQUIRES
        esp_timer
    EMBED_FILES
        "shutter.wav"
    EMBED_TXTFILES
        "index.html"
)
