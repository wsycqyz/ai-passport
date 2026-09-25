// main/sonic_listener.c —— see sonic_listener.h.
#include "sonic_listener.h"

#include "bsp_audio.h"
#include "sonic_link.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>

static const char *TAG = "sonic_listener";

#define CHUNK_SAMPLES     256           // 16 ms; the I2S DMA ring holds 90 ms
#define STOP_TIMEOUT_MS   2500          // one blocked read times out after 1 s
#define TASK_STACK_BYTES  6144          // receiver + RS decode work arrays
// Above the LVGL task (4) and the controller (5) so rendering never delays
// I2S reads; the worker blocks in bsp_audio_read() most of the time.
#define TASK_PRIORITY     6
#define CLIP_LEVEL        32000
#define CLIP_HOLD_CHUNKS  30

static sonic_rx_t s_rx;                 // owned by the worker while it runs
static TaskHandle_t s_task;
static SemaphoreHandle_t s_stopped;
static volatile bool s_cancel;
static sonic_listener_post_t s_post;
static bool s_audio_awake;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static sonic_listener_status_t s_status;
static sonic_wifi_credentials_t s_creds;
static bool s_have_creds;

static void post(sonic_listener_event_t event)
{
    if (s_post) s_post(event);
}

// Map chunk RMS to 0..100 over -60..0 dBFS.
static uint8_t level_percent(uint64_t sum_squares)
{
    const float rms = sqrtf((float)sum_squares / CHUNK_SAMPLES);
    if (rms < 32.8f) return 0;          // below -60 dBFS
    const float dbfs = 20.0f * log10f(rms / 32768.0f);
    const float pct = (dbfs + 60.0f) * (100.0f / 60.0f);
    return (uint8_t)(pct > 100.0f ? 100.0f : pct);
}

static void handle_frame(void)
{
    size_t len = 0;
    const uint8_t *payload = sonic_rx_payload(&s_rx, &len);
    sonic_wifi_credentials_t creds;
    const sonic_wifi_status_t status = sonic_wifi_parse(payload, len, &creds);
    sonic_rx_clear_payload(&s_rx);
    if (status != SONIC_WIFI_OK) {
        ESP_LOGW(TAG, "frame decoded but rejected as Wi-Fi credentials (%d)", (int)status);
        post(SONIC_LISTENER_EVT_UNSUPPORTED);
        return;
    }
    ESP_LOGI(TAG, "credentials received (SSID %u bytes, password %u chars)",
             creds.ssid_len, creds.password_len);
    taskENTER_CRITICAL(&s_lock);
    s_creds = creds;
    s_have_creds = true;
    taskEXIT_CRITICAL(&s_lock);
    sonic_wifi_wipe(&creds);
    post(SONIC_LISTENER_EVT_CREDENTIALS);
}

