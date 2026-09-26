// main/main.c —— GitHub contribution heatmap on the AI Passport.
//
// The main page shows the configured user's contribution calendar, 13 weeks
// per page (UP: older, DOWN: newer), and a bar of the work time left today
// (UTC+8; the clock is set from NTP once Wi-Fi is up). Wi-Fi connects in the
// background; OK opens the "Wi-Fi by sound" setup pages, whose UP/DOWN keys
// come back. Downloaded history is kept in NVS, so it is on screen right after
// power-on; only the rolling year is refreshed once online. After 10 minutes
// without a key press the device powers down (deep sleep); any key turns it
// back on.
//
// Tasks and ownership:
//   - button callbacks (esp_timer task), Wi-Fi/IP event handlers (event loop
//     task), the Wi-Fi timeout timer, the audio worker, the download worker
//     and the NTP client (lwIP task) only post messages to s_queue;
//   - the controller task owns app_flow, wifi_link, the listener lifecycle,
//     the contribution store and view, download scheduling, NVS history, the
//     work-time bar, the power-off sequence, and all UI calls (which take the
//     LVGL lock themselves);
//   - LVGL renders in its own port task.
// Any key press first wakes a dimmed backlight and does nothing else.
#include "app_flow.h"
#include "app_text.h"
#include "app_ui.h"
#include "clock_sync.h"
#include "gh_fetch.h"
#include "hm_nvs.h"
#include "hm_store.h"
#include "hm_ui.h"
#include "hm_view.h"
#include "sonic_listener.h"
#include "wifi_link.h"
#include "wifi_policy.h"
#include "work_bar.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "heatmap";

#define QUEUE_DEPTH          16
#define CONTROLLER_STACK     6144
#define CONTROLLER_PRIORITY  5
#define TICK_MS              100
#define LISTEN_TIMEOUT_MS    90000
#define LISTEN_GRACE_MS      5000       // never time out in the middle of a frame
#define NOTICE_MS            2500
#define BATTERY_PERIOD_MS    30000
#define IDLE_DIM_MS          60000
#define POWER_OFF_IDLE_MS    (10 * 60 * 1000)   // no key press for this long: power down
#define BACKLIGHT_ON         100
#define BACKLIGHT_DIM        12
#define REFRESH_MS           (30 * 60 * 1000)   // the API caches results for an hour
#define FETCH_RETRY_MS       30000              // first retry of a failed download
#define FETCH_RETRY_MAX_MS   (10 * 60 * 1000)
#define YEAR_RETRY_MS        30000
#define WIFI_RETRY_MS        15000              // first background reconnect
#define WIFI_RETRY_MAX_MS    (5 * 60 * 1000)
#define SSID_TEXT_CAP        (SONIC_WIFI_SSID_MAX + 1)
// The work-time bar's clock: the time zone is fixed at UTC+8.
#define LOCAL_UTC_OFFSET_MIN (8 * 60)
#define WORK_START_MIN       (CONFIG_HEATMAP_WORK_START_HOUR * 60)
#define WORK_END_MIN         (CONFIG_HEATMAP_WORK_END_HOUR * 60)

_Static_assert(CONFIG_HEATMAP_WORK_END_HOUR > CONFIG_HEATMAP_WORK_START_HOUR,
               "the work day must end after it starts");

typedef enum {
    MSG_BUTTON = 0,
    MSG_WIFI,
    MSG_LISTENER,
    MSG_FETCH,
    MSG_TIME,
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    union {
        struct {
            bsp_btn_t btn;
            bsp_btn_ev_t event;
        } button;
        wifi_link_msg_t wifi;
        sonic_listener_event_t listener;
        gh_fetch_done_t fetch;
        int64_t utc;                         // MSG_TIME: the time the clock was set to
    };
} app_msg_t;

static QueueHandle_t s_queue;
static app_flow_t s_flow;
static sonic_wifi_credentials_t s_saved;     // what flash holds
static sonic_wifi_credentials_t s_active;    // what is being connected/used
static bool s_battery_ok;
static bool s_listener_stop_pending;
static bool s_dimmed;
static int64_t s_listen_deadline_us;
static int64_t s_damaged_until_us;
static int64_t s_unsupported_until_us;
static int64_t s_last_input_us;
static uint8_t s_wifi_failures;
static int64_t s_wifi_retry_us;

