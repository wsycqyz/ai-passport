// Host tests for the application state machine, retry back-off, Wi-Fi
// failure policy, and SSID display formatting (main/app_flow.c,
// main/wifi_policy.c, main/app_text.c).
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

static app_flow_t booted(bool saved)
{
    app_flow_t f;
    app_flow_init(&f);
    app_flow_handle(&f, saved ? APP_EVENT_BOOT_SAVED : APP_EVENT_BOOT_EMPTY);
    return f;
}

static void test_first_boot_to_heatmap_and_save(void)
{
    app_flow_t f;
    app_flow_init(&f);
    CHECK(f.state == APP_STATE_BOOT && f.link == APP_LINK_OFF);
    CHECK(app_flow_handle(&f, APP_EVENT_BOOT_EMPTY) == APP_ACTION_RENDER);
    CHECK(f.state == APP_STATE_SETUP && f.note == APP_NOTE_NO_SAVED && !f.have_saved);
    CHECK(app_flow_note_text(f.note) != NULL);

    CHECK(app_flow_handle(&f, APP_EVENT_OK) == (APP_ACTION_LISTEN_START | APP_ACTION_RENDER));
    CHECK(f.state == APP_STATE_LISTENING && f.note == APP_NOTE_NONE);

    CHECK(app_flow_handle(&f, APP_EVENT_CREDENTIALS) ==
          (APP_ACTION_LISTEN_STOP | APP_ACTION_RENDER | APP_ACTION_CONNECT));
    CHECK(f.state == APP_STATE_CONNECTING && f.source == APP_SOURCE_SOUND &&
          f.link == APP_LINK_CONNECTING);

    // Once connected the main page returns, and the new credentials are saved.
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_CONNECTED) == (APP_ACTION_SAVE | APP_ACTION_RENDER));
    CHECK(f.state == APP_STATE_HEATMAP && f.link == APP_LINK_UP);

    // OK opens the Wi-Fi setup page and turns the radio off.
    CHECK(app_flow_handle(&f, APP_EVENT_OK) ==
          (APP_ACTION_WIFI_STOP | APP_ACTION_FETCH_CANCEL | APP_ACTION_RENDER));
    CHECK(f.state == APP_STATE_SETUP && f.link == APP_LINK_OFF && f.note == APP_NOTE_NONE);
}

static void test_saved_boot_runs_in_background(void)
{
    app_flow_t f;
    app_flow_init(&f);
    // With saved credentials the main page opens at once.
    CHECK(app_flow_handle(&f, APP_EVENT_BOOT_SAVED) == (APP_ACTION_RENDER | APP_ACTION_CONNECT));
    CHECK(f.state == APP_STATE_HEATMAP && f.have_saved && f.source == APP_SOURCE_SAVED &&
          f.link == APP_LINK_CONNECTING);
    // Saved credentials that connect are not written again.
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_CONNECTED) == APP_ACTION_LINK);
    CHECK(f.state == APP_STATE_HEATMAP && f.link == APP_LINK_UP);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_CONNECTED) == 0);

    // A dropped link reconnects with the same credentials.
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_LOST) == (APP_ACTION_LINK | APP_ACTION_CONNECT));
    CHECK(f.link == APP_LINK_CONNECTING && f.source == APP_SOURCE_RECONNECT);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_LOST) == 0);

    // A failed attempt waits, then retries; the page never changes.
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_FAILED) ==
          (APP_ACTION_WIFI_STOP | APP_ACTION_RETRY_LATER | APP_ACTION_LINK));
    CHECK(f.state == APP_STATE_HEATMAP && f.link == APP_LINK_WAITING);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_FAILED) == 0);
    CHECK(app_flow_handle(&f, APP_EVENT_RETRY) == (APP_ACTION_LINK | APP_ACTION_CONNECT));
    CHECK(f.link == APP_LINK_CONNECTING && f.source == APP_SOURCE_RECONNECT);
    CHECK(app_flow_handle(&f, APP_EVENT_RETRY) == 0);

    // UP/DOWN on the main page scroll the calendar, not the flow.
    CHECK(app_flow_handle(&f, APP_EVENT_BACK) == 0);
    CHECK(app_flow_handle(&f, APP_EVENT_CREDENTIALS) == 0);
    CHECK(f.state == APP_STATE_HEATMAP);
}

static void test_back_to_heatmap(void)
{
    // Back from every setup page; with saved credentials it reconnects.
    app_flow_t f = booted(true);
    app_flow_handle(&f, APP_EVENT_OK);
    CHECK(f.state == APP_STATE_SETUP);
    CHECK(app_flow_handle(&f, APP_EVENT_BACK) == (APP_ACTION_RENDER | APP_ACTION_CONNECT));
    CHECK(f.state == APP_STATE_HEATMAP && f.link == APP_LINK_CONNECTING && f.source == APP_SOURCE_SAVED);

    app_flow_handle(&f, APP_EVENT_OK);
    app_flow_handle(&f, APP_EVENT_OK);
    CHECK(f.state == APP_STATE_LISTENING);
    CHECK(app_flow_handle(&f, APP_EVENT_BACK) ==
          (APP_ACTION_LISTEN_STOP | APP_ACTION_RENDER | APP_ACTION_CONNECT));
    CHECK(f.state == APP_STATE_HEATMAP);

    // Leaving an attempt with new credentials returns to the saved ones.
    app_flow_handle(&f, APP_EVENT_OK);
    app_flow_handle(&f, APP_EVENT_OK);
    app_flow_handle(&f, APP_EVENT_CREDENTIALS);
    CHECK(f.state == APP_STATE_CONNECTING);
    CHECK(app_flow_handle(&f, APP_EVENT_BACK) ==
          (APP_ACTION_WIFI_STOP | APP_ACTION_RENDER | APP_ACTION_CONNECT));
    CHECK(f.state == APP_STATE_HEATMAP && f.source == APP_SOURCE_SAVED);

    // Without saved credentials the main page opens offline.
    f = booted(false);
    CHECK(app_flow_handle(&f, APP_EVENT_BACK) == APP_ACTION_RENDER);
    CHECK(f.state == APP_STATE_HEATMAP && f.link == APP_LINK_OFF);
    CHECK(app_flow_handle(&f, APP_EVENT_RETRY) == 0);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_CONNECTED) == 0);
    CHECK(app_flow_handle(&f, APP_EVENT_OK) ==
          (APP_ACTION_WIFI_STOP | APP_ACTION_FETCH_CANCEL | APP_ACTION_RENDER));
    CHECK(f.state == APP_STATE_SETUP);
}

