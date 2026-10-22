# 注册本示例的 main 组件。
#
# SRCS：本组件只有一个应用源码 main.c。
# INCLUDE_DIRS：当前目录作为头文件搜索路径。
# REQUIRES：声明依赖 ble_uart 共享组件和 ESP-IDF 的 nvs_flash 组件。
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES ble_uart nvs_flash)