static void worker(void *arg)
{
    (void)arg;
    int16_t pcm[CHUNK_SAMPLES];
    uint32_t chunks = 0;
    int64_t worst_us = 0;
    int clip_hold = 0;
    uint8_t level = 0;

    while (!s_cancel) {
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            if (!s_cancel) {
                ESP_LOGE(TAG, "microphone read failed");
                post(SONIC_LISTENER_EVT_AUDIO_ERROR);
            }
            break;
        }
        uint64_t sum = 0;
        int peak = 0;
        for (int i = 0; i < CHUNK_SAMPLES; i++) {
            const int s = pcm[i];
            sum += (uint64_t)((int64_t)s * s);
            const int a = s < 0 ? -s : s;
            if (a > peak) peak = a;
        }

        const int64_t t0 = esp_timer_get_time();
        const uint32_t events = sonic_rx_process(&s_rx, pcm, CHUNK_SAMPLES);
        const int64_t spent = esp_timer_get_time() - t0;
        if (spent > worst_us) worst_us = spent;
        chunks++;

        const uint8_t now = level_percent(sum);
        level = now > level ? now : (level > 3 ? (uint8_t)(level - 3) : 0);
        clip_hold = peak >= CLIP_LEVEL ? CLIP_HOLD_CHUNKS : (clip_hold > 0 ? clip_hold - 1 : 0);
        const sonic_rx_state_t state = sonic_rx_state(&s_rx);
        taskENTER_CRITICAL(&s_lock);
        s_status.level = level;
        s_status.clipping = clip_hold > 0;
        s_status.receiving = state == SONIC_RX_BODY;
        s_status.progress = sonic_rx_progress(&s_rx);
        taskEXIT_CRITICAL(&s_lock);

        if (events & SONIC_RX_EV_HEADER) post(SONIC_LISTENER_EVT_RECEIVING);
        // Header failures are usually noise that briefly looked like a
        // preamble, so only failures after a valid header are reported.
        if ((events & SONIC_RX_EV_ERROR) && sonic_rx_last_error(&s_rx) != SONIC_RX_ERR_HEADER) {
            ESP_LOGW(TAG, "frame rejected (error %d)", (int)sonic_rx_last_error(&s_rx));
            post(SONIC_LISTENER_EVT_DAMAGED);
        }
        if (events & SONIC_RX_EV_FRAME) handle_frame();
    }

    ESP_LOGI(TAG, "stopped after %lu chunks; worst %lld us per 16 ms chunk; stack free %u B",
             (unsigned long)chunks, (long long)worst_us,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    memset(pcm, 0, sizeof(pcm));
    // Last access to shared state: the owner deletes this task after the take.
    xSemaphoreGive(s_stopped);
    for (;;) vTaskSuspend(NULL);
}

esp_err_t sonic_listener_start(sonic_listener_post_t post_fn)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    if (!s_stopped) {
        s_stopped = xSemaphoreCreateBinary();
        if (!s_stopped) return ESP_ERR_NO_MEM;
    }
    // After bsp_audio_sleep() the codec must be woken; set_format also covers
    // a codec that was never put to sleep.
    esp_err_t err = bsp_audio_wake();
    if (err == ESP_OK) err = bsp_audio_set_format(SONIC_SAMPLE_RATE_HZ, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "microphone unavailable: %s", esp_err_to_name(err));
        (void)bsp_audio_sleep();
        return err;
    }
    s_audio_awake = true;

    sonic_rx_init(&s_rx);
    sonic_rx_set_max_payload(&s_rx, SONIC_WIFI_PAYLOAD_MAX);
    taskENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_have_creds = false;
    taskEXIT_CRITICAL(&s_lock);
    sonic_wifi_wipe(&s_creds);
    s_post = post_fn;
    s_cancel = false;
    (void)xSemaphoreTake(s_stopped, 0);
    if (xTaskCreate(worker, "sonic_rx", TASK_STACK_BYTES, NULL, TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        s_audio_awake = false;
        (void)bsp_audio_sleep();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sonic_listener_stop(void)
{
    TaskHandle_t task = s_task;
    if (task) {
        s_cancel = true;
        if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "worker did not stop in time");
            return ESP_ERR_TIMEOUT;   // keep the handle so a retry can finish
        }
        vTaskDelete(task);
        s_task = NULL;
    }
    if (s_audio_awake) {
        const esp_err_t err = bsp_audio_sleep();
        if (err != ESP_OK) ESP_LOGW(TAG, "codec sleep failed: %s", esp_err_to_name(err));
        s_audio_awake = false;
    }
    s_post = NULL;
    sonic_rx_reset(&s_rx);   // wipes any partially received secret
    taskENTER_CRITICAL(&s_lock);
    s_have_creds = false;
    memset(&s_status, 0, sizeof(s_status));
    taskEXIT_CRITICAL(&s_lock);
    sonic_wifi_wipe(&s_creds);
    return ESP_OK;
}

void sonic_listener_status(sonic_listener_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

bool sonic_listener_take(sonic_wifi_credentials_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    const bool have = s_have_creds;
    if (have) *out = s_creds;
    s_have_creds = false;
    sonic_wifi_wipe(&s_creds);
    taskEXIT_CRITICAL(&s_lock);
    return have;
}
