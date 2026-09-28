idf_component_register(SRCS
                        "app/main.c"
                        "middleware/audio/audio_service.c"
                        "middleware/audio/rec_local_service.c"
                        "middleware/audio/media_player_service.c"
                        "middleware/ai/xiaozhi_service.c"
                        "middleware/ai/ai_light.c"
                        "middleware/ai/ai_speaker.c"
                        "middleware/ai/ai_camera.c"
                        "middleware/ai/ai_device.c"
                        "middleware/net/wifi_manager.c"
                        "middleware/net/net_time_service.c"
                        "middleware/periph/fs_service.c"
                        "middleware/periph/sdcard_service.c"
                        "middleware/periph/led_service.c"
                        "middleware/periph/button_service.c"
                        "middleware/camera/camera_service.c"
                        "middleware/camera/ov3660_fmt.c"
                        "middleware/camera/capture_service.c"
                        "middleware/camera/viewfinder.c"
                        "middleware/camera/color_hsv.c"
                        "middleware/camera/face_service.cpp"
                        "middleware/media/avi_mjpeg_muxer.c"
                        "middleware/media/avi_mjpeg_demuxer.c"
                        "middleware/media/file_mgr_service.c"
                        "middleware/media/record_video_service.c"
                        "3rd/ringbuf/ringbuf.c"
                        "ui/ui_manager/page_manager.c"
                        "ui/overlay_manager.c"
                        "ui/ui_common.c"
                        "ui/page_splash.c"
                        "ui/page_main.c"
                        "ui/tab_record.c"
                        "ui/overlay_ai.c"
                        "ui/ui_binfont.c"
                        "ui/tab_camera.c"
                        "ui/tab_video.c"
                        "ui/tab_files.c"
                        "ui/tab_sys_entry.c"
                        "ui/overlay_wallpaper.c"
                        "ui/page_wifi.c"
                        "ui/page_bluetooth.c"
                        "ui/page_rgb.c"
                        "ui/page_system.c"
                        "ui/overlay_wp_particles.c"
                        "ui/fonts/lv_font_digital_96.c"
                        "ui/fonts/lv_font_cn_20.c"
                        "ui/fonts/lv_font_cn_30.c"
                        "ui/fonts/lv_font_cn_45.c"
                      INCLUDE_DIRS "."
                                    "ui/ui_manager"
                                    "middleware"
                                    "middleware/audio"
                                    "middleware/ai"
                                    "middleware/net"
                                    "middleware/periph"
                                    "middleware/camera"
                      PRIV_REQUIRES esp_littlefs esp_wifi esp_event esp_netif nvs_flash esp_timer lwip
                                    esp_http_client cjson mbedtls zlib esp_video esp_cam_sensor esp32_s31_korvo_1
                                    esp_driver_jpeg
                                    ## ---- §60 异族模型（vendor 本地组件，SDCARD 模式）----
                                    hand_detect hand_gesture_recognition
                                    ## ★ §62 阉割 POSE：coco_pose 组件已删除，不再 REQUIRES。
                                    ## ---- S5 M2/M3：MP3 播放 + 小智对话（组件名以拉取后为准）----
                                    esp_player esp_audio_render esp_extractor esp_audio_codec
                                    media_lib_sal gmf_core gmf_audio gmf_io
                                    av_processor gmf_ai_audio esp-sr esp_xiaozhi
                                    fatfs sdmmc)

# =============================================================================
#  ★★ OV3660 **私有**寄存器表（2026-09-25，S2-0 运行时切格式用）
#
#  为什么需要：运行时切传感器格式只能走 esp_video 的私有 ioctl VIDIOC_S_SENSOR_FMT，
#  而它要求一份**带 regs 指针**的完整 `esp_cam_sensor_format_t` ——
#  因为 `ov3660_set_format()` 会把 format->regs 强转成 `ov3660_reginfo_t *` 去写寄存器。
#  那份寄存器表在 esp_cam_sensor 的 `sensors/ov3660/private_include/` 里，**不对外导出**；
#  BSP（bsp_camera.c 只导出 bsp_camera_start）与 esp_video 也都不暴露 sensor 句柄，
#  userspace 无法用 esp_cam_sensor_query_format() 枚举拿到它。
#  ⇒ 只能把该目录**加进本组件的私有 include 路径**，由 middleware/camera/ov3660_fmt.c 引用。
#
#  ★ 设计选择：**引用**（而不是复制 10.5 KB 寄存器值）。
#    复制会**静默过期**（供应商改时序我们不知道，画面悄悄变差）；
#    引用则在供应商改动时**编译期大声报错**，逼我们同步。详见 ov3660_fmt.h 文件头。
#
#  ★ 为什么用 target_include_directories 而不是 PRIV_INCLUDE_DIRS：
#    后者会触发 IDF 的 component_validation 警告
#    （"Private include directory ... belongs to component esp_cam_sensor"）。
#    那个警告在这里是**预期中的**：我们正是要引用它的私有表。
# =============================================================================
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp_cam_sensor/sensors/ov3660/private_include")