static void test_failure_loop(void)
{
    app_flow_t f = booted(false);
    app_flow_handle(&f, APP_EVENT_OK);
    app_flow_handle(&f, APP_EVENT_CREDENTIALS);
    // Failed sound credentials are never saved.
    const uint32_t actions = app_flow_handle(&f, APP_EVENT_WIFI_FAILED);
    CHECK(actions == (APP_ACTION_WIFI_STOP | APP_ACTION_RENDER));
    CHECK(f.state == APP_STATE_FAILED && f.link == APP_LINK_OFF);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_LOST) == 0);
    CHECK(app_flow_handle(&f, APP_EVENT_OK) == APP_ACTION_RENDER);
    CHECK(f.state == APP_STATE_SETUP && f.note == APP_NOTE_NONE);

    app_flow_handle(&f, APP_EVENT_OK);
    app_flow_handle(&f, APP_EVENT_CREDENTIALS);
    app_flow_handle(&f, APP_EVENT_WIFI_FAILED);
    CHECK(app_flow_handle(&f, APP_EVENT_BACK) == APP_ACTION_RENDER);
    CHECK(f.state == APP_STATE_HEATMAP && f.link == APP_LINK_OFF);

    // Stopping an attempt with OK returns to the setup page.
    f = booted(false);
    app_flow_handle(&f, APP_EVENT_OK);
    app_flow_handle(&f, APP_EVENT_CREDENTIALS);
    CHECK(app_flow_handle(&f, APP_EVENT_OK) == (APP_ACTION_WIFI_STOP | APP_ACTION_RENDER));
    CHECK(f.state == APP_STATE_SETUP && f.note == APP_NOTE_CONNECT_STOPPED);
    // A late Wi-Fi result must not leave the setup page.
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_CONNECTED) == 0);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_FAILED) == 0);
    CHECK(f.state == APP_STATE_SETUP);
}

static void test_listening_exits(void)
{
    const struct {
        app_event_t event;
        app_note_t note;
    } cases[] = {
        { APP_EVENT_OK, APP_NOTE_LISTEN_CANCELLED },
        { APP_EVENT_LISTEN_TIMEOUT, APP_NOTE_LISTEN_TIMEOUT },
        { APP_EVENT_LISTEN_FAILED, APP_NOTE_AUDIO_ERROR },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        app_flow_t f = booted(false);
        app_flow_handle(&f, APP_EVENT_OK);
        CHECK(app_flow_handle(&f, cases[i].event) == (APP_ACTION_LISTEN_STOP | APP_ACTION_RENDER));
        CHECK(f.state == APP_STATE_SETUP && f.note == cases[i].note);
        CHECK(app_flow_note_text(f.note) != NULL);
    }
    app_flow_t f = booted(false);
    app_flow_handle(&f, APP_EVENT_OK);
    // Stale Wi-Fi events while listening are ignored.
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_CONNECTED) == 0);
    CHECK(app_flow_handle(&f, APP_EVENT_WIFI_LOST) == 0);
    CHECK(app_flow_handle(&f, APP_EVENT_RETRY) == 0);
    CHECK(f.state == APP_STATE_LISTENING);
    CHECK(app_flow_note_text(APP_NOTE_NONE) == NULL);
}

static void test_retry_delay(void)
{
    CHECK(app_retry_delay_ms(15000, 300000, 0) == 15000);
    CHECK(app_retry_delay_ms(15000, 300000, 1) == 15000);
    CHECK(app_retry_delay_ms(15000, 300000, 2) == 30000);
    CHECK(app_retry_delay_ms(15000, 300000, 5) == 240000);
    CHECK(app_retry_delay_ms(15000, 300000, 6) == 300000);
    CHECK(app_retry_delay_ms(15000, 300000, 255) == 300000);
    CHECK(app_retry_delay_ms(30000, 600000, 3) == 120000);
    CHECK(app_retry_delay_ms(4000000000u, 4100000000u, 9) == 4100000000u);
    CHECK(app_retry_delay_ms(500, 100, 1) == 100);

    // History restored from flash is refreshed once per power-on, even though
    // the boot clock restarts at 0; after that every period.
    const int64_t period = 30LL * 60 * 1000000;
    CHECK(app_refresh_due(false, 0, 0, period));
    CHECK(app_refresh_due(false, 5 * 60 * 1000000LL, 0, period));
    CHECK(!app_refresh_due(true, period - 1, 0, period));
    CHECK(app_refresh_due(true, period, 0, period));
    CHECK(!app_refresh_due(true, 2 * period - 1, period, period));
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
    test_first_boot_to_heatmap_and_save();
    test_saved_boot_runs_in_background();
    test_back_to_heatmap();
    test_failure_loop();
    test_listening_exits();
    test_retry_delay();
    test_policy();
    test_ssid_text();
    if (s_failures) {
        fprintf(stderr, "test_app_flow: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_app_flow: PASS\n");
    return 0;
}
