// main/wifi_link.c —— see wifi_link.h for the threading and attempt model.
#include "wifi_link.h"

#include "demo_radio.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "wifi_link";

#define NVS_NAMESPACE     "sonic_wifi"
#define NVS_KEY           "sta"
#define RECORD_VERSION    1
#define START_TIMEOUT_MS  5000
// Covers an all-channel scan plus a WPA handshake that a slow AP retries.
#define JOIN_TIMEOUT_MS   20000
#define DHCP_TIMEOUT_MS   15000
// No new attempt starts after this much time since wifi_link_begin().
#define TOTAL_BUDGET_MS   60000
#define HOSTNAME          "ai-passport"

typedef struct {
    uint8_t version;
    uint8_t ssid_len;
    uint8_t password_len;
    uint8_t reserved;
    uint8_t ssid[SONIC_WIFI_SSID_MAX];
    uint8_t password[SONIC_WIFI_PASSWORD_MAX];
} stored_record_t;

static wifi_link_post_t s_post;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_timer;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_driver_ready;
static bool s_wifi_handler_on;
static bool s_ip_handler_on;
static bool s_ready;
static bool s_started;
static wifi_link_stage_t s_stage;
static uint32_t s_token;
static volatile uint32_t s_armed_token;
static uint8_t s_attempt;
static uint8_t s_limit;
static int64_t s_begin_us;
static int64_t s_stage_us;
static wifi_fail_t s_failure;
static uint16_t s_reason;
static wifi_link_info_t s_info;
static bool s_have_info;

static void wipe(void *data, size_t len)
{
    volatile uint8_t *p = data;
    while (len--) *p++ = 0;
}

static void forward(wifi_link_msg_type_t type, uint16_t reason, uint32_t token)
{
    const wifi_link_post_t post = s_post;
    if (!post) return;
    const wifi_link_msg_t msg = { .type = type, .reason = reason, .token = token };
    post(&msg);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        forward(WIFI_LINK_MSG_STA_START, 0, 0);
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        forward(WIFI_LINK_MSG_STA_CONNECTED, 0, 0);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        forward(WIFI_LINK_MSG_STA_DISCONNECTED, event ? event->reason : 0, 0);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_STA_GOT_IP) forward(WIFI_LINK_MSG_GOT_IP, 0, 0);
}

static void on_timeout(void *arg)
{
    (void)arg;
    forward(WIFI_LINK_MSG_TIMEOUT, 0, s_armed_token);
}

static void release_driver(void)
{
    if (s_ip_handler_on) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler_on = false;
    }
    if (s_wifi_handler_on) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler_on = false;
    }
    if (s_driver_ready) {
        esp_wifi_deinit();
        s_driver_ready = false;
    }
    if (s_netif) {
        // Also clears a partially attached driver registration.
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
}

esp_err_t wifi_link_init(wifi_link_post_t post)
{
    if (s_ready) return ESP_OK;
    s_post = post;
    esp_err_t err = demo_radio_nvs_prepare();
    if (err == ESP_OK) err = demo_radio_network_prepare();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS/network services unavailable: %s", esp_err_to_name(err));
        return err;
    }
    if (!s_timer) {
        const esp_timer_create_args_t args = { .callback = on_timeout, .name = "wifi_link" };
        err = esp_timer_create(&args, &s_timer);
        if (err != ESP_OK) return err;
    }

    // Checked steps instead of esp_netif_create_default_wifi_sta(), which
    // aborts on allocation failure.
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    s_netif = esp_netif_new(&netif_cfg);
    if (!s_netif) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = esp_netif_attach_wifi_station(s_netif);
    if (err == ESP_OK) err = esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK) goto fail;
    (void)esp_netif_set_hostname(s_netif, HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) goto fail;
    s_driver_ready = true;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event,
                                              NULL, &s_wifi_handler);
    if (err != ESP_OK) goto fail;
    s_wifi_handler_on = true;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event,
                                              NULL, &s_ip_handler);
    if (err != ESP_OK) goto fail;
    s_ip_handler_on = true;
    // Never let the driver persist untested credentials on its own.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto fail;
    s_ready = true;
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
    release_driver();
    return err;
}