// Contributions (controller task only).
static hm_store_t s_store;
static hm_view_t s_view;
static hm_page_t s_page;                     // scratch, static to spare the stack
static gh_block_t s_block;                   // scratch copy of a download
static uint32_t s_fetch_id;                  // download in flight, 0 when none
static bool s_refreshed;                     // rolling year downloaded since power-on
static int64_t s_last_ok_us;                 // last successful rolling-year download
static int64_t s_last_retry_us;              // earliest next rolling-year attempt
static uint8_t s_last_failures;
static int s_year_failed;                    // calendar year whose download failed
static int64_t s_year_retry_us;
static work_bar_t s_work_bar;                // what the bar shows (zero: off)

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

// Called from the controller task only, so the stack figure is that task's.
static void log_heap(const char *where)
{
    ESP_LOGI(TAG, "heap @%s: free %u, largest block %u, minimum ever %u; controller stack free %u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

// ---------------------------------------------------------------------------
// Producers (other tasks): enqueue only.
// ---------------------------------------------------------------------------

static void post(const app_msg_t *msg, TickType_t wait)
{
    if (s_queue && xQueueSend(s_queue, msg, wait) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full; message %d dropped", (int)msg->kind);
    }
}

static void on_button(bsp_btn_t btn, bsp_btn_ev_t event, void *user)
{
    (void)user;
    const app_msg_t msg = { .kind = MSG_BUTTON, .button = { .btn = btn, .event = event } };
    post(&msg, 0);
}

static void on_wifi(const wifi_link_msg_t *wifi)
{
    const app_msg_t msg = { .kind = MSG_WIFI, .wifi = *wifi };
    post(&msg, 0);
}

static void on_listener(sonic_listener_event_t event)
{
    const app_msg_t msg = { .kind = MSG_LISTENER, .listener = event };
    post(&msg, 0);
}

// The download worker may wait: a lost result would stall downloading.
static void on_fetch(const gh_fetch_done_t *done)
{
    const app_msg_t msg = { .kind = MSG_FETCH, .fetch = *done };
    post(&msg, portMAX_DELAY);
}

// lwIP task: the clock was just set; the next tick would pick it up anyway.
static void on_clock(int64_t utc)
{
    const app_msg_t msg = { .kind = MSG_TIME, .utc = utc };
    post(&msg, 0);
}

// ---------------------------------------------------------------------------
// Rendering (controller task).
// ---------------------------------------------------------------------------

static void active_ssid(char *out)
{
    app_text_ssid(s_active.ssid, s_active.ssid_len, out, SSID_TEXT_CAP);
}

static const char *empty_reason(void)
{
    if (!s_flow.have_saved) return "Wi-Fi is not set up.\nPress OK to set it up.";
    switch (s_flow.link) {
    case APP_LINK_CONNECTING:
        return "Connecting to Wi-Fi...";
    case APP_LINK_UP:
        return s_last_failures ? "Can't reach GitHub.\nTrying again soon."
                               : "Loading contributions...";
    case APP_LINK_WAITING:
    case APP_LINK_OFF:
    default:
        return "Wi-Fi is not connected.\nPress OK to set it up.";
    }
}

static void render_heatmap(void)
{
    hm_view_build(&s_view, &s_store, &s_page);
    hm_ui_set_page(&s_page, empty_reason());
    hm_ui_set_online(s_flow.link == APP_LINK_UP);
}

static void update_connecting(void)
{
    const wifi_link_stage_t stage = wifi_link_stage();
    const uint32_t elapsed = wifi_link_stage_elapsed_ms();
    const char *what = "Connecting...";
    uint32_t lo = 0;
    uint32_t hi = 100;
    uint32_t typical_ms = 1000;
    switch (stage) {
    case WIFI_LINK_STARTING:   what = "Starting Wi-Fi...";     lo = 0;   hi = 120; typical_ms = 800;  break;
    case WIFI_LINK_JOINING:    what = "Joining network...";    lo = 120; hi = 700; typical_ms = 6000; break;
    case WIFI_LINK_GETTING_IP: what = "Getting IP address..."; lo = 700; hi = 980; typical_ms = 3000; break;
    case WIFI_LINK_CONNECTED:  what = "Connected";             lo = hi = 1000; break;
    case WIFI_LINK_IDLE:       break;
    }
    // Eases towards the stage's upper bound; the real result ends the page.
    const uint32_t progress = lo + (hi - lo) * elapsed / (elapsed + typical_ms);
    char line[48];
    if (wifi_link_attempt() > 1 && stage != WIFI_LINK_CONNECTED) {
        snprintf(line, sizeof(line), "%s (try %u of %u)", what, wifi_link_attempt(),
                 wifi_link_attempt_limit());
    } else {
        snprintf(line, sizeof(line), "%s", what);
    }
    app_ui_set_connecting(line, (uint16_t)progress);
}

static void show_failed(void)
{
    const wifi_fail_t fail = wifi_link_failure();
    const uint16_t reason = wifi_link_reason();
    char ssid[SSID_TEXT_CAP];
    active_ssid(ssid);
    char detail[64];
    if (reason) {
        snprintf(detail, sizeof(detail), "%s  |  code %u", ssid, reason);
    } else {
        snprintf(detail, sizeof(detail), "%s", ssid);
    }
    app_ui_show_failed(wifi_policy_title(fail), wifi_policy_hint(fail), detail);
}

static void render(void)
{
    char ssid[SSID_TEXT_CAP];
    switch (s_flow.state) {
    case APP_STATE_HEATMAP:
        hm_ui_show();
        render_heatmap();
        break;
    case APP_STATE_CONNECTING:
        active_ssid(ssid);
        app_ui_show_connecting(ssid);
        update_connecting();
        break;
    case APP_STATE_SETUP:
        app_ui_show_setup(app_flow_note_text(s_flow.note));
        break;
    case APP_STATE_LISTENING:
        app_ui_show_listening();
        break;
    case APP_STATE_FAILED:
        show_failed();
        break;
    case APP_STATE_BOOT:
        break;
    }
}

// ---------------------------------------------------------------------------
// Actions (controller task).
// ---------------------------------------------------------------------------

static void dispatch(app_event_t event);

static void stop_listener(void)
{
    if (sonic_listener_stop() != ESP_OK) s_listener_stop_pending = true;
}

static void start_listener(void)
{
    s_listen_deadline_us = now_us() + (int64_t)LISTEN_TIMEOUT_MS * 1000;
    s_damaged_until_us = 0;
    s_unsupported_until_us = 0;
    log_heap("listen");
    const esp_err_t err = sonic_listener_start(on_listener);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "listening failed to start: %s", esp_err_to_name(err));
        dispatch(APP_EVENT_LISTEN_FAILED);
    }
}

