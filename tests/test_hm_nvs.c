// Host tests for the flash history (main/hm_nvs.c) against an in-memory NVS:
// save and restore, the history limit surviving a restart, pruning by the
// 10-year window, another user's data, and malformed entries.
#include "hm_calendar.h"
#include "hm_nvs.h"
#include "hm_store.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        s_failures++; \
    } \
} while (0)

// ---------------------------------------------------------------------------
// In-memory NVS (one partition, a few namespaces).
// ---------------------------------------------------------------------------

#define MAX_ENTRIES 32
#define MAX_HANDLES 4

typedef struct {
    bool used;
    char ns[NVS_NS_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
    size_t len;
    uint8_t data[400];
} entry_t;

static entry_t s_entries[MAX_ENTRIES];
static char s_namespaces[4][NVS_NS_NAME_MAX_SIZE];
static struct { bool open; nvs_open_mode_t mode; char ns[NVS_NS_NAME_MAX_SIZE]; } s_handles[MAX_HANDLES];
static int s_writes, s_erases;

struct nvs_opaque_iterator_t { char ns[NVS_NS_NAME_MAX_SIZE]; nvs_type_t type; int index; };
static struct nvs_opaque_iterator_t s_iter;
static bool s_iter_live;

static void reset_nvs(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_namespaces, 0, sizeof(s_namespaces));
    memset(s_handles, 0, sizeof(s_handles));
    s_writes = s_erases = 0;
    s_iter_live = false;
}

static int open_handles(void)
{
    int n = 0;
    for (int i = 0; i < MAX_HANDLES; i++) n += s_handles[i].open;
    return n;
}

static bool ns_exists(const char *ns)
{
    for (int i = 0; i < 4; i++) {
        if (strcmp(s_namespaces[i], ns) == 0) return true;
    }
    return false;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (strlen(name) >= NVS_NS_NAME_MAX_SIZE) return ESP_ERR_INVALID_ARG;
    if (!ns_exists(name)) {
        if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
        for (int i = 0; i < 4; i++) {
            if (!s_namespaces[i][0]) {
                strcpy(s_namespaces[i], name);
                break;
            }
        }
    }
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (s_handles[i].open) continue;
        s_handles[i].open = true;
        s_handles[i].mode = mode;
        strcpy(s_handles[i].ns, name);
        *out = (nvs_handle_t)(i + 1);
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

void nvs_close(nvs_handle_t handle)
{
    if (handle >= 1 && handle <= MAX_HANDLES) s_handles[handle - 1].open = false;
}

static entry_t *find(nvs_handle_t handle, const char *key, nvs_type_t type)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        entry_t *e = &s_entries[i];
        if (e->used && strcmp(e->ns, s_handles[handle - 1].ns) == 0 && strcmp(e->key, key) == 0 &&
            (type == NVS_TYPE_ANY || e->type == type)) {
            return e;
        }
    }
    return NULL;
}

static esp_err_t put(nvs_handle_t handle, const char *key, nvs_type_t type, const void *value, size_t len)
{
    if (!s_handles[handle - 1].open || s_handles[handle - 1].mode != NVS_READWRITE) return ESP_ERR_INVALID_STATE;
    if (strlen(key) >= NVS_KEY_NAME_MAX_SIZE || len > sizeof(s_entries[0].data)) return ESP_ERR_INVALID_ARG;
    entry_t *e = find(handle, key, NVS_TYPE_ANY);
    for (int i = 0; !e && i < MAX_ENTRIES; i++) {
        if (!s_entries[i].used) e = &s_entries[i];
    }
    if (!e) return ESP_ERR_NO_MEM;
    e->used = true;
    strcpy(e->ns, s_handles[handle - 1].ns);
    strcpy(e->key, key);
    e->type = type;
    e->len = len;
    memcpy(e->data, value, len);
    s_writes++;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *len)
{
    const entry_t *e = find(handle, key, NVS_TYPE_STR);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) return ESP_ERR_INVALID_SIZE;
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    return put(handle, key, NVS_TYPE_STR, value, strlen(value) + 1);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *len)
{
    const entry_t *e = find(handle, key, NVS_TYPE_BLOB);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) return ESP_ERR_INVALID_SIZE;
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t len)
{
    return put(handle, key, NVS_TYPE_BLOB, value, len);
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    if (s_handles[handle - 1].mode != NVS_READWRITE) return ESP_ERR_INVALID_STATE;
    entry_t *e = find(handle, key, NVS_TYPE_ANY);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    e->used = false;
    s_erases++;
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    if (s_handles[handle - 1].mode != NVS_READWRITE) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].ns, s_handles[handle - 1].ns) == 0) {
            s_entries[i].used = false;
            s_erases++;
        }
    }
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    return s_handles[handle - 1].open ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool iter_match(int i)
{
    return s_entries[i].used && strcmp(s_entries[i].ns, s_iter.ns) == 0 &&
           (s_iter.type == NVS_TYPE_ANY || s_entries[i].type == s_iter.type);
}

