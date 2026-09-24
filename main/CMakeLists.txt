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
    # bt_sink_service = A2DP Sink 接收（bt2codec 管线 + 元数据/封面）
    "middleware/bluetooth/bt_sink_service.c"
    "middleware/bluetooth/link_manager.c"

    # ---- middleware / network：WiFi 与网络服务 ----
    "middleware/network/wifi_manager.c"
    "middleware/network/net_time_service.c"
    # pcnow_service = PC 音乐副屏数据服务（UDP 50004 + HTTP 拉封面/歌词）
    "middleware/network/pcnow_service.c"

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
    # ui_binfont = LittleFS/PSRAM 二进制字体加载（btsink 中文预渲染字体）
    "ui/common/ui_binfont.c"

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
    # tab_audio = 原 tab_music + tab_btpush 合并（本地播放 / 蓝牙推流共用一页）
    "ui/tabs/tab_audio.c"
    # tab_btsink = A2DP Sink 接收页（封面 + 元数据 + AVRCP 控制）
    "ui/tabs/tab_btsink.c"
    # tab_pcnow = PC 音乐副屏页（圆形封面旋转 + 进度 + v1.1 歌词三行窗）
    "ui/tabs/tab_pcnow.c"
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