static void save_active(void)
{
    if (wifi_link_save(&s_active) == ESP_OK) {
        s_saved = s_active;
        s_flow.have_saved = true;
        ESP_LOGI(TAG, "credentials saved for the next start");
    } else {
        ESP_LOGW(TAG, "credentials work but could not be saved");
    }
}

static void start_connect(void)
{
    log_heap("connect");
    if (wifi_link_begin(&s_active) == WIFI_LINK_FAILED) {
        dispatch(APP_EVENT_WIFI_FAILED);
    } else if (s_flow.state == APP_STATE_CONNECTING) {
        update_connecting();
    }
}

static void schedule_wifi_retry(void)
{
    if (s_wifi_failures < UINT8_MAX) s_wifi_failures++;
    const uint32_t delay = app_retry_delay_ms(WIFI_RETRY_MS, WIFI_RETRY_MAX_MS, s_wifi_failures);
    s_wifi_retry_us = now_us() + (int64_t)delay * 1000;
    ESP_LOGI(TAG, "Wi-Fi unavailable; next attempt in %u s", (unsigned)(delay / 1000));
}

static void apply(uint32_t actions)
{
    // Choose the credentials before the Connecting page that shows them is drawn.
    if ((actions & APP_ACTION_CONNECT) && s_flow.source == APP_SOURCE_SAVED) s_active = s_saved;
    if (actions & APP_ACTION_LISTEN_STOP) stop_listener();
    if (actions & APP_ACTION_WIFI_STOP) wifi_link_stop();
    if (actions & APP_ACTION_FETCH_CANCEL) gh_fetch_cancel();
    if (actions & APP_ACTION_SAVE) save_active();
    if (actions & APP_ACTION_LISTEN_START) start_listener();
    if (actions & APP_ACTION_RETRY_LATER) schedule_wifi_retry();
    if (actions & APP_ACTION_RENDER) {
        render();
    } else if ((actions & APP_ACTION_LINK) && s_flow.state == APP_STATE_HEATMAP) {
        render_heatmap();
    }
    if (actions & APP_ACTION_CONNECT) start_connect();
}