bool wifi_link_load(sonic_wifi_credentials_t *out)
{
    sonic_wifi_wipe(out);
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    stored_record_t record;
    size_t len = sizeof(record);
    const esp_err_t err = nvs_get_blob(handle, NVS_KEY, &record, &len);
    nvs_close(handle);
    const bool ok = err == ESP_OK && len == sizeof(record) && record.version == RECORD_VERSION &&
                    record.ssid_len >= 1 && record.ssid_len <= SONIC_WIFI_SSID_MAX &&
                    memchr(record.ssid, 0, record.ssid_len) == NULL &&
                    record.password_len <= SONIC_WIFI_PASSWORD_MAX &&
                    sonic_wifi_password_valid(record.password, record.password_len);
    if (ok) {
        memcpy(out->ssid, record.ssid, record.ssid_len);
        out->ssid_len = record.ssid_len;
        memcpy(out->password, record.password, record.password_len);
        out->password_len = record.password_len;
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "ignoring an invalid saved Wi-Fi record");
    }
    wipe(&record, sizeof(record));
    return ok;
}

esp_err_t wifi_link_save(const sonic_wifi_credentials_t *creds)
{
    stored_record_t record = { .version = RECORD_VERSION };
    record.ssid_len = creds->ssid_len;
    record.password_len = creds->password_len;
    memcpy(record.ssid, creds->ssid, creds->ssid_len);
    memcpy(record.password, creds->password, creds->password_len);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_KEY, &record, sizeof(record));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    wipe(&record, sizeof(record));
    if (err != ESP_OK) ESP_LOGE(TAG, "saving Wi-Fi credentials failed: %s", esp_err_to_name(err));
    return err;
}

static void set_stage(wifi_link_stage_t stage)
{
    s_stage = stage;
    s_stage_us = esp_timer_get_time();
}

static void arm(uint32_t ms)
{
    esp_timer_stop(s_timer);
    // A fresh token per arm: a timeout already queued for the previous stage
    // (e.g. a join timeout racing STA_CONNECTED) must not count for this one.
    s_armed_token = ++s_token;
    esp_timer_start_once(s_timer, (uint64_t)ms * 1000u);
}

static void disarm(void)
{
    if (s_timer) esp_timer_stop(s_timer);
}

static void radio_off(void)
{
    if (!s_started) return;
    const esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    s_started = false;
}

static wifi_link_result_t give_up(wifi_fail_t fail, uint16_t reason)
{
    disarm();
    s_token++;
    radio_off();
    s_failure = fail;
    s_reason = reason;
    s_have_info = false;
    set_stage(WIFI_LINK_IDLE);
    ESP_LOGW(TAG, "giving up: %s (reason %u) after %u attempt(s)",
             wifi_policy_title(fail), reason, s_attempt);
    return WIFI_LINK_FAILED;
}

static wifi_link_result_t start_attempt(void)
{
    s_token++;
    // A full restart makes every attempt begin from a known driver state.
    radio_off();
    const esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return give_up(WIFI_FAIL_DRIVER, 0);
    }
    s_started = true;
    set_stage(WIFI_LINK_STARTING);
    arm(START_TIMEOUT_MS);
    return WIFI_LINK_PROGRESS;
}

static wifi_link_result_t attempt_failed(wifi_fail_t fail, uint16_t reason)
{
    disarm();
    s_failure = fail;
    s_reason = reason;
    s_limit = wifi_policy_attempts(fail);
    const int64_t elapsed_ms = (esp_timer_get_time() - s_begin_us) / 1000;
    ESP_LOGW(TAG, "attempt %u/%u failed: %s (reason %u)", s_attempt, s_limit,
             wifi_policy_title(fail), reason);
    if (s_attempt < s_limit && elapsed_ms < TOTAL_BUDGET_MS) {
        s_attempt++;
        return start_attempt();
    }
    return give_up(fail, reason);
}

static void read_info(void)
{
    wifi_link_info_t info = { 0 };
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, info.ip, sizeof(info.ip));
        esp_ip4addr_ntoa(&ip.netmask, info.netmask, sizeof(info.netmask));
        esp_ip4addr_ntoa(&ip.gw, info.gateway, sizeof(info.gateway));
    }
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
        esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, info.dns, sizeof(info.dns));
    } else {
        strcpy(info.dns, "-");
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        info.rssi = ap.rssi;
        info.channel = ap.primary;
    }
    s_info = info;
    s_have_info = true;
}

