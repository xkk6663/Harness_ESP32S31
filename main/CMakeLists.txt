# =============================================================================
#  源文件登记（SRCS）
#
#  ⚠ 新增 .c 必须在此登记，否则不参与编译（规范 03 / 10）。
#  路径分两层：第一层 app / middleware / ui / 3rd，第二层为领域子目录。
#    middleware/  audio | bluetooth | network | system
#    ui/          pages | tabs | overlays | common | ui_manager | assets
# =============================================================================
idf_component_register(SRCS
    # ---- app 层：入口编排 ----
    "app/main.c"

    # ---- middleware / audio：音频与音频会话 ----
    "middleware/audio/audio_service.c"
    "middleware/audio/record_service.c"
    "middleware/audio/udp_voice.c"
    "middleware/audio/session_manager.c"

    # ---- middleware / bluetooth：经典蓝牙链路 ----
    "middleware/bluetooth/bt_audio_service.c"
    "middleware/bluetooth/bt_push_service.c"
    "middleware/bluetooth/link_manager.c"

    # ---- middleware / network：WiFi 与网络服务 ----
    "middleware/network/wifi_manager.c"
    "middleware/network/net_time_service.c"

    # ---- middleware / system：板级外设与存储 ----
    "middleware/system/button_service.c"
    "middleware/system/led_service.c"
    "middleware/system/fs_service.c"
    "middleware/system/sdcard_service.c"

    # ---- 3rd：第三方组件 ----
    "3rd/ringbuf/ringbuf.c"

    # ---- ui / ui_manager：页面调度器 ----
    "ui/ui_manager/page_manager.c"

    # ---- ui / common：公共部件与主题 ----
    "ui/common/ui_common.c"

    # ---- ui / pages：全屏页面 ----
    "ui/pages/page_main.c"
    "ui/pages/page_splash.c"
    "ui/pages/page_wifi.c"
    "ui/pages/page_bluetooth.c"
    "ui/pages/page_system.c"
    "ui/pages/page_rgb.c"
    "ui/pages/page_wav_player.c"

    # ---- ui / tabs：主框架内的标签页 ----
    "ui/tabs/tab_call.c"
    "ui/tabs/tab_record.c"
    "ui/tabs/tab_sys_entry.c"
    "ui/tabs/tab_music.c"
    "ui/tabs/tab_btpush.c"
    "ui/tabs/tab_video.c"

    # ---- ui / overlays：浮层（壁纸 / 粒子） ----
    "ui/overlays/overlay_manager.c"
    "ui/overlays/overlay_wallpaper.c"
    "ui/overlays/overlay_wp_particles.c"

    # ---- ui / assets：字体与图标资源 ----
    "ui/assets/fonts/lv_font_digital_96.c"
    "ui/assets/fonts/lv_font_cn_20.c"
    "ui/assets/fonts/lv_font_cn_30.c"
    "ui/assets/fonts/lv_font_cn_45.c"
    "ui/assets/icons/image_wifi.c"
    "ui/assets/icons/image_sun.c"
    "ui/assets/icons/image_cloud.c"
    "ui/assets/icons/image_temp.c"
    "ui/assets/icons/image_humidity.c"
    "ui/assets/icons/image_wind.c"
    "ui/assets/icons/image_sunrise.c"
    "ui/assets/icons/image_rain.c"
  INCLUDE_DIRS "."
                "ui/ui_manager"
  PRIV_REQUIRES esp_littlefs esp_wifi esp_event esp_netif nvs_flash esp_timer lwip
                                    esp_http_client cjson mbedtls zlib fatfs sdmmc driver
                                    esp_player esp_audio_render esp_extractor esp_audio_codec
                                    gmf_core gmf_audio gmf_io esp_codec_dev media_lib_sal
                                    esp_bt_audio
                                    esp_video_render esp_video_codec esp_new_jpeg gmf_video lvgl__lvgl)

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