// Any page change counts as activity: the new page starts bright and gets the
// full idle period, whether a key press or a Wi-Fi/audio event caused it.
static void wake_screen(void)
{
    s_last_input_us = now_us();
    if (s_dimmed) {
        bsp_display_backlight(BACKLIGHT_ON);
        s_dimmed = false;
    }
}

static void dispatch(app_event_t event)
{
    const app_state_t before = s_flow.state;
    const app_link_t link_before = s_flow.link;
    const uint32_t actions = app_flow_handle(&s_flow, event);
    if (!actions) return;
    ESP_LOGI(TAG, "state %d -> %d, link %d -> %d (event %d)", before, s_flow.state, link_before,
             s_flow.link, event);
    if (s_flow.state != before) wake_screen();
    if (s_flow.link == APP_LINK_UP && link_before != APP_LINK_UP) {
        // A fresh connection may download at once, and sets the clock.
        s_wifi_failures = 0;
        s_last_retry_us = 0;
        s_year_retry_us = 0;
        const esp_err_t err = clock_sync_start(on_clock);
        if (err != ESP_OK) ESP_LOGW(TAG, "clock cannot be set: %s", esp_err_to_name(err));
    }
    apply(actions);
}

// ---------------------------------------------------------------------------
// Work-time bar (controller task).
// ---------------------------------------------------------------------------

// Cheap enough for every tick; the screen is touched only when the bar's
// 5-minute step, colour or availability changed.
static void update_work_bar(void)
{
    work_bar_t bar;
    work_bar_compute((int64_t)time(NULL), LOCAL_UTC_OFFSET_MIN, WORK_START_MIN, WORK_END_MIN, &bar);
    if (work_bar_same(&bar, &s_work_bar)) return;
    s_work_bar = bar;
    char text[16];
    work_bar_text(&bar, text, sizeof(text));
    ESP_LOGI(TAG, "work time left: %s", bar.tone == WORK_BAR_OFF ? "clock not set" : text);
    if (bar.tone != WORK_BAR_OFF) ESP_LOGI(TAG, "work bar: %u of %u min", bar.left_min, bar.total_min);
    hm_ui_set_work_bar(&bar);
}

