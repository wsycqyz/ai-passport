// main/vocab_audio.h - audio worker task: word clips + synthesized cues (spec 8.1, 8.7).
//
// DEVICE ONLY (includes esp_err.h; never included by pure modules or host tests).
//
// Ownership: after vocab_audio_start() the worker task owns every bsp_audio_* call (format,
// volume, writes). The only exception is the deep-sleep path: after vocab_audio_shutdown()
// returned ESP_OK, the app task calls bsp_audio_sleep() and bsp_audio_prepare_deep_sleep().
//
// Callers: the app task (vocab_app.c) only - a single producer. Request functions never
// block (zero-timeout queue send, depth VOCAB_AUDIO_QUEUE_DEPTH; a full queue drops the
// request, logs, and returns false). Only vocab_audio_wait_idle() and
// vocab_audio_shutdown() block, both bounded by their timeout.
//
// Playback: 16 kHz / 16-bit / mono. Clips are IMA-ADPCM decoded from the embedded,
// self-describing clip pack (vocab_clips.h, vocab_adpcm.h) and cues synthesized
// (vocab_tone.h), streamed in chunks of
// VOCAB_AUDIO_CHUNK_SAMPLES (16 ms) from static buffers (no allocation per sound). Any new
// PLAY_* or STOP request preempts the current sound at the next chunk boundary. At volume
// 0 nothing is written to I2S. A write error stops the current sound and is logged; the
// worker keeps serving requests. The worker never touches LVGL or NVS.
//
// Failure: if bsp_audio_init()/bsp_audio_set_format() or task creation fails, the module
// stays "unavailable": vocab_audio_available() is false, requests return false and do
// nothing, vocab_audio_is_idle() is true. The app then shows the dim mute icon and uses
// WORD_PULSE for every clip (spec 8.7). Never aborts.
//
// Owner: assets developer. Contract written by the lead; only additive changes.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "vocab_types.h"

// Boot (app task, once): installs the embedded clip pack (vocab_clips_install(); a rejected
// or empty pack only means "no word clips" and is logged), then bsp_audio_init(),
// bsp_audio_set_format(16000, 16, 1), bsp_audio_set_volume(volume_percent), queue + worker
// task creation (priority above the app and LVGL tasks, about 3 KB stack). Returns ESP_OK
// when audio is usable, otherwise the first error (module unavailable, see above).
esp_err_t vocab_audio_start(uint8_t volume_percent);

// True after a successful vocab_audio_start() and before vocab_audio_shutdown().
bool vocab_audio_available(void);

// True when `uid` has a playable clip in the pack that vocab_audio_start() installed (the
// empty pack embedded in CI and public builds has none). Pure lookup; any task.
bool vocab_audio_has_clip(vocab_uid_t uid);

// Requests (app task). Return false when unavailable, unknown cue/uid, or queue full, and
// at volume 0 (mute writes nothing; the session substitutes WORD_PULSE before asking).
// Requests may be sent while a sound plays: PLAY_* replaces it after a 4 ms fade-out.
bool vocab_audio_play_word(vocab_uid_t uid);
bool vocab_audio_play_cue(vocab_cue_t cue);
bool vocab_audio_play_cue_then_word(vocab_cue_t cue, vocab_uid_t uid);  // cue, then clip
void vocab_audio_stop(void);                       // preempt and discard pending plays
void vocab_audio_set_volume(uint8_t percent);      // 80 / 30 / 0; applied by the worker

// Save gate (spec 10.4): true when no PCM is being written and no request is pending.
// After a sound ends or is stopped the worker first writes about 96 ms of silence (the I2S
// DMA ring then holds only silence), so idle follows a STOP within about 0.2 s.
// Safe from any task (reads a volatile flag).
bool vocab_audio_is_idle(void);

// Blocks the calling (app) task up to `timeout_ms` until idle; returns the final idle state.
bool vocab_audio_wait_idle(uint32_t timeout_ms);

// Deep sleep (app task): stop playback, end the worker and wait up to `timeout_ms` for its
// acknowledgement. ESP_OK: the worker no longer exists and makes no bsp_audio_* call.
// ESP_ERR_TIMEOUT: the worker did not stop (the caller restarts instead of sleeping).
// Calling it when unavailable returns ESP_OK.
esp_err_t vocab_audio_shutdown(uint32_t timeout_ms);
