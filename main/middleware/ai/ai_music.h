/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_music.h
 * @brief 小智 MCP 在线点歌工具（S7-B1 / §73.25 / §73.27 方案 B）。
 *
 * 工具：self.music.play(name) / self.music.stop
 *
 * ★ §73.27 播放链路 = 会话内并行：xiaozhi_service_music_play(url) →
 *   av_processor audio_playback（s_play，闲置至今的"URL 播放入口"）
 *   → esp_audio_simple_player 解码 → 与 TTS 汇入同一条下行流出声。
 *   小智会话全程不断；TTS feeder priority > 音乐 playback priority，
 *   小智说话时音乐被音频策略自动让路。media_player_service（本地
 *   MP3/FILES 页）与点歌彻底解耦，各自独立通路。
 *
 * 音源：端侧搜索为主（k_music_apis 多端点兜底，移植 vocat）——服务端
 * LLM 只给歌名，没有编 URL 的机会（url 参数已删：MCP 引擎 tools/call
 * 校验会强制要求所有注册属性，双属性 = LLM 只给 name 时被拒单，
 * esp_mcp_engine.c esp_mcp_tool_args_cb 实锤）。
 */

#pragma once

#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册 MCP 点歌工具（chat_setup 里与其他 ai_* 并列挂载）
 */
esp_err_t ai_music_attach(esp_mcp_t *mcp);

/** 播放结果回调（kicker 任务 / simple_player 播放任务上下文调用，勿阻塞）：
 *  搜索失败 / 播放失败 / 自然播完的文字结论。xiaozhi_service_init 里注册
 *  成 XZ_EVT_TEXT 通道（同 ai_punch 模式）；未注册时只打 LOG。 */
typedef void (*ai_music_result_cb_t)(const char *text);

void ai_music_set_result_cb(ai_music_result_cb_t cb);

#ifdef __cplusplus
}
#endif