static void handle_time(int64_t utc)
{
    const time_t local = (time_t)(utc + LOCAL_UTC_OFFSET_MIN * 60);
    struct tm tm;
    gmtime_r(&local, &tm);
    ESP_LOGI(TAG, "clock set: %04d-%02d-%02d %02d:%02d:%02d (UTC+8)", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    update_work_bar();
}

// ---------------------------------------------------------------------------
// Contribution downloads (controller task).
// ---------------------------------------------------------------------------

static void start_fetch(gh_fetch_kind_t kind, int year)
{
    const uint32_t id = gh_fetch_start(kind, year);
    if (id) s_fetch_id = id;
}

// One download at a time: the rolling year first (once per power-on, even when
// history was restored from flash, then every REFRESH_MS), then the calendar
// years the current page (plus one page further back) needs.
static void schedule_fetch(int64_t now)
{
    if (s_fetch_id || s_flow.state != APP_STATE_HEATMAP || s_flow.link != APP_LINK_UP) return;
    const bool due = app_refresh_due(s_refreshed, now, s_last_ok_us, (int64_t)REFRESH_MS * 1000);
    if (due && now >= s_last_retry_us) {
        start_fetch(GH_FETCH_LAST, 0);
        return;
    }
    if (!s_store.have_last) return;   // older years need "today" first
    int32_t from;
    int32_t to;
    hm_view_wanted_days(&s_view, &from, &to);
    const int year = hm_store_missing_year(&s_store, from, to);
    if (year && !(year == s_year_failed && now < s_year_retry_us)) start_fetch(GH_FETCH_YEAR, year);
}

static void handle_fetch(const gh_fetch_done_t *done)
{
    if (done->id != s_fetch_id) return;
    s_fetch_id = 0;
    const int64_t now = now_us();
    const bool ok = done->status == GH_FETCH_OK && gh_fetch_take(done->id, &s_block);
    if (done->kind == GH_FETCH_LAST) {
        // Flash is written only when the calendar actually changed.
        const bool changed = !s_store.have_last || !hm_block_same(&s_store.last, &s_block);
        if (ok && hm_store_set_last(&s_store, &s_block)) {
            s_refreshed = true;
            s_last_ok_us = now;
            s_last_failures = 0;
            if (changed) {
                const esp_err_t err = hm_nvs_save_last(&s_store, CONFIG_HEATMAP_GITHUB_USER);
                if (err != ESP_OK) ESP_LOGW(TAG, "rolling year not saved: %s", esp_err_to_name(err));
            }
            log_heap("data");
        } else if (done->status != GH_FETCH_CANCELLED) {
            if (s_last_failures < UINT8_MAX) s_last_failures++;
            const uint32_t delay = app_retry_delay_ms(FETCH_RETRY_MS, FETCH_RETRY_MAX_MS, s_last_failures);
            s_last_retry_us = now + (int64_t)delay * 1000;
            ESP_LOGW(TAG, "contributions unavailable; retry in %u s", (unsigned)(delay / 1000));
        }
    } else if (ok && hm_store_set_year(&s_store, done->year, &s_block)) {
        if (s_year_failed == done->year) s_year_failed = 0;
        const esp_err_t err = hm_nvs_save_year(&s_store, CONFIG_HEATMAP_GITHUB_USER, done->year);
        if (err != ESP_OK) ESP_LOGW(TAG, "year %d not saved: %s", done->year, esp_err_to_name(err));
    } else if (done->status != GH_FETCH_CANCELLED) {
        s_year_failed = done->year;
        s_year_retry_us = now + (int64_t)YEAR_RETRY_MS * 1000;
    }
    hm_view_sync(&s_view, &s_store);
    if (s_flow.state == APP_STATE_HEATMAP) render_heatmap();
}

// ---------------------------------------------------------------------------
// Message handling and periodic work (controller task).
// ---------------------------------------------------------------------------

static void handle_button(bsp_btn_t btn, bsp_btn_ev_t event)
{
    // Any key activity, even a long hold, keeps the device on.
    s_last_input_us = now_us();
    // Two quick presses arrive as one DOUBLE event instead of two clicks.
    if (event != BSP_BTN_CLICK && event != BSP_BTN_DOUBLE) return;
    if (s_dimmed) {
        // The press that wakes the screen does not also trigger the key.
        bsp_display_backlight(BACKLIGHT_ON);
        s_dimmed = false;
        return;
    }
    if (btn == BSP_BTN_OK) {
        dispatch(APP_EVENT_OK);
        return;
    }
    if (s_flow.state != APP_STATE_HEATMAP) {
        dispatch(APP_EVENT_BACK);
        return;
    }
    const int direction = btn == BSP_BTN_UP ? -1 : 1;
    bool moved = hm_view_scroll(&s_view, &s_store, direction);
    if (event == BSP_BTN_DOUBLE) moved |= hm_view_scroll(&s_view, &s_store, direction);
    if (moved) render_heatmap();
}

static void handle_wifi(const wifi_link_msg_t *msg)
{
    switch (wifi_link_handle(msg)) {
    case WIFI_LINK_PROGRESS:
        if (s_flow.state == APP_STATE_CONNECTING) update_connecting();
        break;
    case WIFI_LINK_UP:
        dispatch(APP_EVENT_WIFI_CONNECTED);
        break;
    case WIFI_LINK_FAILED:
        dispatch(APP_EVENT_WIFI_FAILED);
        break;
    case WIFI_LINK_LOST:
        dispatch(APP_EVENT_WIFI_LOST);
        break;
    case WIFI_LINK_INFO_CHANGED:
    case WIFI_LINK_NONE:
        break;
    }
}

static void handle_listener(sonic_listener_event_t event)
{
    if (s_flow.state != APP_STATE_LISTENING) return;
    switch (event) {
    case SONIC_LISTENER_EVT_CREDENTIALS:
        if (sonic_listener_take(&s_active)) dispatch(APP_EVENT_CREDENTIALS);
        break;
    case SONIC_LISTENER_EVT_DAMAGED:
        s_damaged_until_us = now_us() + (int64_t)NOTICE_MS * 1000;
        break;
    case SONIC_LISTENER_EVT_UNSUPPORTED:
        s_unsupported_until_us = now_us() + (int64_t)NOTICE_MS * 1000;
        break;
    case SONIC_LISTENER_EVT_AUDIO_ERROR:
        dispatch(APP_EVENT_LISTEN_FAILED);
        break;
    case SONIC_LISTENER_EVT_RECEIVING:
        break;
    }
}

static void update_listening(int64_t now)
{
    sonic_listener_status_t status;
    sonic_listener_status(&status);
    const int64_t grace = now + (int64_t)LISTEN_GRACE_MS * 1000;
    if (status.receiving && s_listen_deadline_us < grace) s_listen_deadline_us = grace;
    if (now >= s_listen_deadline_us) {
        dispatch(APP_EVENT_LISTEN_TIMEOUT);
        return;
    }
    char message[48];
    if (status.clipping) {
        snprintf(message, sizeof(message), "Too loud - lower the PC volume");
    } else if (now < s_damaged_until_us) {
        snprintf(message, sizeof(message), "Signal damaged - keep playing");
    } else if (now < s_unsupported_until_us) {
        snprintf(message, sizeof(message), "That sound was not Wi-Fi setup");
    } else if (status.receiving) {
        snprintf(message, sizeof(message), "Receiving %u%%", (unsigned)(status.progress / 10));
    } else {
        snprintf(message, sizeof(message), "Play the setup sound on your PC");
    }
    const app_ui_listening_t ui = {
        .level = status.level,
        .clipping = status.clipping,
        .receiving = status.receiving,
        .progress = status.progress,
        .message = message,
        .seconds_left = (int)((s_listen_deadline_us - now + 999999) / 1000000),
    };
    app_ui_set_listening(&ui);
}

static void log_power_step(const char *step, esp_err_t err)
{
    if (err != ESP_OK) ESP_LOGW(TAG, "power off continues: %s failed: %s", step, esp_err_to_name(err));
}

// "Power off": the firmware cannot disconnect the battery (the hardware power
// button does that), so the device enters deep sleep with its peripherals shut
// down and wakes, i.e. restarts, when any of the three keys is pressed. The
// history is already in NVS. Terminal: it never returns.
static void power_off(void)
{
    ESP_LOGI(TAG, "no key pressed for %d min: powering off until a key is pressed",
             POWER_OFF_IDLE_MS / 60000);
    // Long-lived radio work first; its tasks stop for good once the chip sleeps.
    gh_fetch_cancel();
    stop_listener();
    wifi_link_stop();
    // Never sleep without a key wake. A key held at this moment, or a driver
    // that cannot be released, restarts the device instead (as a wake would).
    const esp_err_t wake = bsp_button_prepare_deep_sleep();
    if (wake != ESP_OK) {
        ESP_LOGW(TAG, "key wake not armed (%s); restarting instead", esp_err_to_name(wake));
        esp_restart();
    }
    // BSP deep-sleep contract: shared-bus devices, then their pins, then the panel.
    log_power_step("CW2017 suspend", bsp_battery_sleep());
    log_power_step("ES8311 suspend", bsp_audio_sleep());
    log_power_step("I2S pin release", bsp_audio_prepare_deep_sleep());
    log_power_step("shared I2C pin release", bsp_i2c_prepare_deep_sleep());
    // Keep the LVGL lock so no flush reaches the panel after it sleeps.
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "cannot stop LVGL before the panel sleeps; restarting");
        esp_restart();
    }
    log_power_step("ST7789 suspend", bsp_display_prepare_deep_sleep());
    esp_deep_sleep_start();
    // The buses cannot be resumed in this run after the steps above.
    esp_restart();
}

