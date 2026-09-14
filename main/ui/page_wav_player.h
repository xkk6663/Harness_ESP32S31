/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_wav_player.h
 * @brief WAV player page (play / stop / repeat / volume) + audio event
 *        registration that re-enables tab buttons on completion.
 */

#pragma once

#include "page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const Page_t Page_WavPlayer;

/* Open the player page for a SPIFFS file path. */
void Page_WavPlayer_Open(const char *path);

/* Register audio completion callbacks (called once by Page_Main). */
void page_wav_player_register_audio_events(void);

#ifdef __cplusplus
}
#endif