esp_err_t nvs_entry_find(const char *part, const char *ns, nvs_type_t type, nvs_iterator_t *it)
{
    if (strcmp(part, NVS_DEFAULT_PART_NAME) != 0 || s_iter_live) return ESP_ERR_INVALID_ARG;
    strcpy(s_iter.ns, ns);
    s_iter.type = type;
    for (s_iter.index = 0; s_iter.index < MAX_ENTRIES && !iter_match(s_iter.index); s_iter.index++) {}
    if (s_iter.index == MAX_ENTRIES) {
        *it = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_iter_live = true;
    *it = &s_iter;
    return ESP_OK;
}

esp_err_t nvs_entry_next(nvs_iterator_t *it)
{
    if (!*it) return ESP_ERR_INVALID_ARG;
    for (s_iter.index++; s_iter.index < MAX_ENTRIES && !iter_match(s_iter.index); s_iter.index++) {}
    if (s_iter.index == MAX_ENTRIES) {
        // As in ESP-IDF: the exhausted iterator is released and cleared.
        s_iter_live = false;
        *it = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t nvs_entry_info(const nvs_iterator_t it, nvs_entry_info_t *out)
{
    if (!it) return ESP_ERR_INVALID_ARG;
    const entry_t *e = &s_entries[it->index];
    strcpy(out->namespace_name, e->ns);
    strcpy(out->key, e->key);
    out->type = e->type;
    return ESP_OK;
}

void nvs_release_iterator(nvs_iterator_t it)
{
    if (it) s_iter_live = false;
}

static bool has_key(const char *key)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].ns, "heatmap") == 0 && strcmp(s_entries[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

static int key_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) n += s_entries[i].used && strcmp(s_entries[i].ns, "heatmap") == 0;
    return n;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

#define USER "wsycqyz"

static int32_t D(int y, int m, int d)
{
    return hm_days_from_civil(y, m, d);
}

static void make_last(gh_block_t *b, int32_t today)
{
    gh_block_init(b, D(2008, 1, 1), D(2199, 12, 31));
    for (int32_t day = hm_week_start(hm_week_of(today) - 52); day <= today; day++) {
        gh_block_put(b, day, (uint8_t)(day % 5), day % 5 ? (int32_t)(day % 7) : 0);
    }
}

static void make_year(gh_block_t *b, int year, uint8_t level)
{
    gh_block_init(b, D(year, 1, 1), D(year, 12, 31));
    for (int32_t day = D(year, 1, 1); day <= D(year, 12, 31); day++) gh_block_put(b, day, level, level);
}

static hm_store_t s_store, s_restored;
static gh_block_t s_block;

static void test_restore_after_power_on(void)
{
    const int32_t today = D(2026, 9, 25);
    reset_nvs();
    hm_store_init(&s_restored);
    CHECK(!hm_nvs_load(&s_restored, USER) && !s_restored.have_last);   // nothing saved yet
    hm_store_init(&s_store);
    CHECK(hm_nvs_save_last(&s_store, USER) == ESP_ERR_INVALID_STATE);

    make_last(&s_block, today);
    CHECK(hm_store_set_last(&s_store, &s_block));
    CHECK(hm_nvs_save_last(&s_store, USER) == ESP_OK);
    CHECK(hm_nvs_save_year(&s_store, USER, 2025) == ESP_ERR_INVALID_ARG);   // not downloaded
    const int years[] = { 2025, 2024, 2023, 2022 };
    for (int i = 0; i < 4; i++) {
        make_year(&s_block, years[i], years[i] == 2022 ? 0 : 3);   // 2022: no contributions
        CHECK(hm_store_set_year(&s_store, years[i], &s_block));
        CHECK(hm_nvs_save_year(&s_store, USER, years[i]) == ESP_OK);
    }
    CHECK(s_store.floor_year == 2023);
    // The empty year that set the limit stays, so the limit survives a restart.
    CHECK(has_key("user") && has_key("last") && has_key("y2025") && has_key("y2024") &&
          has_key("y2023") && has_key("y2022") && key_count() == 6);
    CHECK(open_handles() == 0 && !s_iter_live);

    // Power on: everything is back, and nothing has to be downloaded again.
    hm_store_init(&s_restored);
    CHECK(hm_nvs_load(&s_restored, USER));
    CHECK(s_restored.today == today && hm_block_same(&s_restored.last, &s_store.last));
    CHECK(s_restored.floor_year == 2023 && hm_store_oldest_year(&s_restored) == 2023);
    for (int i = 0; i < 4; i++) {
        const gh_block_t *a = hm_store_year_block(&s_store, years[i]);
        const gh_block_t *b = hm_store_year_block(&s_restored, years[i]);
        CHECK(a && b && hm_block_same(a, b));
    }
    CHECK(hm_store_missing_year(&s_restored, D(2012, 1, 1), today) == 0);
    CHECK(hm_store_level(&s_restored, D(2023, 1, 1)) == 3);
    CHECK(hm_store_level(&s_restored, D(2022, 12, 31)) == HM_LEVEL_NONE);
    CHECK(open_handles() == 0);
}

static void test_prune_follows_the_window(void)
{
    // Ten years later the window is 2027..2036; stored older years go.
    const int writes = s_writes;
    make_last(&s_block, D(2037, 3, 1));
    CHECK(hm_store_set_last(&s_restored, &s_block));
    CHECK(hm_nvs_save_last(&s_restored, USER) == ESP_OK);
    CHECK(s_writes == writes + 1);
    CHECK(has_key("user") && has_key("last") && key_count() == 2);
    CHECK(open_handles() == 0 && !s_iter_live);
}

static void test_other_user_and_bad_data(void)
{
    const int32_t today = D(2026, 9, 25);
    reset_nvs();
    hm_store_init(&s_store);
    make_last(&s_block, today);
    hm_store_set_last(&s_store, &s_block);
    CHECK(hm_nvs_save_last(&s_store, USER) == ESP_OK);
    make_year(&s_block, 2025, 2);
    hm_store_set_year(&s_store, 2025, &s_block);
    CHECK(hm_nvs_save_year(&s_store, USER, 2025) == ESP_OK);

    // A malformed year is skipped; the rest still loads.
    nvs_handle_t h;
    CHECK(nvs_open("heatmap", NVS_READWRITE, &h) == ESP_OK);
    CHECK(nvs_set_blob(h, "y2025", "\x01\x00garbage", 9) == ESP_OK);
    nvs_close(h);
    hm_store_init(&s_restored);
    CHECK(hm_nvs_load(&s_restored, USER) && !hm_store_year_block(&s_restored, 2025));

    // Another configured user sees nothing, and its first save starts over.
    hm_store_init(&s_restored);
    CHECK(!hm_nvs_load(&s_restored, "someone-else"));
    CHECK(hm_nvs_save_last(&s_store, "someone-else") == ESP_OK);
    CHECK(key_count() == 2 && has_key("user") && has_key("last"));
    hm_store_init(&s_restored);
    CHECK(!hm_nvs_load(&s_restored, USER));

    // A malformed rolling year restores nothing.
    CHECK(hm_nvs_save_last(&s_store, USER) == ESP_OK);
    CHECK(nvs_open("heatmap", NVS_READWRITE, &h) == ESP_OK);
    CHECK(nvs_set_blob(h, "last", "\x07", 1) == ESP_OK);
    nvs_close(h);
    hm_store_init(&s_restored);
    CHECK(!hm_nvs_load(&s_restored, USER) && !s_restored.have_last);
    CHECK(open_handles() == 0);
}

int main(void)
{
    test_restore_after_power_on();
    test_prune_follows_the_window();
    test_other_user_and_bad_data();
    if (s_failures) {
        fprintf(stderr, "test_hm_nvs: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_hm_nvs: PASS\n");
    return 0;
}
