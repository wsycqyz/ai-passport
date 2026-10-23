// main/vocab_audio.c - audio worker task: word clips + synthesized cues (spec 8.1, 8.4, 8.7);
// see vocab_audio.h for the contract.
//
// One worker task owns every bsp_audio_* call after vocab_audio_start(). The app task (the
// only producer) sends requests through a depth-4 queue without blocking. The worker
// renders 256-sample chunks (16 ms at 16 kHz) into a static buffer - cue synthesis
// (vocab_tone) and IMA-ADPCM decoding straight from the clip blob mapped in flash
// (vocab_clips + vocab_adpcm) - and writes them with the blocking bsp_audio_write(), which
// paces the loop on the I2S DMA (6 x 240 frames, about 90 ms). Between chunks it drains
// the queue: a new PLAY or STOP preempts the current sound after a 4 ms fade-out (no
// click); SET_VOLUME is applied at once and volume 0 stops the sound without writing
// anything (mute = no I2S writes). After a sound ends or is stopped the worker writes
// about 96 ms of silence before it reports idle, so the DMA ring holds only silence when
// the app starts an NVS write (spec 10.4: flash writes stall the cache and could replay a
// stale DMA buffer). Static memory only (task stack, queue, buffers); no allocation.
#include "vocab_audio.h"

#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "vocab_adpcm.h"
#include "vocab_clips.h"
#include "vocab_config.h"
#include "vocab_tone.h"

static const char *TAG = "vocab_audio";

#define AUDIO_TASK_PRIO     6     // above the app task (5) and the LVGL port task (4)
#define AUDIO_STACK_BYTES   3072
#define CHUNK               VOCAB_AUDIO_CHUNK_SAMPLES
#define FADE_SAMPLES        64    // 4 ms fade-out when a sound is preempted or stopped
#define FLUSH_CHUNKS        6     // 96 ms of silence: covers the 90 ms I2S DMA ring
#define WAIT_POLL_MS        2

// Clip pack embedded by main/CMakeLists.txt (the local test pack, or the empty pack in CI and
// public builds; amendment A6), read in place from flash after vocab_clips_install().
extern const uint8_t clips_blob_start[] asm("_binary_vocab_clips_bin_start");
extern const uint8_t clips_blob_end[] asm("_binary_vocab_clips_bin_end");

typedef enum {
    REQ_PLAY = 1,   // cue and/or uid (cue first when both are set)
    REQ_STOP,
    REQ_VOLUME,
    REQ_SHUTDOWN,
} req_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t cue;
    uint8_t volume;
    uint8_t reserved;
    vocab_uid_t uid;
} audio_req_t;

// Sound being rendered (worker task only).
typedef struct {
    bool active;
    bool written;          // at least one chunk of this sound reached the I2S driver
    bool in_word;          // cue finished (or none): rendering the word clip
    vocab_uid_t uid;       // word after the cue, VOCAB_UID_NONE for a cue alone
    vocab_tone_t tone;
    const vocab_clip_t *clip;
    const uint8_t *codes;  // ADPCM codes of the clip (after its header)
    uint32_t pos;          // next code index
    vocab_adpcm_state_t adpcm;
} sound_t;

static StaticQueue_t s_queue_buf;
static uint8_t s_queue_storage[VOCAB_AUDIO_QUEUE_DEPTH * sizeof(audio_req_t)];
static QueueHandle_t s_queue;
static StaticSemaphore_t s_ack_buf;
static SemaphoreHandle_t s_ack;
static StaticTask_t s_tcb;
static StackType_t s_stack[AUDIO_STACK_BYTES];
static TaskHandle_t s_task;

static volatile bool s_available;  // set by the app task (start/shutdown)
static volatile bool s_busy;       // worker: a request is being served or PCM is written
static uint8_t s_app_volume;       // app task: last requested volume

// Worker-only state.
static sound_t s_snd;
static int16_t s_pcm[CHUNK];
static uint8_t s_volume;
static uint8_t s_flush_left;
static bool s_wrote;               // PCM written since the last silence flush
static uint32_t s_write_errors;
static bool s_stack_logged;

