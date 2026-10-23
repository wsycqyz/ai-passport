// main/vocab_app.c - the Word Buddy app task (see vocab_app.h).
//
// One task owns the engine, the view model, the persistent store and the key queue:
//   boot: battery gauge, NVS (no automatic erase: any failure -> volatile mode), stored
//         progress, audio worker, UI widgets, engine (HOME), backlight on after the first frame
//   loop: block on the key queue until the next engine deadline; feed the engine; execute its
//         commands in order; render the view once under bsp_lvgl_lock()
// Commands: audio requests go to the audio worker; NEW_EPOCH drains the key queue while
// keeping PRESS/RELEASE bookkeeping; SAVE runs the store with the audio-idle save gate;
// BACKLIGHT also pauses/resumes animations; READ_BATTERY polls the fuel gauge;
// DEEP_SLEEP runs the shutdown sequence of main/demo_low_power.c (GPIO0 key wake only).
#include "vocab_app.h"

#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "vocab_audio.h"
#include "vocab_bank.h"
#include "vocab_config.h"
#include "vocab_engine.h"
#include "vocab_store.h"
#include "vocab_ui.h"

// The pure core mirrors the BSP enums numerically (vocab_types.h).
_Static_assert((int)BSP_BTN_UP == (int)VOCAB_KEY_UP, "key mapping");
_Static_assert((int)BSP_BTN_DOWN == (int)VOCAB_KEY_DOWN, "key mapping");
_Static_assert((int)BSP_BTN_OK == (int)VOCAB_KEY_OK, "key mapping");
_Static_assert((int)BSP_BTN_PRESS == (int)VOCAB_RAW_PRESS, "event mapping");
_Static_assert((int)BSP_BTN_CLICK == (int)VOCAB_RAW_CLICK, "event mapping");
_Static_assert((int)BSP_BTN_DOUBLE == (int)VOCAB_RAW_DOUBLE, "event mapping");
_Static_assert((int)BSP_BTN_LONG == (int)VOCAB_RAW_LONG, "event mapping");
_Static_assert((int)BSP_BTN_RELEASE == (int)VOCAB_RAW_RELEASE, "event mapping");
_Static_assert(VOCAB_BTN_SHORT_PRESS_MS == BSP_BTN_SHORT_PRESS_MS, "double-click window mirror");

static const char *TAG = "vocab_app";

#define APP_TASK_STACK          6144
#define APP_TASK_PRIO           5     // above the LVGL port task (4); the audio worker runs higher
#define LVGL_LOCK_MS            200
#define AUDIO_SHUTDOWN_MS       1000
#define FIRST_FRAME_POLLS       50    // x 10 ms: bound for the boot frame wait

static QueueHandle_t s_keys;
static TaskHandle_t s_task;
// Large state lives in static storage, never on the task stack.
static vocab_engine_t s_engine;
static vocab_view_t s_view;
static vocab_outbox_t s_out;
static vocab_store_t s_store;
static vocab_meta_t s_meta;
static vocab_record_t s_records[VOCAB_RECORDS_MAX];
static const vocab_bank_t *s_bank;
static bool s_ui_ok;
static bool s_ui_paused;
static uint32_t s_screens_logged;        // (screen, stage) pairs whose LVGL memory was logged
static size_t s_min_biggest_free = SIZE_MAX;

static vocab_ms_t now_ms(void)
{
    return (vocab_ms_t)(esp_timer_get_time() / 1000);
}

static uint8_t volume_percent(uint8_t index)
{
    static const uint8_t levels[VOCAB_VOL_LEVEL_COUNT] = VOCAB_VOL_LEVELS_INIT;
    return levels[index < VOCAB_VOL_LEVEL_COUNT ? index : VOCAB_VOL_DEFAULT_INDEX];
}

// ---------------------------------------------------------------------------------------
// NVS key-value adapter for vocab_store (namespace VOCAB_NVS_NAMESPACE)
// ---------------------------------------------------------------------------------------

