# ESP-IDF 工程顶层 CMakeLists.txt。
# 下面这些基础语句的顺序是 ESP-IDF CMake 构建系统要求的，通常不要随意调整。
cmake_minimum_required(VERSION 3.22)

# 引入共享的 ble_uart 组件。
#
# 这里必须在 project() 之前把组件目录加入 EXTRA_COMPONENT_DIRS，
# 这样 main 组件中的 `REQUIRES ble_uart` 才能找到 ble_uart。
#
# 路径是相对于当前示例目录计算的：
#   bluetooth/ble_uart_service
#       ../common/ble_uart
#   -> bluetooth/common/ble_uart
#
# 注意：当前仓库 main 分支中没有提交这个目录，因此按当前仓库内容直接构建时
# 会缺少 ble_uart 共享组件；需要把对应组件源码补齐后才能正常编译。
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/ble_uart")

# 把 IDF_PATH 转成 CMake 能稳定处理的路径格式。
cmake_path(CONVERT "$ENV{IDF_PATH}" TO_CMAKE_PATH_LIST ESP_IDF_PATH)

# 蓝牙安全功能会使用 mbedTLS，这里加载 ESP-IDF 为 Bluetooth 准备的 mbedTLS 默认配置。
list(APPEND sdkconfig_defaults "${ESP_IDF_PATH}/components/mbedtls/config/mbedtls_preset_bt.conf")

# 加载 ESP-IDF 工程构建系统。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 使用 ESP-IDF 的最小构建模式，只构建本项目实际依赖到的组件。
idf_build_set_property(MINIMAL_BUILD ON)

# 定义工程名称。
project(ble_uart_service)
