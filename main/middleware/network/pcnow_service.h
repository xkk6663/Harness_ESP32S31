/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pcnow_service.h
 * @brief 中间层：PC 音乐副屏数据服务（方案 V2，复用 link_manager 链路）。
 *
 * ── 数据来源（见 docs/项目规划/PC音乐副屏局域网推流实施方案.md V2）──────
 *  - PC 每 200ms 发 HEARTBEAT 到 :50003 → link_manager 负责在线状态/对端 IP，
 *    本服务**不碰** 50003，只挂 observer 拿 ONLINE/OFFLINE（4 个槽位的第 4 个）；
 *  - PC 推 NOW_META / NOW_PROG / NOW_LRC(JSON) 到本服务独占的 UDP :50004；
 *  - 大块数据由本服务主动拉：封面 GET http://<peer>:50005/cover/<hash>.jpg、
 *    全量歌词 GET http://<peer>:50005/lyrics.json?k=<hash>（esp_http_client）。
 *
 * ── 歌词 v1.1：时间轴在设备 ──────────────────────────────────────────
 * PC 下发的 lyrics.json 是**已归一化**的行数组（PC 端做完 LRC 解析/排序），
 * 本服务只存数组 + 按 pos_ms 二分查行；pos 用 NOW_PROG(1Hz) + 本地内插时钟，
 * 因此进度条与歌词**同源**，拖进度/丢包/断线重连都立即正确。
 * NOW_LRC（v1 单行推送）保留为回退：全量歌词缺失/解析失败时仍可显示。
 * ★ F44：PC 侧在 lyrics.json 被成功拉走后就**自动停发** NOW_LRC（它已成冗余）；
 *   只有全量歌词没到位时才会继续用这条通道。
 *
 * ── 大块数据"一次一首"（F44）─────────────────────────────────────────
 * 封面/歌词每首歌**各只拉一次**，靠 META 里的 hash 判重（`s_cover_hash`/`s_lrc_hash`）。
 * 前提是 PC 只在**数据真正就绪**时才在 META 里带 hash；否则设备端会凭 hash 去拉，
 * 撞上 404 后无限重试（每 3s 重下一张大图），把 UDP 收包任务堵死。
 *
 * ── 线程纪律（铁律，同 bt_sink_service）───────────────────────────────
 * 收包任务（pcnow_rx）**只解析 JSON**；一切阻塞 IO/解码在拉取任务（pcnow_fetch）里做
 * （F44：在收包任务里 http_get 会导致 UDP 成片丢包）；两者都不碰 LVGL，只更新
 * 临界区保护的快照，UI 用 lv_timer 轮询 pcnow_service_get_info()。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCNOW_UDP_PORT     50004      /* PC → 设备 NOW_* JSON（ICD V1.3） */
#define PCNOW_HTTP_PORT    50005      /* PC 侧封面/歌词拉取服务 */
#define PCNOW_TITLE_LEN    128
#define PCNOW_ARTIST_LEN   128
#define PCNOW_ALBUM_LEN    128
#define PCNOW_PLAYER_LEN   32
#define PCNOW_LRC_LEN      128        /* 单行歌词 */
#define PCNOW_COVER_HASH_LEN 40       /* SHA1 前 8 hex，留余量 */
#define PCNOW_SYNC_SRC_LEN 16         /* ICD V1.3：同步来源标识 */

typedef enum {
    PCNOW_OFFLINE = 0,   /* link_manager 离线（没收到 PC 心跳） */
    PCNOW_WAITING,       /* 在线但还没收到 NOW_META */
    PCNOW_LIVE,          /* 正在接收 PC 推流 */
} pcnow_state_t;

/** UI 轮询快照（UTF-8，NUL 结尾）。pos_ms 为**本地内插**后的实时进度。 */
typedef struct {
    char     title[PCNOW_TITLE_LEN];
    char     artist[PCNOW_ARTIST_LEN];
    char     album[PCNOW_ALBUM_LEN];
    char     player[PCNOW_PLAYER_LEN];
    uint32_t duration_ms;
    uint32_t pos_ms;            /* 内插值；暂停时冻结 */
    bool     playing;
    char     lrc_text[PCNOW_LRC_LEN];   /* 当前行（v1.1 自算；v1 回退则来自推送） */
    char     lrc_prev[PCNOW_LRC_LEN];   /* 上一行（无则空串） */
    char     lrc_next[PCNOW_LRC_LEN];   /* 下一行（无则空串） */
    int      lrc_index;          /* 当前行号；-1 = 无歌词/间奏 */
    uint32_t lrc_seq;            /* 行号变化时递增，UI 只在变化时刷新 */
    uint32_t cover_id;           /* 新封面就绪时递增，UI 只在变化时换图 */
    bool     cover_ready;
    /**
     * ICD V1.3 同步置信度（来自 META 的 sync_src），UI 直接显示用：
     *   "timeline" 真时间轴（准） / "dom" CDP DOM（准） / "a11y" 界面读取（准）
     *   "manual"   已人工校准    / "event" 仅事件锚定（可能偏移）
     * 空串 = 老版 PC，按 "event" 处理。
     */
    char     sync_src[PCNOW_SYNC_SRC_LEN];
    bool     lrc_line_from_pc;   /* true = 本行由 PC 的 line 通道指定（非本机二分） */
} pcnow_info_t;

/**
 * @brief 初始化：注册 link_manager observer、起收包任务（绑 :50004）。
 * @note  在 link_manager_init() 之后调用（main.c 已保证顺序）。
 *        收包任务栈 8KB PSRAM 外置（F35 模式，照 udp_voice/link_manager）。
 */
void pcnow_service_init(void);

/** 取快照（LVGL 线程调用；内部加锁、非阻塞）。返回 false = 尚无任何 META。 */
bool pcnow_service_get_info(pcnow_info_t *out);

/** 取链路状态（直接透传 link_manager 在线位 + 是否收到过 META）。 */
pcnow_state_t pcnow_service_get_state(void);

/**
 * @brief 取当前封面像素（LVGL 线程；锁内拷指针，不拷像素）。
 * @return RGB888(LVGL 序) 缓冲指针；未就绪返回 NULL。缓冲由服务持有，
 *         UI 只应在 cover_id 变化时重新取用（换面时旧缓冲仍在，不会悬空）。
 */
const uint8_t *pcnow_service_get_cover(int *w, int *h);

/**
 * @brief 向 PC 发送媒体控制命令（{"type":"NOW_CMD","cmd":"..."}，走
 *        link_manager_send_json → PC:50003；PC 端 NetLink 需 bind 50003）。
 * @param cmd "play" / "pause" / "next" / "prev"
 * @note  任意线程可调（内部只组包 + UDP 发送，无阻塞）。
 */
void pcnow_service_send_cmd(const char *cmd);

/**
 * @brief 发送进度跳转命令（NOW_CMD seek + pos_ms），并把本地内插基准立即
 *        跳到 pos_ms（UI 即时反馈；PC 的下一拍 NOW_PROG 会再校准）。
 * @note  尽力而为：SMTC seek 仅对上报时间轴的播放器有效（网易云 Win32 不支持，
 *        表现为"只动显示不动音乐"）。
 */
void pcnow_service_send_seek(uint32_t pos_ms);

#ifdef __cplusplus
}
#endif
