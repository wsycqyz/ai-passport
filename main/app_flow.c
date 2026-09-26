// main/app_flow.c —— see app_flow.h for the state diagram.
#include "app_flow.h"

#include <stddef.h>

void app_flow_init(app_flow_t *flow)
{
    flow->state = APP_STATE_BOOT;
    flow->link = APP_LINK_OFF;
    flow->source = APP_SOURCE_SAVED;
    flow->note = APP_NOTE_NONE;
    flow->have_saved = false;
}

static uint32_t go_setup(app_flow_t *flow, app_note_t note)
{
    flow->state = APP_STATE_SETUP;
    flow->link = APP_LINK_OFF;
    flow->note = note;
    return APP_ACTION_RENDER;
}

// The main page always opens, whether or not Wi-Fi can connect.
static uint32_t go_heatmap(app_flow_t *flow)
{
    flow->state = APP_STATE_HEATMAP;
    flow->note = APP_NOTE_NONE;
    if (!flow->have_saved) {
        flow->link = APP_LINK_OFF;
        return APP_ACTION_RENDER;
    }
    flow->link = APP_LINK_CONNECTING;
    flow->source = APP_SOURCE_SAVED;
    return APP_ACTION_RENDER | APP_ACTION_CONNECT;
}

static uint32_t reconnect(app_flow_t *flow)
{
    flow->link = APP_LINK_CONNECTING;
    flow->source = APP_SOURCE_RECONNECT;
    return APP_ACTION_LINK | APP_ACTION_CONNECT;
}

static uint32_t on_heatmap(app_flow_t *flow, app_event_t event)
{
    switch (event) {
    case APP_EVENT_OK:
        return APP_ACTION_WIFI_STOP | APP_ACTION_FETCH_CANCEL | go_setup(flow, APP_NOTE_NONE);
    case APP_EVENT_WIFI_CONNECTED:
        if (flow->link != APP_LINK_CONNECTING) return 0;
        flow->link = APP_LINK_UP;
        return APP_ACTION_LINK;
    case APP_EVENT_WIFI_FAILED:
        if (flow->link != APP_LINK_CONNECTING) return 0;
        flow->link = APP_LINK_WAITING;
        return APP_ACTION_WIFI_STOP | APP_ACTION_RETRY_LATER | APP_ACTION_LINK;
    case APP_EVENT_WIFI_LOST:
        return flow->link == APP_LINK_UP ? reconnect(flow) : 0;
    case APP_EVENT_RETRY:
        return flow->link == APP_LINK_WAITING ? reconnect(flow) : 0;
    default:
        return 0;
    }
}

uint32_t app_flow_handle(app_flow_t *flow, app_event_t event)
{
    switch (flow->state) {
    case APP_STATE_BOOT:
        if (event == APP_EVENT_BOOT_SAVED) {
            flow->have_saved = true;
            return go_heatmap(flow);
        }
        if (event == APP_EVENT_BOOT_EMPTY) {
            flow->have_saved = false;
            return go_setup(flow, APP_NOTE_NO_SAVED);
        }
        return 0;

    case APP_STATE_HEATMAP:
        return on_heatmap(flow, event);

    case APP_STATE_SETUP:
        if (event == APP_EVENT_OK) {
            flow->state = APP_STATE_LISTENING;
            flow->note = APP_NOTE_NONE;
            return APP_ACTION_LISTEN_START | APP_ACTION_RENDER;
        }
        if (event == APP_EVENT_BACK) return go_heatmap(flow);
        return 0;

    case APP_STATE_LISTENING:
        switch (event) {
        case APP_EVENT_OK:
            return APP_ACTION_LISTEN_STOP | go_setup(flow, APP_NOTE_LISTEN_CANCELLED);
        case APP_EVENT_BACK:
            return APP_ACTION_LISTEN_STOP | go_heatmap(flow);
        case APP_EVENT_CREDENTIALS:
            flow->state = APP_STATE_CONNECTING;
            flow->link = APP_LINK_CONNECTING;
            flow->source = APP_SOURCE_SOUND;
            flow->note = APP_NOTE_NONE;
            return APP_ACTION_LISTEN_STOP | APP_ACTION_RENDER | APP_ACTION_CONNECT;
        case APP_EVENT_LISTEN_TIMEOUT:
            return APP_ACTION_LISTEN_STOP | go_setup(flow, APP_NOTE_LISTEN_TIMEOUT);
        case APP_EVENT_LISTEN_FAILED:
            return APP_ACTION_LISTEN_STOP | go_setup(flow, APP_NOTE_AUDIO_ERROR);
        default:
            return 0;
        }

    case APP_STATE_CONNECTING:
        switch (event) {
        case APP_EVENT_OK:
            return APP_ACTION_WIFI_STOP | go_setup(flow, APP_NOTE_CONNECT_STOPPED);
        case APP_EVENT_BACK:
            return APP_ACTION_WIFI_STOP | go_heatmap(flow);
        case APP_EVENT_WIFI_CONNECTED: {
            // Once connected, go straight back to the main page.
            const uint32_t save = flow->source == APP_SOURCE_SOUND ? APP_ACTION_SAVE : 0;
            flow->state = APP_STATE_HEATMAP;
            flow->link = APP_LINK_UP;
            return save | APP_ACTION_RENDER;
        }
        case APP_EVENT_WIFI_FAILED:
            flow->state = APP_STATE_FAILED;
            flow->link = APP_LINK_OFF;
            return APP_ACTION_WIFI_STOP | APP_ACTION_RENDER;
        default:
            return 0;
        }

    case APP_STATE_FAILED:
        if (event == APP_EVENT_OK) return go_setup(flow, APP_NOTE_NONE);
        if (event == APP_EVENT_BACK) return go_heatmap(flow);
        return 0;
    }
    return 0;
}

const char *app_flow_note_text(app_note_t note)
{
    switch (note) {
    case APP_NOTE_NO_SAVED:         return "No Wi-Fi saved yet";
    case APP_NOTE_CONNECT_STOPPED:  return "Connection stopped";
    case APP_NOTE_LISTEN_CANCELLED: return "Listening cancelled";
    case APP_NOTE_LISTEN_TIMEOUT:   return "No setup sound heard";
    case APP_NOTE_AUDIO_ERROR:      return "Microphone error";
    case APP_NOTE_NONE:
    default:                        return NULL;
    }
}

uint32_t app_retry_delay_ms(uint32_t base_ms, uint32_t cap_ms, uint8_t failures)
{
    uint32_t delay = base_ms;
    for (uint8_t i = 1; i < failures && delay < cap_ms; i++) {
        delay = delay > cap_ms / 2 ? cap_ms : delay * 2;
    }
    return delay < cap_ms ? delay : cap_ms;
}

bool app_refresh_due(bool downloaded_since_boot, int64_t now_us, int64_t last_ok_us, int64_t period_us)
{
    return !downloaded_since_boot || now_us - last_ok_us >= period_us;
}