static nvs_handle_t s_nvs;
static bool s_nvs_open;

// Opens the namespace lazily so a transient nvs_open failure is retried at the next save.
static bool nvs_ready(void)
{
    if (s_nvs_open) return true;
    const esp_err_t err = nvs_open(VOCAB_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    s_nvs_open = true;
    return true;
}

static vocab_kv_status_t kv_get(void *ctx, const char *key, void *buf, size_t *len)
{
    (void)ctx;
    if (!nvs_ready()) return VOCAB_KV_ERROR;
    const esp_err_t err = nvs_get_blob(s_nvs, key, buf, len);
    if (err == ESP_OK) return VOCAB_KV_OK;
    if (err == ESP_ERR_NVS_NOT_FOUND) return VOCAB_KV_MISSING;
    ESP_LOGW(TAG, "nvs_get_blob(%s) failed: %s", key, esp_err_to_name(err));
    return VOCAB_KV_ERROR;
}

static bool kv_set(void *ctx, const char *key, const void *buf, size_t len)
{
    (void)ctx;
    if (!nvs_ready()) return false;
    const esp_err_t err = nvs_set_blob(s_nvs, key, buf, len);
    if (err != ESP_OK) ESP_LOGW(TAG, "nvs_set_blob(%s) failed: %s", key, esp_err_to_name(err));
    return err == ESP_OK;
}

static bool kv_erase(void *ctx, const char *key)
{
    (void)ctx;
    if (!nvs_ready()) return false;
    const esp_err_t err = nvs_erase_key(s_nvs, key);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

static bool kv_commit(void *ctx)
{
    (void)ctx;
    if (!nvs_ready()) return false;
    const esp_err_t err = nvs_commit(s_nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static const vocab_kv_ops_t KV_OPS = { kv_get, kv_set, kv_erase, kv_commit };

// Save gate (spec 10.4): writes wait for the audio worker to be idle.
static bool gate_idle(void *ctx)
{
    (void)ctx;
    return vocab_audio_is_idle();
}

static void gate_wait(void *ctx, uint32_t ms)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

static const vocab_audio_gate_t GATE = { gate_idle, gate_wait, NULL };

// Spec 10.4: request STOP, wait up to SAVE_AUDIO_WAIT_MS for the worker's idle flag, then
// write. No audio request is issued meanwhile: this task issues every request itself.
static void save_point(uint32_t kind)
{
    const vocab_session_persist_t p = vocab_engine_persist(&s_engine);
    s_meta.tick = p.tick;
    s_meta.xp = p.xp;
    s_meta.volume_index = p.volume_index;
    s_meta.objective_answers = p.objective_total;
    s_meta.first_try_correct = p.first_try_total;
    vocab_audio_stop();
    const int64_t t0 = esp_timer_get_time();
    const vocab_save_result_t res =
        vocab_store_save(&s_store, s_bank, s_records, &s_meta, &GATE, VOCAB_SAVE_AUDIO_WAIT_MS);
    const int64_t us = esp_timer_get_time() - t0;
    switch (res) {
    case VOCAB_SAVE_OK:
        ESP_LOGI(TAG, "progress saved%s (tick %u, %lld ms)",
                 kind == VOCAB_SAVE_LOW_BATTERY ? " at the low-battery card boundary" : "",
                 (unsigned)p.tick, (long long)(us / 1000));
        break;
    case VOCAB_SAVE_DEFERRED:
        ESP_LOGW(TAG, "audio busy: save deferred");
        break;
    case VOCAB_SAVE_FAILED:
        ESP_LOGE(TAG, "save failed; progress kept in RAM, retried at the next save point");
        break;
    case VOCAB_SAVE_CLEAN:
    case VOCAB_SAVE_VOLATILE:
    default:
        break;
    }
    vocab_engine_storage_warning(&s_engine, vocab_store_warning(&s_store), &s_view);
}

// ---------------------------------------------------------------------------------------
// UI, memory logging, deep sleep
// ---------------------------------------------------------------------------------------

// Prints after the LVGL lock is released; `mon` was captured under the lock.
static void log_memory(const char *where, const lv_mem_monitor_t *mon)
{
    if (mon->free_biggest_size < s_min_biggest_free) s_min_biggest_free = mon->free_biggest_size;
    ESP_LOGI(TAG, "%s: LVGL pool %u/%u B used (%u%%, frag %u%%), biggest free %u B (min %u B); "
                  "heap free %u B (min %u B); app stack free %u B",
             where, (unsigned)(mon->total_size - mon->free_size), (unsigned)mon->total_size,
             (unsigned)mon->used_pct, (unsigned)mon->frag_pct, (unsigned)mon->free_biggest_size,
             (unsigned)s_min_biggest_free, (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void render(void)
{
    if (!s_ui_ok || !bsp_lvgl_lock(LVGL_LOCK_MS)) return;
    vocab_ui_render(&s_view);
    // Measure the LVGL pool once per (screen, stage) combination so the heaviest one shows up.
    const unsigned key = (unsigned)s_view.screen * 4u + (unsigned)s_engine.session.stage_format;
    lv_mem_monitor_t mon;
    const bool first = key < 32u && !(s_screens_logged & (1u << key));
    if (first) {
        s_screens_logged |= 1u << key;
        lv_mem_monitor(&mon);
    }
    bsp_lvgl_unlock();
    if (first) log_memory("screen", &mon);
}

static void set_ui_paused(bool paused)
{
    if (!s_ui_ok || paused == s_ui_paused) return;
    if (bsp_lvgl_lock(LVGL_LOCK_MS)) {
        vocab_ui_set_paused(paused);
        s_ui_paused = paused;
        bsp_lvgl_unlock();
    }
}

// Boot: the LVGL task draws HOME (the panel shows its power-up content until then, so the
// backlight stays off). A refresh runs entirely under the LVGL lock, so the first one that
// completes after render() released the lock includes HOME. Rendering stays in the LVGL
// task and its larger stack.
static void wait_first_frame(void)
{
    if (!s_ui_ok) return;
    const uint32_t start = vocab_ui_frame_count();
    for (int i = 0; i < FIRST_FRAME_POLLS && vocab_ui_frame_count() == start; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void log_step(const char *step, esp_err_t err)
{
    if (err != ESP_OK) ESP_LOGW(TAG, "deep sleep continues: %s failed: %s", step, esp_err_to_name(err));
}

// Spec 9: same order as main/demo_low_power.c sleep_task, GPIO0 low-level key wake only.
static void deep_sleep(void)
{
    const int mv = bsp_button_read_mv();
    if (mv >= 0 && mv < BSP_BTN_MV_RELEASED_MIN) {
        // A held key keeps GPIO0 low: the chip would wake at once. Retry in 5 s.
        ESP_LOGW(TAG, "key held (%d mV): deep sleep postponed", mv);
        vocab_engine_sleep_refused(&s_engine, now_ms());
        return;
    }
    ESP_LOGI(TAG, "entering deep sleep (any key wakes)");
    if (vocab_audio_shutdown(AUDIO_SHUTDOWN_MS) != ESP_OK) {
        ESP_LOGE(TAG, "audio worker did not stop; restarting instead of sleeping");
        esp_restart();
    }
    // Hand the key pin back to digital input + pull-up; terminal for this run.
    int level = 0;
    const esp_err_t err = bsp_button_prepare_deep_sleep(&level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button hand-off failed (%s); restarting", esp_err_to_name(err));
        esp_restart();
    }
    if (level != 1) ESP_LOGW(TAG, "key pressed during the hand-off; the sleep may end at once");
    log_step("GPIO wake arm",
             esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW));
    // CW2017 and ES8311 share I2C: finish the gauge first.
    log_step("CW2017 suspend", bsp_battery_sleep());
    log_step("ES8311 suspend", bsp_audio_sleep());
    log_step("I2S pin release", bsp_audio_prepare_deep_sleep());
    log_step("shared I2C pin release", bsp_i2c_prepare_deep_sleep());
    // Wait for the current flush and keep LVGL from drawing after the LCD is off; the lock
    // is never released in this run.
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "cannot stop LVGL before deep sleep; restarting");
        esp_restart();
    }
    if (s_ui_ok) vocab_ui_set_paused(true);
    log_step("ST7789 suspend", bsp_display_prepare_deep_sleep());
    esp_deep_sleep_start();
    ESP_LOGE(TAG, "esp_deep_sleep_start returned; restarting");
    esp_restart();
}

// ---------------------------------------------------------------------------------------
// Command execution and the task loop
// ---------------------------------------------------------------------------------------

static bool app_has_clip(void *ctx, vocab_uid_t uid)
{
    (void)ctx;
    return vocab_audio_has_clip(uid);
}

static void drain_keys(void)
{
    vocab_raw_event_t raw;
    while (xQueueReceive(s_keys, &raw, 0) == pdTRUE) {
        vocab_engine_key_drained(&s_engine, &raw, now_ms());
    }
}

static void execute(const vocab_outbox_t *out)
{
    if (out->overflow) ESP_LOGE(TAG, "command outbox overflow");
    for (uint8_t i = 0; i < out->count; i++) {
        const vocab_cmd_t *cmd = &out->cmd[i];
        switch (cmd->kind) {
        case VOCAB_CMD_PLAY_WORD:
            (void)vocab_audio_play_word(cmd->uid);
            break;
        case VOCAB_CMD_PLAY_CUE:
            (void)vocab_audio_play_cue(cmd->cue);
            break;
        case VOCAB_CMD_PLAY_CUE_THEN_WORD:
            (void)vocab_audio_play_cue_then_word(cmd->cue, cmd->uid);
            break;
        case VOCAB_CMD_STOP_AUDIO:
            vocab_audio_stop();
            break;
        case VOCAB_CMD_NEW_EPOCH:
            drain_keys();  // spec 3.3 R3 flush, keeping PRESS/RELEASE bookkeeping
            break;
        case VOCAB_CMD_SET_VOLUME:
            vocab_audio_set_volume((uint8_t)cmd->value);
            break;
        case VOCAB_CMD_SAVE:
            save_point(cmd->value);
            break;
        case VOCAB_CMD_BACKLIGHT:
            bsp_display_backlight((uint8_t)cmd->value);
            set_ui_paused(cmd->value == 0);
            break;
        case VOCAB_CMD_READ_BATTERY:
            vocab_engine_battery(&s_engine, bsp_battery_soc(), &s_view);
            break;
        case VOCAB_CMD_DEEP_SLEEP:
            deep_sleep();
            break;
        case VOCAB_CMD_TIMER_START:
        case VOCAB_CMD_TIMER_CANCEL:
        case VOCAB_CMD_NONE:
        default:
            break;
        }
    }
}

static void app_task(void *arg)
{
    (void)arg;
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "fuel gauge unavailable");

    // Storage: never erase automatically (spec 10.5); a failed init runs in volatile mode.
    const esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed (%s): progress will not be saved; reflash to recover",
                 esp_err_to_name(nvs_err));
    }
    s_bank = vocab_bank_builtin();
    vocab_store_init(&s_store, nvs_err == ESP_OK ? &KV_OPS : NULL, NULL);
    const bool loaded = vocab_store_load(&s_store, s_bank, s_records, &s_meta);
    ESP_LOGI(TAG, "%s progress: tick %u, XP %u, volume %u%s", loaded ? "stored" : "fresh",
             (unsigned)s_meta.tick, (unsigned)s_meta.xp, (unsigned)volume_percent(s_meta.volume_index),
             s_store.volatile_mode ? " (volatile)" : "");

    const bool audio_ok = vocab_audio_start(volume_percent(s_meta.volume_index)) == ESP_OK;
    vocab_view_init(&s_view);
    lv_mem_monitor_t mon;
    if (bsp_lvgl_lock(1000)) {
        s_ui_ok = vocab_ui_create();
        lv_mem_monitor(&mon);
        bsp_lvgl_unlock();
        log_memory(s_ui_ok ? "widgets created" : "widget creation failed", &mon);
    }
    if (!s_ui_ok) ESP_LOGE(TAG, "UI creation failed; running without display updates");

    const vocab_engine_config_t cfg = {
        .session = {
            .bank = s_bank,
            // A bank larger than the record array cannot be tracked: BANK_ERROR screen.
            .records = s_bank->count <= VOCAB_RECORDS_MAX ? s_records : NULL,
            .rng = NULL,
            .audio_ok = audio_ok,
            .has_clip = audio_ok ? app_has_clip : NULL,
            .clip_ctx = NULL,
            .tick = s_meta.tick,
            .xp = s_meta.xp,
            .volume_index = s_meta.volume_index,
            .objective_total = s_meta.objective_answers,
            .first_try_total = s_meta.first_try_correct,
            .storage_warning = vocab_store_warning(&s_store),
        },
        .rng_seed = esp_random(),
    };
    vocab_outbox_clear(&s_out);
    vocab_engine_init(&s_engine, &cfg, now_ms(), &s_view, &s_out);
    render();
    wait_first_frame();
    execute(&s_out);   // backlight on, first battery reading
    render();
    ESP_LOGI(TAG, "ready: %u words, audio %s", (unsigned)s_bank->count, audio_ok ? "on" : "off");

    for (;;) {
        TickType_t wait = portMAX_DELAY;
        vocab_ms_t deadline;
        if (vocab_engine_next_deadline(&s_engine, &deadline)) {
            const int32_t delta = (int32_t)(deadline - now_ms());
            wait = delta <= 0 ? 0 : pdMS_TO_TICKS((uint32_t)delta) + 1;
        }
        vocab_raw_event_t raw;
        vocab_outbox_clear(&s_out);
        if (xQueueReceive(s_keys, &raw, wait) == pdTRUE) {
            vocab_engine_key(&s_engine, &raw, now_ms(), &s_view, &s_out);
            execute(&s_out);
            vocab_outbox_clear(&s_out);
        }
        vocab_engine_poll(&s_engine, now_ms(), &s_view, &s_out);
        execute(&s_out);
        render();
    }
}

esp_err_t vocab_app_start(void)
{
    if (s_task) return ESP_OK;
    s_keys = xQueueCreate(VOCAB_INPUT_QUEUE_DEPTH, sizeof(vocab_raw_event_t));
    if (!s_keys) return ESP_ERR_NO_MEM;
    if (xTaskCreate(app_task, "vocab_app", APP_TASK_STACK, NULL, APP_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_keys);
        s_keys = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void vocab_app_post_key(bsp_btn_t btn, bsp_btn_ev_t ev, int64_t time_us)
{
    QueueHandle_t keys = s_keys;
    if (!keys || (int)btn < 0 || (int)btn >= (int)VOCAB_KEY_COUNT || (int)ev < 0 ||
        (int)ev > (int)VOCAB_RAW_RELEASE) {
        return;
    }
    const vocab_raw_event_t raw = {
        .key = (vocab_key_t)btn,
        .kind = (vocab_raw_kind_t)ev,
        .t_ms = (vocab_ms_t)(time_us / 1000),
    };
    (void)xQueueSend(keys, &raw, 0);  // full queue: the new event is dropped (spec 3.1)
}