# =============================================================================
#  ★ mcp-c-sdk × GCC 15.2 兼容补丁（2026-09-26，build 实报）
#
#  现象：espressif__mcp-c-sdk 里 8+ 处 `strncpy(dst, src, sizeof(dst) - 1)` 模式
#        （截断+手动补 NUL 的合法写法）触发 GCC 15 新的 -Wstringop-truncation，
#        而 IDF 全局 -Werror 把它升级成 error ⇒ 编译失败。
#        该组件按老 GCC 发布；IDF 自带组件全部为 GCC 15.2 适配过，无此问题。
#  处置：对**整个组件**把该告警从 -Werror 降回 warning（同 IDF 自己的
#        -Wno-error=deprecated-declarations 惯例），不逐个改 managed 源码
#        （重新求解会被冲掉）。仅此一个组件、仅此一个告警，影响面最小。
#  依赖：main 在组件拓扑序的最后（依赖 esp_xiaozhi→mcp-c-sdk），
#        此时 mcp 的 target 已注册，可安全 target_compile_options。
# =============================================================================
idf_component_get_property(_mcp_lib espressif__mcp-c-sdk COMPONENT_LIB)
if(_mcp_lib)
    target_compile_options(${_mcp_lib} PRIVATE -Wno-error=stringop-truncation)
else()
    message(WARNING "mcp-c-sdk target not found; GCC15 strncpy patch skipped")
endif()



# =============================================================================
#  本地密钥注入（编译期宏）—— 和风天气 API Key / API Host
#
#  设计意图（红线）：
#    API Key **绝不写进任何被 git 跟踪的源码**。它只存在于环境变量里
#    （由 tools/idf_env.local.ps1 提供，该文件已被 .gitignore 忽略），
#    构建时在这里被读出来，转成本组件私有的 -D 编译宏。
#
#  未注入时：源码 net_time_service.h 的 #ifndef 占位宏生效，
#            行为与"没配 key"完全一致（打印 not configured 并跳过天气）。
#
#  ⚠ 重要（踩坑记录 F18）：
#    CMake 的 $ENV{} 只在 **configure 阶段**求值一次。若只改环境变量而直接
#    增量构建，build.ninja 里会一直沿用 **旧值**，表现为"配置明明改了、
#    固件里还是旧域名字符串"。
#    因此下面用 idf_build_set_property 把这两个值登记为 build property，
#    并在 tools/build.ps1 / flash.ps1 里改成"默认先 reconfigure"，
#    双保险避免静默使用旧值。
# =============================================================================

set(_qwx_key  "$ENV{QWEATHER_KEY}")
set(_qwx_host "$ENV{QWEATHER_API_HOST}")
set(_qwx_loc  "$ENV{QWEATHER_LOC_ID}")

if(NOT _qwx_key STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "QWEATHER_KEY=\"${_qwx_key}\"")
    string(LENGTH "${_qwx_key}" _qwx_key_len)
    message(STATUS "QWEATHER_KEY: injected from environment (len=${_qwx_key_len}, value hidden)")
else()
    message(STATUS "QWEATHER_KEY: not set, weather disabled (placeholder kept)")
endif()

if(NOT _qwx_host STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "QWEATHER_API_HOST=\"${_qwx_host}\"")
    message(STATUS "QWEATHER_API_HOST: injected -> ${_qwx_host}")
else()
    message(STATUS "QWEATHER_API_HOST: not set, using source default")
endif()

if(NOT _qwx_loc STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "QWEATHER_LOC_ID=\"${_qwx_loc}\"")
    message(STATUS "QWEATHER_LOC_ID: injected -> ${_qwx_loc}")
endif()
