# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(COMPONENTS main) # "Trim" the build. Include the minimal set of components; main and anything it depends on.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(display_audio_photo)
# storage 镜像仍会生成到 build/storage.bin，但不带 FLASH_IN_PROJECT：
# 日常 `idf.py flash` / `app-flash` 只烧 app（几秒），不重写 10MB 素材镜像。
# 仅在素材(xwkkk.jpg/wav)变更、换片/擦除后首次烧录时，才单独写一次：
#   esptool --chip esp32s31 -p COM9 write-flash build/storage.bin 0x620000
littlefs_create_partition_image(storage littlefs_content)
