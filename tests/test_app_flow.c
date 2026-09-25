// Host tests for the application state machine, Wi-Fi failure policy, and
// SSID display formatting (main/app_flow.c, main/wifi_policy.c, main/app_text.c).
#include "app_flow.h"
#include "app_text.h"
#include "wifi_policy.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        s_failures++; \
    } \
} while (0)

static void test_first_boot_to_success_and_save(void)
{
    app_flow_t f;
    app_flow_init(&f);
    CHECK(f.state == APP_STATE_BOOT);
    CHECK(app_flow_handle(&f, APP_EVENT_BOOT_EMPTY) == APP_ACTION_RENDER);
    CHECK(f.state == APP_STATE_SETUP && f.note == APP_NOTE_NO_SAVED);
    CHECK(app_flow_note_text(f.note) != NULL);

    CHECK(app_flow_handle(&f, APP_EVENT_BUTTON) == (APP_ACTION_LISTEN_START | APP_ACTION_RENDER));
    CHECK(f.state == APP_STATE_LISTENING && f.note == APP_NOTE_NONE);

    CHECK(app_flow_handle(&f, APP_EVENT_CREDENTIALS) ==
          (APP_ACTION_LISTEN_STOP | APP_ACTION_RENDER | APP_ACTION_CONNECT));
    CHECK(f.state == APP_STATE_CONNECTING && f.source == APP_SOURCE_SOUND);

    // New credentials are persisted only once they have connected.
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_CONNECTED) == (APP_ACTION_SAVE | APP_ACTION_RENDER));
    CHECK(f.state == APP_STATE_CONNECTED);

    CHECK(app_flow_handle(&f, APP_EVENT_BUTTON) == (APP_ACTION_WIFI_STOP | APP_ACTION_RENDER));
    CHECK(f.state == APP_STATE_SETUP && f.note == APP_NOTE_NONE);
}

static void test_saved_boot_paths(void)
{
    app_flow_t f;
    app_flow_init(&f);
    CHECK(app_flow_handle(&f, APP_EVENT_BOOT_SAVED) == (APP_ACTION_RENDER | APP_ACTION_CONNECT));
    CHECK(f.state == APP_STATE_CONNECTING && f.source == APP_SOURCE_SAVED);
    // Saved credentials that connect are not written again.
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_CONNECTED) == APP_ACTION_RENDER);

    // Losing the link retries the same credentials.
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_LOST) == (APP_ACTION_RENDER | APP_ACTION_CONNECT));
    CHECK(f.state == APP_STATE_CONNECTING && f.source == APP_SOURCE_RECONNECT);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_CONNECTED) == APP_ACTION_RENDER);

    // "Re-configure Wi-Fi" while the saved network is still connecting.
    app_flow_init(&f);
    app_flow_handle(&f, APP_EVENT_BOOT_SAVED);
    CHECK(app_flow_handle(&f, APP_EVENT_BUTTON) == (APP_ACTION_WIFI_STOP | APP_ACTION_RENDER));
    CHECK(f.state == APP_STATE_SETUP && f.note == APP_NOTE_CONNECT_STOPPED);
    // A late Wi-Fi result must not leave the setup page.
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_CONNECTED) == 0);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_FAILED) == 0);
    CHECK(f.state == APP_STATE_SETUP);
}

static void test_failure_loop(void)
{
    app_flow_t f;
    app_flow_init(&f);
    app_flow_handle(&f, APP_EVENT_BOOT_SAVED);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_FAILED) == (APP_ACTION_WIFI_STOP | APP_ACTION_RENDER));
    CHECK(f.state == APP_STATE_FAILED);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_LOST) == 0);
    CHECK(app_flow_handle(&f, APP_EVENT_BUTTON) == APP_ACTION_RENDER);
    CHECK(f.state == APP_STATE_SETUP && f.note == APP_NOTE_NONE);

    // Failed sound credentials are never saved.
    app_flow_handle(&f, APP_EVENT_BUTTON);
    app_flow_handle(&f, APP_EVENT_CREDENTIALS);
    CHECK((app_flow_handle(&f, APP_EVENT_WIFI_FAILED) & APP_ACTION_SAVE) == 0);
    CHECK(f.state == APP_STATE_FAILED);
}

