# GitHub contribution heatmap application. The Wi-Fi by sound module
# (app_ui, app_text, sonic_listener, wifi_link, wifi_policy and the
# components/sonic_link receiver) provides the Wi-Fi setup pages. The baseline
# hardware-test pages (demo_*.c, ui_pixel*, demo_navigation*) stay in this
# directory for the upstream host tests but are not part of this firmware;
# demo_radio.c is reused for the one-time NVS/netif/event-loop setup.
idf_component_register(
    SRCS "main.c"
         "app_flow.c"
         "app_text.c"
         "app_ui.c"
         "gh_fetch.c"
         "gh_parse.c"
         "hm_calendar.c"
         "hm_nvs.c"
         "hm_store.c"
         "hm_ui.c"
         "hm_view.c"
         "sonic_listener.c"
         "wifi_link.c"
         "wifi_policy.c"
         "work_bar.c"
         "clock_sync.c"
         "demo_radio.c"
    INCLUDE_DIRS "."
    REQUIRES bsp sonic_link esp_event esp_http_client esp_netif esp_timer esp_wifi mbedtls nvs_flash
)
