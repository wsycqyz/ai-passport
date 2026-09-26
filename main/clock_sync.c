// main/clock_sync.c —— see clock_sync.h.
#include "clock_sync.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stddef.h>

static const char *TAG = "clock_sync";

static bool s_started;
static volatile clock_sync_cb_t s_on_sync;

static void on_time(struct timeval *tv)
{
    const clock_sync_cb_t cb = s_on_sync;
    if (cb && tv) cb((int64_t)tv->tv_sec);
}

esp_err_t clock_sync_start(clock_sync_cb_t on_sync)
{
    s_on_sync = on_sync;
    if (s_started) return esp_netif_sntp_start();   // stop + start: ask again now
    // sdkconfig.defaults asks for two server slots, but an sdkconfig made
    // before that still has one; SNTP would then reject two servers.
#if CONFIG_LWIP_SNTP_MAX_SERVERS >= 2
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.cloudflare.com"));
#else
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
#endif
    cfg.wait_for_sync = false;   // nobody blocks on the first sync
    cfg.sync_cb = on_time;
    const esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        s_started = true;
        ESP_LOGI(TAG, "asking %u NTP server(s) for the time", (unsigned)cfg.num_of_servers);
    } else {
        ESP_LOGW(TAG, "time sync unavailable: %s", esp_err_to_name(err));
    }
    return err;
}