static void test_listening_exits(void)
{
    const struct {
        app_event_t event;
        app_note_t note;
    } cases[] = {
        { APP_EVENT_BUTTON, APP_NOTE_LISTEN_CANCELLED },
        { APP_EVENT_LISTEN_TIMEOUT, APP_NOTE_LISTEN_TIMEOUT },
        { APP_EVENT_LISTEN_FAILED, APP_NOTE_AUDIO_ERROR },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        app_flow_t f;
        app_flow_init(&f);
        app_flow_handle(&f, APP_EVENT_BOOT_EMPTY);
        app_flow_handle(&f, APP_EVENT_BUTTON);
        CHECK(app_flow_handle(&f, cases[i].event) == (APP_ACTION_LISTEN_STOP | APP_ACTION_RENDER));
        CHECK(f.state == APP_STATE_SETUP && f.note == cases[i].note);
        CHECK(app_flow_note_text(f.note) != NULL);
    }
    app_flow_t f;
    app_flow_init(&f);
    app_flow_handle(&f, APP_EVENT_BOOT_EMPTY);
    app_flow_handle(&f, APP_EVENT_BUTTON);
    // Stale Wi-Fi events while listening are ignored.
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_CONNECTED) == 0);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_LOST) == 0);
    CHECK(f.state == APP_STATE_LISTENING);
    CHECK(app_flow_note_text(APP_NOTE_NONE) == NULL);
}

static void test_policy(void)
{
    CHECK(wifi_policy_classify(201) == WIFI_FAIL_NOT_FOUND);
    CHECK(wifi_policy_classify(200) == WIFI_FAIL_NOT_FOUND);
    CHECK(wifi_policy_classify(15) == WIFI_FAIL_WRONG_PASSWORD);
    CHECK(wifi_policy_classify(204) == WIFI_FAIL_WRONG_PASSWORD);
    CHECK(wifi_policy_classify(202) == WIFI_FAIL_WRONG_PASSWORD);
    CHECK(wifi_policy_classify(210) == WIFI_FAIL_SECURITY);
    CHECK(wifi_policy_classify(23) == WIFI_FAIL_SECURITY);
    CHECK(wifi_policy_classify(5) == WIFI_FAIL_AP_FULL);
    CHECK(wifi_policy_classify(203) == WIFI_FAIL_REJECTED);
    CHECK(wifi_policy_classify(8) == WIFI_FAIL_UNKNOWN);
    CHECK(wifi_policy_classify(0) == WIFI_FAIL_UNKNOWN);

    CHECK(wifi_policy_attempts(WIFI_FAIL_SECURITY) == 1);
    CHECK(wifi_policy_attempts(WIFI_FAIL_DRIVER) == 1);
    CHECK(wifi_policy_attempts(WIFI_FAIL_WRONG_PASSWORD) == 2);
    CHECK(wifi_policy_attempts(WIFI_FAIL_NOT_FOUND) == 2);
    CHECK(wifi_policy_attempts(WIFI_FAIL_UNKNOWN) == 3);
    CHECK(wifi_policy_attempts(WIFI_FAIL_NO_IP) == 3);

    CHECK(strcmp(wifi_policy_title(WIFI_FAIL_UNKNOWN), "Error") == 0);
    CHECK(strcmp(wifi_policy_title(WIFI_FAIL_WRONG_PASSWORD), "Wrong password") == 0);
    for (int f = WIFI_FAIL_NONE; f <= WIFI_FAIL_UNKNOWN; f++) {
        const char *title = wifi_policy_title((wifi_fail_t)f);
        const char *hint = wifi_policy_hint((wifi_fail_t)f);
        CHECK(title && hint && *title && *hint);
        // Keep titles within one line of the 20 px font on a 240 px screen.
        CHECK(strlen(title) <= 22);
        for (const char *p = hint; *p; p++) CHECK(*p >= 0x20 && *p <= 0x7E);
    }
}

static void test_ssid_text(void)
{
    char out[40];
    app_text_ssid((const uint8_t *)"Home 2.4G", 9, out, sizeof(out));
    CHECK(strcmp(out, "Home 2.4G") == 0);
    // Two CJK characters (3 bytes each) become one '?' each.
    app_text_ssid((const uint8_t *)"\xE5\x92\x96\xE5\x95\xA1-Wi", 9, out, sizeof(out));
    CHECK(strcmp(out, "?\?-Wi") == 0);   // escaped: "??-" is a C trigraph
    // Invalid and truncated sequences never swallow following ASCII.
    app_text_ssid((const uint8_t *)"\xFF" "a" "\xE5\x92" "b", 5, out, sizeof(out));
    CHECK(strcmp(out, "?a??b") == 0);
    app_text_ssid((const uint8_t *)"tab\there", 8, out, sizeof(out));
    CHECK(strcmp(out, "tab?here") == 0);
    // Truncation keeps the terminator.
    app_text_ssid((const uint8_t *)"abcdefgh", 8, out, 5);
    CHECK(strcmp(out, "abcd") == 0);
    app_text_ssid(NULL, 0, out, sizeof(out));
    CHECK(out[0] == '\0');
}

int main(void)
{
    test_first_boot_to_success_and_save();
    test_saved_boot_paths();
    test_failure_loop();
    test_listening_exits();
    test_policy();
    test_ssid_text();
    if (s_failures) {
        fprintf(stderr, "test_app_flow: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_app_flow: PASS\n");
    return 0;
}
