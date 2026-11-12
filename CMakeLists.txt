# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(COMPONENTS main) # "Trim" the build. Include the minimal set of components; main and anything it depends on.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ---- 固件版本：version.txt（人决定）+ 构建 metadata（commit）----
# 方案见 docs/项目规划/固件版本管理.md。必须写在 project() 之前：
# IDF 的 project.cmake 用 `if(NOT DEFINED PROJECT_VER)` 判断要不要自己去找 version.txt。
#   * 默认        → PROJECT_VER = <version.txt>+g<短 sha>[.dirty]
#   * 发布包       → 设环境变量 RELEASE_BUILD=1 → PROJECT_VER = <version.txt>（不带 metadata）
#   * 显式 -D      → 以传入值为准（CI 想钉死版本号时用）
# 换 commit 后靠 .git/HEAD 这个 CMAKE_CONFIGURE_DEPENDS 触发重配，版本不会滞留。
if(NOT DEFINED PROJECT_VER)
    set(_ver_file "${CMAKE_CURRENT_SOURCE_DIR}/version.txt")
    if(EXISTS "${_ver_file}")
        file(STRINGS "${_ver_file}" _ver LIMIT_COUNT 1)
        string(STRIP "${_ver}" _ver)
        if(DEFINED ENV{RELEASE_BUILD})
            set(PROJECT_VER "${_ver}")
            message(STATUS "PROJECT_VER = ${PROJECT_VER}  (release build, version.txt only)")
        else()
            find_package(Git QUIET)
            set(_sha "")
            if(GIT_FOUND)
                execute_process(
                    COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE _sha
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
            endif()
            if(_sha)
                execute_process(
                    COMMAND "${GIT_EXECUTABLE}" status --porcelain
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE _dirty
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
                if(_dirty)
                    set(PROJECT_VER "${_ver}+g${_sha}.dirty")
                else()
                    set(PROJECT_VER "${_ver}+g${_sha}")
                endif()
                if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
                    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                                 "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
                endif()
            else()
                set(PROJECT_VER "${_ver}")
            endif()
            message(STATUS "PROJECT_VER = ${PROJECT_VER}  (version.txt + git commit)")
        endif()
    endif()
endif()

project(display_audio_photo)

# gcc15 在总 -Werror=all 下把 bluedroid 自带 bt_types.h 的 strncpy truncation 当错误。
# 该警告来自 IDF 自带代码（无法改源码），APPEND 到末尾以覆盖前面的 -Werror=all。
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
# storage 镜像仍会生成到 build/storage.bin，但不带 FLASH_IN_PROJECT：
# 日常 `idf.py flash` / `app-flash` 只烧 app（几秒），不重写素材镜像。
# 仅在素材(xwkkk.jpg/wav/字体 bin)变更、换片/擦除后首次烧录时，才单独写一次：
#   esptool --chip esp32s31 -p COM9 write-flash 0xA20000 build/storage.bin
# ★★ 2026-09-22 二次订正：参数顺序是 【地址在前、文件在后】！
#   esptool 的用法是 `write-flash [OPTIONS] <address> <filename>...`。
#   写成 `write-flash build/storage.bin 0xA20000` 会报
#   "Invalid value for '<address> <filename>': Address \"build/storage.bin\" must be a number."
# ★ 2026-09-22 订正：此处原写 0x620000 已过时。当前 partitions.csv 为
#   ota_0@0x20000(5M) / ota_1@0x520000(5M) / storage@0xA20000(0x5E0000≈5.875M)。
#   烧错地址会覆盖 ota_0 分区。烧前务必核对 partitions.csv。
#   （build/storage.bin 的大小应正好等于 6,160,384 B = 0x5E0000。）
littlefs_create_partition_image(storage littlefs_content)
