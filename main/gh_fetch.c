// main/gh_fetch.c —— see gh_fetch.h.
#include "gh_fetch.h"

#include "hm_calendar.h"
#include "hm_store.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "gh_fetch";

#define TASK_STACK       8192          // the mbedTLS handshake runs on this stack
#define TASK_PRIORITY    3             // below LVGL (4) and the controller (5)
#define NET_TIMEOUT_MS   5000          // per connect/read step; bounds cancel latency
#define STALL_LIMIT_MS   20000         // give up when the body stops arriving
#define MAX_BODY_BYTES   (1024 * 1024) // GitHub's page for one year is about 230 KB
#define READ_CHUNK       1024
#define URL_CAP          160
#define USER_CAP         40
#define USER_AGENT       "ai-passport-heatmap/1.0"
#define API_URL          "https://github-contributions-api.jogruber.de/v4/"
#define GITHUB_URL       "https://github.com/users/"

typedef enum {
    SOURCE_API = 0,        // github-contributions-api: ~15 KB of JSON per year
    SOURCE_GITHUB,         // github.com calendar HTML: ~230 KB per year
    SOURCE_COUNT,
} source_t;

typedef struct {
    uint32_t id;
    gh_fetch_kind_t kind;
    int16_t year;
} request_t;

static QueueHandle_t s_requests;
static gh_fetch_post_t s_post;
static char s_user[USER_CAP];
static volatile bool s_cancel;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;                    // guarded by s_lock
static uint32_t s_next_id;             // guarded by s_lock
static uint32_t s_block_id;            // id whose result s_block holds; guarded by s_lock
// Worker-owned while a request runs. Static to keep the TLS task stack free.
static gh_block_t s_block;
static gh_parser_t s_parser;
static char s_chunk[READ_CHUNK];

static bool make_url(source_t source, const request_t *req, char *url, size_t cap)
{
    int n;
    if (source == SOURCE_API) {
        n = req->kind == GH_FETCH_LAST
                ? snprintf(url, cap, API_URL "%s?y=last", s_user)
                : snprintf(url, cap, API_URL "%s?y=%d", s_user, req->year);
    } else {
        n = req->kind == GH_FETCH_LAST
                ? snprintf(url, cap, GITHUB_URL "%s/contributions", s_user)
                : snprintf(url, cap, GITHUB_URL "%s/contributions?from=%d-01-01&to=%d-12-31",
                           s_user, req->year, req->year);
    }
    return n > 0 && (size_t)n < cap;
}

static gh_fetch_status_t download(const char *url, int *http_status, size_t *bytes)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = NET_TIMEOUT_MS,
        .buffer_size = READ_CHUNK,
        .user_agent = USER_AGENT,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return GH_FETCH_FAILED;

    gh_fetch_status_t result = GH_FETCH_FAILED;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
        goto done;
    }
    const int64_t length = esp_http_client_fetch_headers(client);
    *http_status = esp_http_client_get_status_code(client);
    if (length < 0 || *http_status != 200 || length > MAX_BODY_BYTES) {
        ESP_LOGW(TAG, "HTTP %d, length %lld", *http_status, (long long)length);
        goto done;
    }
    int64_t last_data_us = esp_timer_get_time();
    for (;;) {
        if (s_cancel) {
            result = GH_FETCH_CANCELLED;
            break;
        }
        const int n = esp_http_client_read(client, s_chunk, sizeof(s_chunk));
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            // A read timed out without data; keep waiting up to the stall limit.
            if (esp_timer_get_time() - last_data_us >= (int64_t)STALL_LIMIT_MS * 1000) {
                ESP_LOGW(TAG, "response stalled after %u bytes", (unsigned)*bytes);
                break;
            }
            continue;
        }
        if (n < 0) {
            ESP_LOGW(TAG, "read failed after %u bytes", (unsigned)*bytes);
            break;
        }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                result = GH_FETCH_OK;
            } else {
                ESP_LOGW(TAG, "response truncated after %u bytes", (unsigned)*bytes);
            }
            break;
        }
        last_data_us = esp_timer_get_time();
        *bytes += (size_t)n;
        if (*bytes > MAX_BODY_BYTES) {
            ESP_LOGW(TAG, "response larger than %d bytes", MAX_BODY_BYTES);
            break;
        }
        gh_parser_feed(&s_parser, s_chunk, (size_t)n);
    }
done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

static bool plausible(const request_t *req)
{
    return req->kind == GH_FETCH_LAST ? hm_store_last_plausible(&s_block)
                                      : hm_store_year_plausible(req->year, &s_block);
}

