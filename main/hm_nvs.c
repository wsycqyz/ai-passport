// main/hm_nvs.c —— see hm_nvs.h.
#include "hm_nvs.h"

#include "hm_calendar.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "hm_nvs";

#define NAMESPACE  "heatmap"
#define KEY_USER   "user"
#define KEY_LAST   "last"
#define USER_CAP   40
#define PRUNE_MAX  16

static uint8_t s_blob[HM_BLOB_MAX];
static gh_block_t s_block;

static void year_key(int year, char key[NVS_KEY_NAME_MAX_SIZE])
{
    snprintf(key, NVS_KEY_NAME_MAX_SIZE, "y%04d", year);
}

static bool same_user(nvs_handle_t h, const char *user)
{
    char stored[USER_CAP];
    size_t len = sizeof(stored);
    return nvs_get_str(h, KEY_USER, stored, &len) == ESP_OK && strcmp(stored, user) == 0;
}

static bool read_block(nvs_handle_t h, const char *key, int32_t min_day, int32_t max_day)
{
    size_t len = sizeof(s_blob);
    if (nvs_get_blob(h, key, s_blob, &len) != ESP_OK) return false;
    if (hm_block_decode(s_blob, len, min_day, max_day, &s_block)) return true;
    ESP_LOGW(TAG, "ignoring malformed \"%s\"", key);
    return false;
}

bool hm_nvs_load(hm_store_t *s, const char *user)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;   // nothing saved yet
    const bool ok = same_user(h, user) &&
                    read_block(h, KEY_LAST, hm_days_from_civil(HM_FIRST_YEAR, 1, 1),
                               hm_days_from_civil(2199, 12, 31)) &&
                    hm_store_set_last(s, &s_block);
    int years = 0;
    if (ok) {
        // Newest first: an empty year on the way moves the history limit, which
        // ends the loop.
        for (int year = hm_year_of(s->last.first_day - 1); year >= hm_store_oldest_year(s); year--) {
            char key[NVS_KEY_NAME_MAX_SIZE];
            year_key(year, key);
            if (read_block(h, key, hm_days_from_civil(year, 1, 1), hm_days_from_civil(year, 12, 31)) &&
                hm_store_set_year(s, year, &s_block)) {
                years++;
            }
        }
        ESP_LOGI(TAG, "restored %u days and %d older year(s)", s->last.filled, years);
    }
    nvs_close(h);
    return ok;
}

// Open for writing; another account's history is erased first.
static esp_err_t open_for(const char *user, nvs_handle_t *h)
{
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, h);
    if (err != ESP_OK) return err;
    if (!same_user(*h, user)) {
        err = nvs_erase_all(*h);
        if (err == ESP_OK) err = nvs_set_str(*h, KEY_USER, user);
        if (err != ESP_OK) nvs_close(*h);
    }
    return err;
}

static esp_err_t write_block(nvs_handle_t h, const char *key, const gh_block_t *b)
{
    const size_t len = hm_block_encode(b, s_blob, sizeof(s_blob));
    return len ? nvs_set_blob(h, key, s_blob, len) : ESP_ERR_INVALID_SIZE;
}

// Erase stored calendar years outside the 10-year window. Years below the
// history limit inside the window stay: the empty year that set the limit is
// how the limit survives a restart.
static void prune(nvs_handle_t h, const hm_store_t *s)
{
    const int oldest = hm_store_window_oldest_year(s);
    const int newest = hm_year_of(s->today);
    char doomed[PRUNE_MAX][NVS_KEY_NAME_MAX_SIZE];
    int count = 0;
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, NAMESPACE, NVS_TYPE_BLOB, &it);
    while (res == ESP_OK && count < PRUNE_MAX) {
        nvs_entry_info_t info;
        int year;
        char tail;
        if (nvs_entry_info(it, &info) == ESP_OK && sscanf(info.key, "y%d%c", &year, &tail) == 1 &&
            (year < oldest || year > newest)) {
            strcpy(doomed[count++], info.key);
        }
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    for (int i = 0; i < count; i++) {
        if (nvs_erase_key(h, doomed[i]) == ESP_OK) ESP_LOGI(TAG, "pruned \"%s\"", doomed[i]);
    }
}

esp_err_t hm_nvs_save_last(const hm_store_t *s, const char *user)
{
    if (!s->have_last) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = open_for(user, &h);
    if (err != ESP_OK) return err;
    err = write_block(h, KEY_LAST, &s->last);
    if (err == ESP_OK) {
        prune(h, s);
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t hm_nvs_save_year(const hm_store_t *s, const char *user, int year)
{
    const gh_block_t *block = hm_store_year_block(s, year);
    if (!s->have_last || !block) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_for(user, &h);
    if (err != ESP_OK) return err;
    char key[NVS_KEY_NAME_MAX_SIZE];
    year_key(year, key);
    err = write_block(h, key, block);
    if (err == ESP_OK) {
        prune(h, s);
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
