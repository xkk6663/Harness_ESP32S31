/* tab_video.h - VIDEO tab：SD 卡媒体（拍照 / 录像） */
#pragma once
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 建 VIDEO 页（方案 §9 阶段 S2-0 的第 5 个 tab）
 *
 * 与 tab_camera 的分工（方案 §2.5.1）：
 *   - CAM  tab = RGB565 240x240 预览（含跑模型 / 人脸检测）
 *   - VIDEO tab = JPEG 1280x720 媒体采集（拍照 / 录像）
 * 两者**共用同一个传感器**，同一时刻只能有一个档位生效 ⇒ 靠
 * camera_service_owner() 做互斥：一边在跑时，另一边的按钮必须置灰并给出原因。
 */
void tab_video_build(lv_obj_t *screen, lv_group_t *group);
void tab_video_destroy(void);

/**
 * @brief 通知本页"当前是不是活跃 tab"（page_main 在 tab 切换时调用）
 *
 * ★ 取景流与 tab 绑定：只有活跃时才占着相机（720p JPEG，owner=PREVIEW）。
 *   - active=true  → 起取景器（viewfinder_enable + 起流；若相机已被
 *                    拍照/录像占用则**只**挂观察者，不抢流 ⇒ 录像中切进来
 *                    照样能看到画面）；
 *   - active=false → 停取景器；只有相机还归 PREVIEW 所有时才停流
 *                    （绝不能掐掉正在录的 MEDIA 流）。
 * ★ 幂等：传相同值直接返回。
 * ★ 初值必须是 false：tabview 起始页是 CALL，VIDEO 从未活跃过。
 */
void tab_video_set_active(bool active);

#ifdef __cplusplus
}
#endif
