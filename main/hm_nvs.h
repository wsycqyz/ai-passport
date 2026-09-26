// main/hm_nvs.h —— Contribution history kept in flash (NVS) across power-offs.
//
// Namespace "heatmap" holds "user" (the GitHub login the data belongs to),
// "last" (the rolling year) and "yYYYY" (calendar years), each block encoded
// with hm_block_encode(). Data of a different configured user is discarded
// on the next save; stored years outside the 10-year window are pruned so the
// 24 KB NVS partition cannot fill up (the empty year below the history limit
// is kept: it restores that limit at the next power-on).
//
// Controller task only (static work buffers). NVS must be initialised first;
// wifi_link_init() does it.
#pragma once

#include "esp_err.h"
#include "hm_store.h"

#include <stdbool.h>

// Restore into an empty store. True when the rolling year was restored.
bool hm_nvs_load(hm_store_t *s, const char *user);

esp_err_t hm_nvs_save_last(const hm_store_t *s, const char *user);
esp_err_t hm_nvs_save_year(const hm_store_t *s, const char *user, int year);