// ---------------------------------------------------------------------------------------
// Clip lookup (pure; any task)
// ---------------------------------------------------------------------------------------

// The installed pack was validated as a whole (bounds, sizes, clip headers) at boot.
static const vocab_clip_t *playable_clip(vocab_uid_t uid)
{
    const vocab_clip_t *clip = vocab_clip_find(uid);
    const uint8_t *bytes = clip ? vocab_clip_bytes(clip) : NULL;
    vocab_adpcm_state_t st;
    if (!bytes || !vocab_adpcm_init(&st, bytes, clip->n_bytes)) return NULL;
    return clip;
}

bool vocab_audio_has_clip(vocab_uid_t uid)
{
    return playable_clip(uid) != NULL;
}

// ---------------------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------------------

static bool start_word(vocab_uid_t uid)
{
    const vocab_clip_t *clip = playable_clip(uid);
    if (!clip) return false;
    const uint8_t *bytes = vocab_clip_bytes(clip);
    s_snd.clip = clip;
    s_snd.codes = bytes + VOCAB_ADPCM_HEADER_BYTES;
    s_snd.pos = 0;
    s_snd.in_word = true;
    return vocab_adpcm_init(&s_snd.adpcm, bytes, clip->n_bytes);
}

static void start_sound(const audio_req_t *req)
{
    memset(&s_snd, 0, sizeof(s_snd));
    s_snd.uid = req->uid;
    if (req->cue != VOCAB_CUE_NONE && vocab_tone_start(&s_snd.tone, (vocab_cue_t)req->cue)) {
        s_snd.active = true;
        return;
    }
    s_snd.active = req->uid != VOCAB_UID_NONE && start_word(req->uid);
}

// Renders up to `max` samples of the current sound (cue, then word); 0 when finished.
static size_t render(int16_t *out, size_t max)
{
    size_t n = 0;
    while (n < max && s_snd.active) {
        if (!s_snd.in_word) {
            const size_t got = vocab_tone_render(&s_snd.tone, out + n, max - n);
            n += got;
            if (got == 0 && !(s_snd.uid != VOCAB_UID_NONE && start_word(s_snd.uid))) {
                s_snd.active = false;
            }
            continue;
        }
        const uint32_t left = s_snd.clip->n_samples - s_snd.pos;
        const size_t take = (max - n) < left ? (max - n) : (size_t)left;
        vocab_adpcm_decode(&s_snd.adpcm, s_snd.codes, s_snd.pos, take, out + n);
        s_snd.pos += (uint32_t)take;
        n += take;
        if (s_snd.pos >= s_snd.clip->n_samples) s_snd.active = false;
    }
    return n;
}

static bool write_pcm(const int16_t *pcm, size_t samples)
{
    if (samples == 0 || s_volume == 0) return true;  // mute: never write
    const esp_err_t err = bsp_audio_write(pcm, samples * sizeof(int16_t));
    if (err != ESP_OK) {
        if (s_write_errors++ % 50u == 0) {
            ESP_LOGW(TAG, "I2S write failed (%s); sound stopped", esp_err_to_name(err));
        }
        return false;
    }
    s_wrote = true;
    return true;
}

// Ends the current sound with a short linear fade so a cut never clicks.
static void fade_out(void)
{
    if (!s_snd.active) return;
    if (!s_snd.written) {  // nothing audible yet: drop it without writing
        s_snd.active = false;
        return;
    }
    const size_t n = render(s_pcm, FADE_SAMPLES);
    for (size_t i = 0; i < n; i++) {
        s_pcm[i] = (int16_t)(((int32_t)s_pcm[i] * (int32_t)(FADE_SAMPLES - i)) / FADE_SAMPLES);
    }
    (void)write_pcm(s_pcm, n);
    s_snd.active = false;
}

static void end_sound(bool flush)
{
    fade_out();
    s_flush_left = (flush && s_wrote && s_volume > 0) ? FLUSH_CHUNKS : 0;
}

