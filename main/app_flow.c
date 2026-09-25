// main/app_flow.c —— see app_flow.h for the state diagram.
#include "app_flow.h"

#include <stddef.h>

void app_flow_init(app_flow_t *flow)
{
    flow->state = APP_STATE_BOOT;
    flow->source = APP_SOURCE_SAVED;
    flow->note = APP_NOTE_NONE;
}

static uint32_t go_setup(app_flow_t *flow, app_note_t note)
{
    flow->state = APP_STATE_SETUP;
    flow->note = note;
    return APP_ACTION_RENDER;
}

static uint32_t go_connect(app_flow_t *flow, app_source_t source)
{
    flow->state = APP_STATE_CONNECTING;
    flow->source = source;
    flow->note = APP_NOTE_NONE;
    return APP_ACTION_RENDER | APP_ACTION_CONNECT;
}

uint32_t app_flow_handle(app_flow_t *flow, app_event_t event)
{
    switch (flow->state) {
    case APP_STATE_BOOT:
        if (event == APP_EVENT_BOOT_SAVED) return go_connect(flow, APP_SOURCE_SAVED);
        if (event == APP_EVENT_BOOT_EMPTY) return go_setup(flow, APP_NOTE_NO_SAVED);
        return 0;

    case APP_STATE_CONNECTING:
        if (event == APP_EVENT_BUTTON) {
            return APP_ACTION_WIFI_STOP | go_setup(flow, APP_NOTE_CONNECT_STOPPED);
        }
        if (event == APP_EVENT_WIFI_CONNECTED) {
            const uint32_t save = flow->source == APP_SOURCE_SOUND ? APP_ACTION_SAVE : 0;
            flow->state = APP_STATE_CONNECTED;
            return save | APP_ACTION_RENDER;
        }
        if (event == APP_EVENT_WIFI_FAILED) {
            flow->state = APP_STATE_FAILED;
            return APP_ACTION_WIFI_STOP | APP_ACTION_RENDER;
        }
        return 0;

    case APP_STATE_SETUP:
        if (event == APP_EVENT_BUTTON) {
            flow->state = APP_STATE_LISTENING;
            flow->note = APP_NOTE_NONE;
            return APP_ACTION_LISTEN_START | APP_ACTION_RENDER;
        }
        return 0;

    case APP_STATE_LISTENING:
        switch (event) {
        case APP_EVENT_BUTTON:
            return APP_ACTION_LISTEN_STOP | go_setup(flow, APP_NOTE_LISTEN_CANCELLED);
        case APP_EVENT_CREDENTIALS:
            return APP_ACTION_LISTEN_STOP | go_connect(flow, APP_SOURCE_SOUND);
        case APP_EVENT_LISTEN_TIMEOUT:
            return APP_ACTION_LISTEN_STOP | go_setup(flow, APP_NOTE_LISTEN_TIMEOUT);
        case APP_EVENT_LISTEN_FAILED:
            return APP_ACTION_LISTEN_STOP | go_setup(flow, APP_NOTE_AUDIO_ERROR);
        default:
            return 0;
        }

    case APP_STATE_CONNECTED:
        if (event == APP_EVENT_BUTTON) return APP_ACTION_WIFI_STOP | go_setup(flow, APP_NOTE_NONE);
        if (event == APP_EVENT_WIFI_LOST) return go_connect(flow, APP_SOURCE_RECONNECT);
        return 0;

    case APP_STATE_FAILED:
        if (event == APP_EVENT_BUTTON) return go_setup(flow, APP_NOTE_NONE);
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