static gh_fetch_status_t run(const request_t *req)
{
    for (source_t source = SOURCE_API; source < SOURCE_COUNT; source++) {
        char url[URL_CAP];
        if (!make_url(source, req, url, sizeof(url))) return GH_FETCH_FAILED;
        if (req->kind == GH_FETCH_LAST) {
            gh_block_init(&s_block, hm_days_from_civil(HM_FIRST_YEAR, 1, 1),
                          hm_days_from_civil(2199, 12, 31));
        } else {
            gh_block_init(&s_block, hm_days_from_civil(req->year, 1, 1),
                          hm_days_from_civil(req->year, 12, 31));
        }
        gh_parser_init(&s_parser, gh_block_on_day, &s_block);
        ESP_LOGI(TAG, "GET %s (heap free %u, largest %u)", url,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        const int64_t start_us = esp_timer_get_time();
        int status = 0;
        size_t bytes = 0;
        const gh_fetch_status_t result = download(url, &status, &bytes);
        const unsigned ms = (unsigned)((esp_timer_get_time() - start_us) / 1000);
        if (result == GH_FETCH_CANCELLED || s_cancel) {
            ESP_LOGI(TAG, "cancelled after %u ms", ms);
            return GH_FETCH_CANCELLED;
        }
        if (result == GH_FETCH_OK && plausible(req)) {
            ESP_LOGI(TAG, "%u days, %u contributions, %u bytes in %u ms (heap minimum ever %u)",
                     s_block.filled, (unsigned)s_block.total, (unsigned)bytes, ms,
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            return GH_FETCH_OK;
        }
        ESP_LOGW(TAG, "source %d unusable: HTTP %d, %u bytes, %u days, %u rejected, %u ms",
                 (int)source, status, (unsigned)bytes, s_block.filled,
                 (unsigned)(s_parser.rejected + s_block.rejected), ms);
    }
    return GH_FETCH_FAILED;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        request_t req;
        if (xQueueReceive(s_requests, &req, portMAX_DELAY) != pdTRUE) continue;
        const gh_fetch_status_t status = run(&req);
        taskENTER_CRITICAL(&s_lock);
        s_block_id = status == GH_FETCH_OK ? req.id : 0;
        s_busy = false;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGD(TAG, "worker stack free %u B", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        const gh_fetch_done_t done = {
            .id = req.id, .kind = req.kind, .year = req.year, .status = status,
        };
        if (s_post) s_post(&done);
    }
}

static bool valid_user(const char *user)
{
    const size_t len = user ? strlen(user) : 0;
    if (len == 0 || len >= USER_CAP) return false;
    for (size_t i = 0; i < len; i++) {
        const char c = user[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

esp_err_t gh_fetch_init(const char *user, gh_fetch_post_t post)
{
    if (s_requests) return ESP_OK;
    if (!valid_user(user)) {
        ESP_LOGE(TAG, "invalid GitHub user name");
        return ESP_ERR_INVALID_ARG;
    }
    strcpy(s_user, user);
    s_post = post;
    s_requests = xQueueCreate(1, sizeof(request_t));
    if (!s_requests) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "gh_fetch", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        vQueueDelete(s_requests);
        s_requests = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "contribution calendar of \"%s\"", s_user);
    return ESP_OK;
}

uint32_t gh_fetch_start(gh_fetch_kind_t kind, int year)
{
    if (!s_requests) return 0;
    uint32_t id = 0;
    taskENTER_CRITICAL(&s_lock);
    if (!s_busy) {
        s_busy = true;
        s_block_id = 0;
        s_cancel = false;
        id = ++s_next_id ? s_next_id : ++s_next_id;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!id) return 0;
    const request_t req = { .id = id, .kind = kind, .year = (int16_t)year };
    // The queue holds one request and is empty whenever the worker is idle.
    if (xQueueSend(s_requests, &req, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_lock);
        s_busy = false;
        taskEXIT_CRITICAL(&s_lock);
        return 0;
    }
    return id;
}

void gh_fetch_cancel(void)
{
    s_cancel = true;
}

bool gh_fetch_take(uint32_t id, gh_block_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    const bool ready = id != 0 && id == s_block_id && !s_busy;
    taskEXIT_CRITICAL(&s_lock);
    // The worker only writes s_block after the next gh_fetch_start(), which
    // runs in the calling task, so the copy below cannot race with it.
    if (ready) *out = s_block;
    return ready;
}