static void apply_volume(uint8_t percent)
{
    if (percent > 100u) percent = 100u;
    if (percent == 0) {
        // Mute: stop at once without writing (the codec output is silenced as well).
        s_snd.active = false;
        s_flush_left = 0;
        s_wrote = false;
    }
    s_volume = percent;
    bsp_audio_set_volume(percent);
}

// Serves every queued request; returns false on SHUTDOWN.
static bool drain_requests(void)
{
    audio_req_t req;
    while (xQueueReceive(s_queue, &req, 0) == pdTRUE) {
        switch ((req_kind_t)req.kind) {
        case REQ_PLAY:
            end_sound(false);
            s_flush_left = 0;
            if (s_volume > 0) start_sound(&req);
            break;
        case REQ_STOP:
            end_sound(true);
            break;
        case REQ_VOLUME:
            apply_volume(req.volume);
            break;
        case REQ_SHUTDOWN:
            s_snd.active = false;
            s_flush_left = 0;
            return false;
        default:
            break;
        }
    }
    return true;
}

static void worker(void *arg)
{
    (void)arg;
    audio_req_t peek;
    for (;;) {
        s_busy = false;
        if (xQueuePeek(s_queue, &peek, portMAX_DELAY) != pdTRUE) continue;
        s_busy = true;  // before the request leaves the queue: idle is never reported early
        if (!drain_requests()) break;
        while (s_snd.active || s_flush_left > 0) {
            if (s_snd.active) {
                const size_t n = render(s_pcm, CHUNK);
                if (!write_pcm(s_pcm, n)) {
                    s_snd.active = false;
                    s_flush_left = 0;
                } else if (s_snd.active) {
                    s_snd.written = true;
                } else {
                    s_flush_left = s_wrote ? FLUSH_CHUNKS : 0;
                }
            } else {
                memset(s_pcm, 0, sizeof(s_pcm));
                if (!write_pcm(s_pcm, CHUNK)) s_flush_left = 1;
                if (--s_flush_left == 0) {
                    s_wrote = false;
                    if (!s_stack_logged) {
                        s_stack_logged = true;
                        ESP_LOGI(TAG, "worker stack high-water mark: %u bytes free",
                                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
                    }
                }
            }
            if (!drain_requests()) goto shutdown;
        }
    }
shutdown:
    s_busy = false;
    // The owner deletes this task after the acknowledgement; no bsp_audio_* call follows.
    xSemaphoreGive(s_ack);
    for (;;) vTaskSuspend(NULL);
}

// ---------------------------------------------------------------------------------------
// App-task API
// ---------------------------------------------------------------------------------------

esp_err_t vocab_audio_start(uint8_t volume_percent)
{
    if (s_available) return ESP_OK;
    // Install the embedded pack first: clip lookups work even when the codec does not.
    const size_t pack_bytes = (size_t)(clips_blob_end - clips_blob_start);
    const vocab_clips_status_t pack = vocab_clips_install(clips_blob_start, pack_bytes);
    if (pack != VOCAB_CLIPS_OK) {
        ESP_LOGE(TAG, "clip pack rejected (%s, %u bytes): no word audio", vocab_clips_status_name(pack),
                 (unsigned)pack_bytes);
    } else if (vocab_clips_count() == 0) {
        ESP_LOGW(TAG, "no word clips in this build (empty pack): words use the visual substitute");
    }
    esp_err_t err = bsp_audio_init();
    if (err == ESP_OK) {
        err = bsp_audio_set_format(VOCAB_AUDIO_RATE_HZ, VOCAB_AUDIO_BITS, VOCAB_AUDIO_CHANNELS);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio unavailable (%s): running silently", esp_err_to_name(err));
        return err;
    }
    if (volume_percent > 100u) volume_percent = 100u;
    s_volume = volume_percent;
    s_app_volume = volume_percent;
    bsp_audio_set_volume(volume_percent);
    if (!s_queue) {
        s_queue = xQueueCreateStatic(VOCAB_AUDIO_QUEUE_DEPTH, sizeof(audio_req_t), s_queue_storage,
                                     &s_queue_buf);
        s_ack = xSemaphoreCreateBinaryStatic(&s_ack_buf);
    }
    if (!s_queue || !s_ack || s_task) return ESP_ERR_INVALID_STATE;
    s_busy = false;
    s_task = xTaskCreateStatic(worker, "vocab_audio", AUDIO_STACK_BYTES, NULL, AUDIO_TASK_PRIO,
                               s_stack, &s_tcb);
    if (!s_task) return ESP_ERR_NO_MEM;
    s_available = true;
    ESP_LOGI(TAG, "audio ready: %u clips, %u-byte pack, volume %u%%", (unsigned)vocab_clips_count(),
             (unsigned)vocab_clips_blob_bytes(), (unsigned)volume_percent);
    return ESP_OK;
}

bool vocab_audio_available(void)
{
    return s_available;
}

// Zero-timeout send. Control requests (stop, volume, shutdown) may drop pending plays to
// make room: they must never be lost.
static bool send_request(const audio_req_t *req, bool control)
{
    if (!s_available) return false;
    if (xQueueSend(s_queue, req, 0) == pdTRUE) return true;
    if (control) {
        xQueueReset(s_queue);
        if (xQueueSend(s_queue, req, 0) == pdTRUE) return true;
    }
    ESP_LOGW(TAG, "request queue full: request %u dropped", (unsigned)req->kind);
    return false;
}

static bool request_play(vocab_cue_t cue, vocab_uid_t uid)
{
    if (!s_available || s_app_volume == 0) return false;
    const audio_req_t req = { .kind = REQ_PLAY, .cue = (uint8_t)cue, .uid = uid };
    return send_request(&req, false);
}

bool vocab_audio_play_word(vocab_uid_t uid)
{
    return vocab_audio_has_clip(uid) && request_play(VOCAB_CUE_NONE, uid);
}

bool vocab_audio_play_cue(vocab_cue_t cue)
{
    return vocab_tone_total_samples(cue) > 0 && request_play(cue, VOCAB_UID_NONE);
}

bool vocab_audio_play_cue_then_word(vocab_cue_t cue, vocab_uid_t uid)
{
    const bool cue_ok = vocab_tone_total_samples(cue) > 0;
    const bool word_ok = vocab_audio_has_clip(uid);
    if (!cue_ok && !word_ok) return false;
    return request_play(cue_ok ? cue : VOCAB_CUE_NONE, word_ok ? uid : VOCAB_UID_NONE);
}

void vocab_audio_stop(void)
{
    const audio_req_t req = { .kind = REQ_STOP };
    (void)send_request(&req, true);
}

void vocab_audio_set_volume(uint8_t percent)
{
    if (percent > 100u) percent = 100u;
    s_app_volume = percent;
    const audio_req_t req = { .kind = REQ_VOLUME, .volume = percent };
    (void)send_request(&req, true);
}

bool vocab_audio_is_idle(void)
{
    if (!s_available) return true;
    return !s_busy && uxQueueMessagesWaiting(s_queue) == 0;
}

bool vocab_audio_wait_idle(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    while (!vocab_audio_is_idle()) {
        if ((TickType_t)(xTaskGetTickCount() - start) >= limit) return vocab_audio_is_idle();
        vTaskDelay(pdMS_TO_TICKS(WAIT_POLL_MS) ? pdMS_TO_TICKS(WAIT_POLL_MS) : 1);
    }
    return true;
}

esp_err_t vocab_audio_shutdown(uint32_t timeout_ms)
{
    if (!s_available || !s_task) return ESP_OK;
    (void)xSemaphoreTake(s_ack, 0);  // clear a stale acknowledgement
    const audio_req_t req = { .kind = REQ_SHUTDOWN };
    if (!send_request(&req, true)) return ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(s_ack, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "audio worker did not stop within %u ms", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    s_available = false;
    vTaskDelete(s_task);
    s_task = NULL;
    return ESP_OK;
}
