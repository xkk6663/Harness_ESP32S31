/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bt_sink_service.h
 * @brief A2DP Sink 接收服务：把手机/电脑推来的音频解出来放板载扬声器，
 *        并把 AVRCP 元数据拆成「真曲名 / 真歌手 / 歌词行」给 UI。
 *
 * 数据流：io_bt(reader) -> aud_dec(SBC/AAC) -> aud_asrc -> io_codec_dev(ES8389)
 *
 * 设计原则（与 bt_push_service 的「方案 B 延迟常驻」对齐）：
 *  - P1 时机前置：pool/pipeline/task 在 bt_sink_service_early_init() 建，
 *    抢在 bsp_display_start() 之前（内部 RAM 最空、PSRAM 未被切碎）；
 *  - P2 显式外置：GMF task 栈进 PSRAM；asrc 权重显式 PSRAM；
 *  - P3 建一次不毁：pipeline 首次 ALLOCATED 建，STOPPED/RELEASED 只停不毁；
 *  - P4 线程纪律：所有回调都跑在 BT/GMF 线程，**绝不碰 LVGL** ——
 *    只更新 volatile / 投队列，UI 侧用 500ms lv_timer 抽取。
 *
 * ★ 2026-09-22：**专辑封面链路整条删除**。
 *   原因见 docs/项目规划/改动记录.md §28.28：AVRCP Cover Art 在安卓侧默认关闭
 *   （默认 AVRCP 1.5 + OEM overlay 常为 false + App 未必交图），实测对端根本不推，
 *   设备端代码再完整也收不到。删掉后一次省下 2×240×240×3 ≈ 337KB PSRAM、
 *   一个解码任务和一个队列。若将来真要封面，git 可回滚，或改走 WiFi 查图兜底。
 */

#ifndef BT_SINK_SERVICE_H
#define BT_SINK_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 元数据字段长度（与参考仓库的量级对齐，够放正常曲名/专辑名） */
#define BT_SINK_TITLE_LEN   128
#define BT_SINK_ARTIST_LEN  128
#define BT_SINK_ALBUM_LEN   128
#define BT_SINK_GENRE_LEN   64

/** 单句歌词长度。实测对端最长一句 40 字节（日文，见 §28.29 日志），128 足够宽裕。 */
#define BT_SINK_LYRIC_LEN   128

/** 播放状态（由 esp_bt_audio_playback_status_t 归一化而来） */
typedef enum {
    BT_SINK_STATE_IDLE = 0,   /* 还没收到任何状态 */
    BT_SINK_STATE_STOPPED,
    BT_SINK_STATE_PLAYING,
    BT_SINK_STATE_PAUSED,
} bt_sink_state_t;

/** 元数据快照（UTF-8，已保证 NUL 结尾）。
 *  ★ title / artist 是**锁住的真值**，不是对端最后一条 TITLE/ARTIST ——
 *    对端开启"车载蓝牙歌词"后会把歌词行覆盖写进 TITLE，见下面的 get_lyric()。 */
typedef struct {
    char title[BT_SINK_TITLE_LEN];
    char artist[BT_SINK_ARTIST_LEN];
    char album[BT_SINK_ALBUM_LEN];
    char genre[BT_SINK_GENRE_LEN];
} bt_sink_meta_t;

/** 最新一句歌词快照（UTF-8，已 NUL 结尾）。 */
typedef struct {
    char     text[BT_SINK_LYRIC_LEN];
    uint32_t seq;      /* 递增序号；0 = 尚未收到过歌词。UI 只在 seq 变化时推进动画 */
} bt_sink_lyric_t;

/**
 * @brief 开机早期预建 GMF pool / pipeline。
 * @note  必须在 bsp_display_start() 之前调用；失败只打日志不 abort（蓝牙非必需功能）。
 */
void bt_sink_service_early_init(void);

/**
 * @brief 由 bt_audio_service 在 STREAM_STATE_CHG 里转发（跑在 BT/GMF 线程）。
 * @param stream esp_bt_audio_stream_handle_t（void* 透传）
 * @param state  ESP_BT_AUDIO_STREAM_STATE_* 四态
 */
void bt_sink_service_on_stream_state(void *stream, int state);

/**
 * @brief 由 bt_audio_service 在 PLAYBACK_STATUS_CHG 里转发（跑在 BT 线程）。
 * @param evt const esp_bt_audio_event_playback_st_t*
 */
void bt_sink_service_on_playback_status(const void *evt);

/**
 * @brief 由 bt_audio_service 在 PLAYBACK_METADATA 里转发（跑在 BT 线程）。
 * @param evt const esp_bt_audio_event_playback_metadata_t*
 */
void bt_sink_service_on_metadata(const void *evt);

/** 蓝牙关闭 / 角色切换时调用：停止并复位接收管线（不销毁，遵循"建一次不毁"） */
void bt_sink_service_request_stop(void);

/**
 * @brief 让出 codec 给 16k 会话（UDP 通话/录音），同步等待完成（≤1s）。
 *        16k 会话启动前必须调用，否则 44.1k 持锁导致 esp_codec_dev 采样率
 *        冲突被静默忽略 → I2S 停在 44.1k → 通话无声（§28.44）。
 * @note  BT 服务未就绪时立即返回；BT 下次播放时 PREPARE 会重开 codec。
 */
void bt_sink_service_audio_yield(void);

/* ---- 以下为 UI 轮询接口（LVGL 线程调用，全部非阻塞、加锁保护）---- */

/** 取元数据快照；返回 false 表示当前无元数据。
 *  ★ title/artist 是锁住的真值（不会被歌词覆盖）。 */
bool bt_sink_service_get_meta(bt_sink_meta_t *out);

/** 取当前播放状态 */
bt_sink_state_t bt_sink_service_get_state(void);

/** 是否正在接收音频流（STREAM STARTED） */
bool bt_sink_service_is_streaming(void);

/**
 * @brief 取最新一句歌词；返回 false 表示 seq 仍为 0（从未收到过歌词）。
 *
 * ── 为什么歌词要单独一个接口，而不是塞进 bt_sink_meta_t ──────────────
 * 国内音乐 App 的"车载蓝牙歌词"实现方式是**把当前歌词行覆盖写进 AVRCP 的
 * TITLE(0x01) 属性**，让老车机不做任何适配就把歌词当"歌名"显示出来。
 * 所以对端推来的 TITLE 是"曲名"和"歌词行"共用的一条通道，本服务必须自己拆：
 *   ① 一次完整元数据突发（TITLE→ARTIST→ALBUM）里的第一个 TITLE 当**真曲名**锁住；
 *   ② 之后 TITLE 再变，就是歌词行 → 走本接口；
 *   ③ 判定"换曲"的判据是**专辑变了** 或 **歌手不再包含已锁歌手**（对端在歌词模式下
 *      会把 ARTIST 改写成 "<曲名> (<译名>) - <歌手>"，所以"包含"仍然成立）。
 * 实测依据与完整日志见 docs/项目规划/改动记录.md §28.28 / §28.29。
 */
bool bt_sink_service_get_lyric(bt_sink_lyric_t *out);

/** 清掉元数据与歌词（重连时服务内部也会调） */
void bt_sink_service_clear_media(void);

/* ---- AVRCP 控制（CT 方向：本机控制对端播放器）---- */

esp_err_t bt_sink_service_play(void);
esp_err_t bt_sink_service_pause(void);
esp_err_t bt_sink_service_next(void);
esp_err_t bt_sink_service_prev(void);

#ifdef __cplusplus
}
#endif

#endif /* BT_SINK_SERVICE_H */
