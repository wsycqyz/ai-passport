// main/main.c —— "Wi-Fi by sound": connect to saved Wi-Fi, or receive new
// credentials from a PC speaker (tools/sonic_link.py) through the microphone.
//
// Tasks and ownership:
//   - button callbacks (esp_timer task), Wi-Fi/IP event handlers (event loop
//     task), the Wi-Fi timeout timer and the audio worker only post messages
//     to s_queue;
//   - the controller task owns app_flow, wifi_link, the listener lifecycle and
//     all UI calls (which take the LVGL lock themselves);
//   - LVGL renders in its own port task.
// The only on-screen button of each page is activated with the OK key; any key
// press first wakes a dimmed backlight.
#include "app_flow.h"
#include "app_text.h"
#include "app_ui.h"
#include "sonic_listener.h"
#include "wifi_link.h"
#include "wifi_policy.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "sound_wifi";

#define QUEUE_DEPTH          16
#define CONTROLLER_STACK     6144
#define CONTROLLER_PRIORITY  5
#define TICK_MS              100
#define LISTEN_TIMEOUT_MS    90000
#define LISTEN_GRACE_MS      5000       // never time out in the middle of a frame
#define NOTICE_MS            2500
#define BATTERY_PERIOD_MS    30000
#define SIGNAL_PERIOD_MS     5000
#define IDLE_DIM_MS          60000
#define BACKLIGHT_ON         100
#define BACKLIGHT_DIM        12
#define SSID_TEXT_CAP        (SONIC_WIFI_SSID_MAX + 1)

typedef enum {
    MSG_BUTTON = 0,
    MSG_WIFI,
    MSG_LISTENER,
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
    };
} app_msg_t;

static QueueHandle_t s_queue;
static app_flow_t s_flow;
static sonic_wifi_credentials_t s_saved;     // what flash holds
static sonic_wifi_credentials_t s_active;    // what is being connected/used
static bool s_have_saved;
static bool s_battery_ok;
static bool s_saved_now;
static bool s_save_failed;
static bool s_listener_stop_pending;
static bool s_dimmed;
static int64_t s_listen_deadline_us;
static int64_t s_damaged_until_us;
static int64_t s_unsupported_until_us;
static int64_t s_last_input_us;

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