static void tick(void)
{
    static int64_t last_battery_us;
    const int64_t now = now_us();

    if (s_listener_stop_pending && sonic_listener_stop() == ESP_OK) s_listener_stop_pending = false;

    switch (s_flow.state) {
    case APP_STATE_LISTENING:
        update_listening(now);
        break;
    case APP_STATE_CONNECTING:
        update_connecting();
        break;
    case APP_STATE_HEATMAP:
        if (s_flow.link == APP_LINK_WAITING && now >= s_wifi_retry_us) dispatch(APP_EVENT_RETRY);
        break;
    default:
        break;
    }
    schedule_fetch(now);
    update_work_bar();

    // The battery level is shown on the Wi-Fi pages only; the heatmap page
    // keeps its top edge free.
    if (s_battery_ok && (last_battery_us == 0 || now - last_battery_us >= (int64_t)BATTERY_PERIOD_MS * 1000)) {
        last_battery_us = now;
        app_ui_set_battery(bsp_battery_soc());
    }

    // Dim only on pages that wait for the user; listening/connecting stay bright.
    const bool waiting = s_flow.state == APP_STATE_HEATMAP || s_flow.state == APP_STATE_SETUP ||
                         s_flow.state == APP_STATE_FAILED;
    const int64_t idle_us = now - s_last_input_us;
    if (waiting && idle_us >= (int64_t)POWER_OFF_IDLE_MS * 1000) power_off();
    if (!s_dimmed && waiting && idle_us >= (int64_t)IDLE_DIM_MS * 1000) {
        bsp_display_backlight(BACKLIGHT_DIM);
        s_dimmed = true;
        if (s_flow.state == APP_STATE_HEATMAP && !s_view.follow_today) {
            // Unattended, the page returns to the current weeks.
            hm_view_to_today(&s_view, &s_store);
            render_heatmap();
        }
    }
}

