// main/gh_fetch.h —— Background download of the GitHub contribution calendar.
//
// One worker task runs at most one request at a time: GitHub's rolling last
// year, or one calendar year. It asks github-contributions-api (compact JSON,
// https://github.com/grubersjoe/github-contributions-api) first and GitHub's
// own calendar page second, parsing either stream on the fly into a gh_block_t,
// so no response is ever held in RAM whole.
//
// Threading: gh_fetch_start/cancel/take are called from the controller task.
// The worker reports the end of each request through the post callback (from
// its own task; it may block until the message is queued). After an OK result,
// gh_fetch_take() copies the block; it stays valid until the next start.
// gh_fetch_cancel() makes a running request stop at its next network read.
#pragma once

#include "esp_err.h"
#include "gh_parse.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GH_FETCH_LAST = 0,     // the rolling year ending today
    GH_FETCH_YEAR,         // one calendar year
} gh_fetch_kind_t;

typedef enum {
    GH_FETCH_OK = 0,
    GH_FETCH_FAILED,
    GH_FETCH_CANCELLED,
} gh_fetch_status_t;

typedef struct {
    uint32_t id;
    gh_fetch_kind_t kind;
    int16_t year;
    gh_fetch_status_t status;
} gh_fetch_done_t;

typedef void (*gh_fetch_post_t)(const gh_fetch_done_t *done);

// user: GitHub login (letters, digits and '-'). Creates the worker task.
esp_err_t gh_fetch_init(const char *user, gh_fetch_post_t post);

// Returns the request id, or 0 when a request is still running.
uint32_t gh_fetch_start(gh_fetch_kind_t kind, int year);

void gh_fetch_cancel(void);

bool gh_fetch_take(uint32_t id, gh_block_t *out);
