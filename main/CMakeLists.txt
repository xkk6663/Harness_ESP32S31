idf_component_register(SRCS
                        "app/main.c"
                        "middleware/audio/audio_service.c"
                        "middleware/audio/udp_voice.c"
                        "middleware/audio/record_service.c"
                        "middleware/net/wifi_manager.c"
                        "middleware/net/session_manager.c"
                        "middleware/net/link_manager.c"
                        "middleware/net/net_time_service.c"
                        "middleware/periph/fs_service.c"
                        "middleware/periph/led_service.c"
                        "middleware/periph/button_service.c"
                        "middleware/camera/camera_service.c"
                        "middleware/camera/color_hsv.c"
                        "middleware/camera/face_service.cpp"
                        "3rd/ringbuf/ringbuf.c"
                        "ui/ui_manager/page_manager.c"
                        "ui/overlay_manager.c"
                        "ui/ui_common.c"
                        "ui/page_splash.c"
                        "ui/page_main.c"
                        "ui/tab_call.c"
                        "ui/tab_record.c"
                        "ui/tab_camera.c"
                        "ui/tab_sys_entry.c"
                        "ui/overlay_wallpaper.c"
                        "ui/page_wav_player.c"
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
                                    "middleware/net"
                                    "middleware/periph"
                                    "middleware/camera"
                      PRIV_REQUIRES esp_littlefs esp_wifi esp_event esp_netif nvs_flash esp_timer lwip
                                    esp_http_client cjson mbedtls zlib esp_video esp_cam_sensor esp32_s31_korvo_1)

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