wifi_link_result_t wifi_link_begin(const sonic_wifi_credentials_t *creds)
{
    wifi_link_stop();
    s_failure = WIFI_FAIL_NONE;
    s_reason = 0;
    s_attempt = 1;
    s_limit = wifi_policy_attempts(WIFI_FAIL_UNKNOWN);
    s_begin_us = esp_timer_get_time();
    if (!s_ready) return give_up(WIFI_FAIL_DRIVER, 0);

    wifi_config_t cfg = { 0 };
    memcpy(cfg.sta.ssid, creds->ssid, creds->ssid_len);
    memcpy(cfg.sta.password, creds->password, creds->password_len);
    // With a password, accept WEP and stronger; without an explicit threshold
    // the driver assumes WPA2 and refuses WPA/WEP networks. Open APs with the
    // same name are still rejected because OPEN is weaker than WEP.
    cfg.sta.threshold.authmode = creds->password_len ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    wipe(&cfg, sizeof(cfg));
    if (err == ESP_ERR_WIFI_PASSWORD) return give_up(WIFI_FAIL_BAD_PASSWORD, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        return give_up(WIFI_FAIL_DRIVER, 0);
    }
    // SSIDs are not secret; the password is never logged.
    ESP_LOGI(TAG, "connecting to \"%.*s\" (password: %u chars)",
             creds->ssid_len, (const char *)creds->ssid, creds->password_len);
    return start_attempt();
}

wifi_link_result_t wifi_link_handle(const wifi_link_msg_t *msg)
{
    if (s_stage == WIFI_LINK_IDLE) return WIFI_LINK_NONE;
    switch (msg->type) {
    case WIFI_LINK_MSG_STA_START: {
        if (s_stage != WIFI_LINK_STARTING) return WIFI_LINK_NONE;
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
            return attempt_failed(WIFI_FAIL_DRIVER, 0);
        }
        set_stage(WIFI_LINK_JOINING);
        arm(JOIN_TIMEOUT_MS);
        return WIFI_LINK_PROGRESS;
    }
    case WIFI_LINK_MSG_STA_CONNECTED:
        if (s_stage != WIFI_LINK_JOINING) return WIFI_LINK_NONE;
        set_stage(WIFI_LINK_GETTING_IP);
        arm(DHCP_TIMEOUT_MS);
        return WIFI_LINK_PROGRESS;
    case WIFI_LINK_MSG_GOT_IP:
        if (s_stage == WIFI_LINK_STARTING) return WIFI_LINK_NONE;
        read_info();
        if (s_stage == WIFI_LINK_CONNECTED) return WIFI_LINK_INFO_CHANGED;
        disarm();
        s_token++;
        s_failure = WIFI_FAIL_NONE;
        s_reason = 0;
        set_stage(WIFI_LINK_CONNECTED);
        ESP_LOGI(TAG, "connected: ip %s gateway %s rssi %d ch %u",
                 s_info.ip, s_info.gateway, s_info.rssi, s_info.channel);
        return WIFI_LINK_UP;
    case WIFI_LINK_MSG_STA_DISCONNECTED:
        // A disconnect while restarting belongs to the previous attempt.
        if (s_stage == WIFI_LINK_STARTING) return WIFI_LINK_NONE;
        if (s_stage == WIFI_LINK_CONNECTED) {
            ESP_LOGW(TAG, "connection lost (reason %u)", msg->reason);
            disarm();
            s_token++;
            s_reason = msg->reason;
            s_have_info = false;
            set_stage(WIFI_LINK_IDLE);
            return WIFI_LINK_LOST;
        }
        return attempt_failed(wifi_policy_classify(msg->reason), msg->reason);
    case WIFI_LINK_MSG_TIMEOUT:
        if (msg->token != s_token) return WIFI_LINK_NONE;
        if (s_stage == WIFI_LINK_STARTING) return attempt_failed(WIFI_FAIL_DRIVER, 0);
        if (s_stage == WIFI_LINK_JOINING) return attempt_failed(WIFI_FAIL_TIMEOUT, 0);
        if (s_stage == WIFI_LINK_GETTING_IP) return attempt_failed(WIFI_FAIL_NO_IP, 0);
        return WIFI_LINK_NONE;
    }
    return WIFI_LINK_NONE;
}

void wifi_link_stop(void)
{
    disarm();
    s_token++;
    radio_off();
    s_have_info = false;
    set_stage(WIFI_LINK_IDLE);
}

wifi_link_stage_t wifi_link_stage(void) { return s_stage; }
uint8_t wifi_link_attempt(void) { return s_attempt; }
uint8_t wifi_link_attempt_limit(void) { return s_limit; }
wifi_fail_t wifi_link_failure(void) { return s_failure; }
uint16_t wifi_link_reason(void) { return s_reason; }

uint32_t wifi_link_stage_elapsed_ms(void)
{
    return (uint32_t)((esp_timer_get_time() - s_stage_us) / 1000);
}

bool wifi_link_info(wifi_link_info_t *out)
{
    if (!s_have_info || s_stage != WIFI_LINK_CONNECTED) return false;
    *out = s_info;
    return true;
}

void wifi_link_refresh_signal(void)
{
    if (!s_have_info || s_stage != WIFI_LINK_CONNECTED) return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_info.rssi = ap.rssi;
        s_info.channel = ap.primary;
    }
}
