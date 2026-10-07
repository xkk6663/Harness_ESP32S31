/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wallpaper_service.h
 * @brief SD-card wallpaper pool: 10-min random rotation + NVS last-image
 *        persistence + double-buffered JPEG decode (§75).
 *
 * 双缓冲协议（§75 设计，三方职责）：
 *   - service 任务（本文件）：只往"空闲 buffer"解码，解完翻 s_cur、gen++，
 *     然后等 commit（UI 动画结束归还旧 buffer，或 5s 超时兜底）才进下一轮；
 *   - LVGL 线程（overlay_wallpaper / overlay_wp_particles）：只读当前 buffer，
 *     通过 gen 代际号轮询发现新图（与 §74 打卡事件同款风格，零跨线程 LVGL 调用）；
 *   - buffer[0] = fs_service 的 file_buffer 前 768KB（借用，不新增分配；
 *     main 的 file_buffer 为 H*V*sizeof(lv_color_t)=1.15MB ≥ 768KB），
 *     buffer[1] = 本服务分配的 work buffer（全屏 RGB565 = 800*480*2 = 768KB）。
 *
 * 红线遵守：本文件属 middleware，不 include lvgl.h；
 * 解码输出 **RGB565**（V2.1 拍板：硬件不支持全屏 RGB888 落屏），
 * 内部走 RGB888 中转 + 软件 565 转换（与 main overlay_wp_particles decode_base
 * 同款已实机验证路线，无需 R/B swap）。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "middleware/system/sdcard_service.h"   /* SDCARD_MNT_PATH */

#ifdef __cplusplus
extern "C" {
#endif

/** 壁纸池目录（SD 卡；UI 端 FILES 页据此判定"SET WALLPAPER"按钮可见性） */
#define WALLPAPER_POOL_DIR  SDCARD_MNT_PATH"/wallpaper"

/** buffer 下标：0 = fs_service file_buffer（借用），1 = work buffer。 */
#define WP_BUF_0    0
#define WP_BUF_1    1

/**
 * @brief 初始化壁纸服务（app_main 串行路径调用，含同步解码）。
 *
 * 顺序要求：必须在 fs_service_init()（file_buffer 就位）之后、
 * Page_Splash 装载之前 —— 开机要同步解码"上次壁纸"（几百 ms）。
 *
 * 恢复优先级：NVS wp_last（且文件在 SD 池中）→ SD 池随机 →
 * littlefs 兜底图（FS_MNT_PATH"/xwkkk.jpg"）→ 深蓝纯色（绝不黑屏崩机）。
 * SD 未挂载 / 池为空不致命：静态兜底图照常显示，轮换任务每 10min 重扫一次。
 */
void wallpaper_service_init(void);

/**
 * @brief 取双缓冲之一的首地址（始终返回全屏尺寸 RGB565 区，已初始化非垃圾）。
 * @param idx WP_BUF_0 / WP_BUF_1（越界按 0 处理）。
 * @return 缓冲指针；buffer[1] 分配失败时 idx==1 返回 NULL。
 */
uint8_t *wallpaper_service_get_buffer(int idx);

/** 当前展示中的 buffer 下标（0/1）。新图解码完成后即翻转为新图下标。 */
int wallpaper_service_get_current(void);

/**
 * 壁纸代际号：每次成功轮换（解码成功 + 翻转）后 +1。
 * UI 在自己的 tick 里轮询此值发现新图（初值 0 = 开机首图，无需动画）。
 */
uint32_t wallpaper_service_get_gen(void);

/**
 * @brief UI 归还"旧图 buffer"（crossfade 动画结束后调用一次）。
 * service 任务收到后才允许开始下一轮解码（防止覆写正在淡出的旧图）；
 * 5s 超时兜底：无 UI 在场时自动继续，绝不卡死轮换。
 */
void wallpaper_service_commit(void);

/**
 * @brief §75 补充 1：手动切换壁纸（FILES 页"SET WALLPAPER"按钮入口）。
 *
 * 异步语义：请求入队后由 wp_svc 任务校验（名字必须在池内，防路径注入）
 * 并解码到空闲 buffer，成功后翻转 + gen++ —— UI 经现有代际号轮询自动
 * crossfade，NVS 随之 write-through。解码失败（坏图/非 800x480）保持
 * 当前图，gen 不变。
 *
 * @param name 池内文件名（纯文件名，不带路径——路径注入在服务端被拒）。
 * @return ESP_OK = 已入队；ESP_ERR_INVALID_STATE = 服务未就绪/降级态；
 *         ESP_FAIL = 队列满（服务正忙）。
 */
esp_err_t wallpaper_service_request_set(const char *name);

/** 当前壁纸文件名（池内相对名；兜底图为 "xwkkk.jpg"；纯色降级态为空串）。 */
void wallpaper_service_get_current_name(char *out, size_t len);

/**
 * @brief §75 补充 2：校验文件是合法的 800x480 JPEG（PC 上传 / 网页上传闸）。
 * 只读文件头部（64KB 内）parse_header，不做整图解码。
 * @return ESP_OK = 合格；ESP_ERR_INVALID_SIZE = 尺寸不是 800x480；
 *         ESP_ERR_INVALID_STATE = 不是合法 JPEG；ESP_ERR_NOT_FOUND/NO_MEM = 读取失败。
 */
esp_err_t wallpaper_service_validate_file(const char *path);

/**
 * @brief §76：PC 上传壁纸（**设备拉**模型）——注册 link_manager JSON 分发槽。
 *
 * 信令（UDP 50003，与 OTA 同构；PC 端 pc_host v0.4）：
 *   PC → 设备：{"type":"WP_OFFER","name":"a.jpg","size":123456,"sha256":"…","url":"http://ip:50005/wallpaper/a.jpg"}
 *              （sha256 为保留字段，设备端忽略——TCP 保证传输完整性，
 *               落池后有 validate_file 尺寸/格式闸兜底）
 *   设备 → PC：{"type":"WP_RESULT","ok":1,"name":"a.jpg","err":""}
 *
 * 设备侧行为：收到 WP_OFFER → wp_fetch 任务（**内部 RAM 栈**，F35：下载写 SD 卡）
 * HTTP GET 流式落 SD（先写 .wp_tmp 临时名）→ validate_file → rename 落池 →
 * request_set()（经现有代际号轮询，UI 自动 crossfade + NVS 记忆）→ WP_RESULT。
 * 任何一步失败：删临时文件、当前壁纸不动、WP_RESULT ok:0。
 *
 * 调用时机：link_manager_init() 之后（本函数只注册回调 + 建任务，非阻塞）。
 */
void wallpaper_service_fetch_init(void);

#ifdef __cplusplus
}
#endif
