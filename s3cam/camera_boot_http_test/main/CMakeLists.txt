idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp32-camera
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        nvs_flash
        esp_driver_gpio
)
