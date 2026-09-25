# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(COMPONENTS main) # "Trim" the build. Include the minimal set of components; main and anything it depends on.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(display_audio_photo)
# storage 镜像仍会生成到 build/storage.bin，但不带 FLASH_IN_PROJECT：
# 日常 `idf.py flash` / `app-flash` 只烧 app（几秒），不重写 4MB 素材镜像。
# 仅在素材(xwkkk.jpg/wav/icons/models)变更、换片/擦除后首次烧录时，才单独写一次：
#
#   idf.py storage-flash            （推荐：自动按分区表偏移/大小生成并烧录）
#   esptool --chip esp32s31 -p COM9 write-flash build/storage.bin 0xC20000
#
# ★★ 偏移必须是 0xC20000，**照抄 partitions.csv 里 storage 这一行**（§28 改造后）：
#      storage,  data, littlefs, 0xC20000, 0x3E0000
#   ★ 历史坑（别重蹈）：本注释曾长期写着 0x620000，而那时 0x620000 落在
#     **ota_1 分区内部** ⇒ 按旧注释烧会**覆盖掉另一份固件**，且当场没有任何报错，
#     只在下次 OTA 回滚时才炸。
#   ★★ §28（2026-09-25）再改一次：OTA 槽扩到 0x600000 ⇒ storage 偏移 0xA20000 → **0xC20000**，
#      大小 0x5E0000 → **0x3E0000**。而新的 **ota_1 恰好就是 0x620000** ——
#      谁要是按"旧笔记里的 0x620000"烧 storage.bin，这次就会**直接覆盖 ota_1 的固件**。
#      改分区表后务必回来同步这一行，并且**永远以 partitions.csv 为唯一真源**。
#   ★ 模型文件就在 littlefs_content/models/ 下，加完文件必须重新 build 才有新 storage.bin。
littlefs_create_partition_image(storage littlefs_content)