static void controller_task(void *arg)
{
    (void)arg;
    if (wifi_link_init(on_wifi) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi unavailable; connection attempts will report an error");
    }
    const bool have_saved = wifi_link_load(&s_saved);
    ESP_LOGI(TAG, "saved Wi-Fi: %s", have_saved ? "yes" : "no");
    hm_store_init(&s_store);
    hm_view_init(&s_view);
    // The stored history is on screen from the first frame; only the rolling
    // year is refreshed once Wi-Fi connects.
    if (hm_nvs_load(&s_store, CONFIG_HEATMAP_GITHUB_USER)) hm_view_sync(&s_view, &s_store);
    log_heap("ready");
    s_last_input_us = now_us();
    app_flow_init(&s_flow);
    dispatch(have_saved ? APP_EVENT_BOOT_SAVED : APP_EVENT_BOOT_EMPTY);

    int64_t next_tick = now_us();
    for (;;) {
        const int64_t wait_us = next_tick - now_us();
        const TickType_t wait = wait_us > 0 ? pdMS_TO_TICKS(wait_us / 1000) : 0;
        app_msg_t msg;
        if (xQueueReceive(s_queue, &msg, wait) == pdTRUE) {
            switch (msg.kind) {
            case MSG_BUTTON:   handle_button(msg.button.btn, msg.button.event); break;
            case MSG_WIFI:     handle_wifi(&msg.wifi); break;
            case MSG_LISTENER: handle_listener(msg.listener); break;
            case MSG_FETCH:    handle_fetch(&msg.fetch); break;
            case MSG_TIME:     handle_time(msg.utc); break;
            }
        }
        if (now_us() >= next_tick) {
            next_tick = now_us() + (int64_t)TICK_MS * 1000;
            tick();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "GitHub contribution heatmap starting (%s)",
             esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO ? "woken by a key" : "power-on or reset");
    bsp_i2c_init();
    // The display is required: without it there is nothing to show.
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init() || !app_ui_init() || !hm_ui_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(BACKLIGHT_ON);

    s_battery_ok = bsp_battery_init() == ESP_OK;
    if (bsp_audio_init() == ESP_OK) {
        // Keep the codec and microphone bias off until listening starts.
        const esp_err_t err = bsp_audio_sleep();
        if (err != ESP_OK) ESP_LOGW(TAG, "codec sleep failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "audio init failed; listening will report a microphone error");
    }

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(app_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "cannot create the event queue");
        return;
    }
    const esp_err_t fetch_err = gh_fetch_init(CONFIG_HEATMAP_GITHUB_USER, on_fetch);
    if (fetch_err != ESP_OK) {
        ESP_LOGE(TAG, "download worker unavailable: %s", esp_err_to_name(fetch_err));
    }
    if (xTaskCreate(controller_task, "app_ctrl", CONTROLLER_STACK, NULL, CONTROLLER_PRIORITY,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the controller task");
        return;
    }
    const esp_err_t err = bsp_button_init(on_button, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(err));
}