static void post(const app_msg_t *msg)
{
    if (s_queue && xQueueSend(s_queue, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full; message %d dropped", (int)msg->kind);
    }
}

static void on_button(bsp_btn_t btn, bsp_btn_ev_t event, void *user)
{
    (void)user;
    const app_msg_t msg = { .kind = MSG_BUTTON, .button = { .btn = btn, .event = event } };
    post(&msg);
}

static void on_wifi(const wifi_link_msg_t *wifi)
{
    const app_msg_t msg = { .kind = MSG_WIFI, .wifi = *wifi };
    post(&msg);
}

static void on_listener(sonic_listener_event_t event)
{
    const app_msg_t msg = { .kind = MSG_LISTENER, .listener = event };
    post(&msg);
}

// ---------------------------------------------------------------------------
// Rendering (controller task).
// ---------------------------------------------------------------------------

static void active_ssid(char *out)
{
    app_text_ssid(s_active.ssid, s_active.ssid_len, out, SSID_TEXT_CAP);
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

static void show_connected(void)
{
    wifi_link_info_t info = { 0 };
    if (!wifi_link_info(&info)) strcpy(info.ip, "-");
    char ssid[SSID_TEXT_CAP];
    active_ssid(ssid);
    const app_ui_connected_t ui = {
        .ssid = ssid,
        .ip = info.ip,
        .netmask = info.netmask[0] ? info.netmask : "-",
        .gateway = info.gateway[0] ? info.gateway : "-",
        .dns = info.dns[0] ? info.dns : "-",
        .rssi = info.rssi,
        .channel = info.channel,
        .footnote = s_save_failed ? "Could not save for next start"
                  : (s_saved_now ? "Saved for next start" : NULL),
        .footnote_warning = s_save_failed,
    };
    app_ui_show_connected(&ui);
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
    case APP_STATE_CONNECTED:
        show_connected();
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
    s_saved_now = false;
    s_save_failed = wifi_link_save(&s_active) != ESP_OK;
    if (!s_save_failed) {
        s_saved = s_active;
        s_have_saved = true;
        s_saved_now = true;
        ESP_LOGI(TAG, "credentials saved for the next start");
    }
}

static void start_connect(void)
{
    s_saved_now = false;
    s_save_failed = false;
    log_heap("connect");
    if (wifi_link_begin(&s_active) == WIFI_LINK_FAILED) {
        dispatch(APP_EVENT_WIFI_FAILED);
    } else {
        update_connecting();
    }
}

static void apply(uint32_t actions)
{
    // Choose the credentials before the Connecting page that shows them is drawn.
    if ((actions & APP_ACTION_CONNECT) && s_flow.source == APP_SOURCE_SAVED) s_active = s_saved;
    if (actions & APP_ACTION_LISTEN_STOP) stop_listener();
    if (actions & APP_ACTION_WIFI_STOP) wifi_link_stop();
    if (actions & APP_ACTION_SAVE) save_active();
    if (actions & APP_ACTION_LISTEN_START) start_listener();
    if (actions & APP_ACTION_RENDER) render();
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
    const uint32_t actions = app_flow_handle(&s_flow, event);
    if (!actions) return;
    ESP_LOGI(TAG, "state %d -> %d (event %d)", before, s_flow.state, event);
    if (s_flow.state != before) wake_screen();
    apply(actions);
}

// ---------------------------------------------------------------------------
// Message handling and periodic work (controller task).
// ---------------------------------------------------------------------------

static void handle_button(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event != BSP_BTN_CLICK) return;
    s_last_input_us = now_us();
    if (s_dimmed) {
        // The press that wakes the screen does not also trigger the button.
        bsp_display_backlight(BACKLIGHT_ON);
        s_dimmed = false;
        return;
    }
    if (btn == BSP_BTN_OK) dispatch(APP_EVENT_BUTTON);
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
        if (s_flow.state == APP_STATE_CONNECTED) show_connected();
        break;
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

static void tick(void)
{
    static int64_t last_battery_us;
    static int64_t last_signal_us;
    const int64_t now = now_us();

    if (s_listener_stop_pending && sonic_listener_stop() == ESP_OK) s_listener_stop_pending = false;

    switch (s_flow.state) {
    case APP_STATE_LISTENING:
        update_listening(now);
        break;
    case APP_STATE_CONNECTING:
        update_connecting();
        break;
    case APP_STATE_CONNECTED:
        if (now - last_signal_us >= (int64_t)SIGNAL_PERIOD_MS * 1000) {
            last_signal_us = now;
            wifi_link_info_t info;
            wifi_link_refresh_signal();
            if (wifi_link_info(&info)) app_ui_set_signal(info.rssi, info.channel);
        }
        break;
    default:
        break;
    }

    if (s_battery_ok && (last_battery_us == 0 || now - last_battery_us >= (int64_t)BATTERY_PERIOD_MS * 1000)) {
        last_battery_us = now;
        app_ui_set_battery(bsp_battery_soc());
    }

    // Dim only on pages that wait for the user; listening/connecting stay bright.
    const bool waiting = s_flow.state == APP_STATE_SETUP || s_flow.state == APP_STATE_CONNECTED ||
                         s_flow.state == APP_STATE_FAILED;
    if (!s_dimmed && waiting && now - s_last_input_us >= (int64_t)IDLE_DIM_MS * 1000) {
        bsp_display_backlight(BACKLIGHT_DIM);
        s_dimmed = true;
    }
}

static void controller_task(void *arg)
{
    (void)arg;
    if (wifi_link_init(on_wifi) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi unavailable; connection attempts will report an error");
    }
    s_have_saved = wifi_link_load(&s_saved);
    ESP_LOGI(TAG, "saved Wi-Fi: %s", s_have_saved ? "yes" : "no");
    log_heap("ready");
    s_last_input_us = now_us();
    app_flow_init(&s_flow);
    dispatch(s_have_saved ? APP_EVENT_BOOT_SAVED : APP_EVENT_BOOT_EMPTY);

    int64_t next_tick = now_us();
    for (;;) {
        const int64_t wait_us = next_tick - now_us();
        const TickType_t wait = wait_us > 0 ? pdMS_TO_TICKS(wait_us / 1000) : 0;
        app_msg_t msg;
        if (xQueueReceive(s_queue, &msg, wait) == pdTRUE) {
            if (msg.kind == MSG_BUTTON) handle_button(msg.button.btn, msg.button.event);
            else if (msg.kind == MSG_WIFI) handle_wifi(&msg.wifi);
            else handle_listener(msg.listener);
        }
        if (now_us() >= next_tick) {
            next_tick = now_us() + (int64_t)TICK_MS * 1000;
            tick();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Wi-Fi by sound starting");
    bsp_i2c_init();
    // The display is required: without it there is no way to show progress.
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init() || !app_ui_init()) {
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
    if (!s_queue || xTaskCreate(controller_task, "app_ctrl", CONTROLLER_STACK, NULL,
                                CONTROLLER_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the controller task");
        return;
    }
    const esp_err_t err = bsp_button_init(on_button, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(err));
}
