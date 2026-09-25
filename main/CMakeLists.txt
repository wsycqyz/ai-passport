# "Wi-Fi by sound" application. The baseline hardware-test pages (demo_*.c,
# ui_pixel*, demo_navigation*) stay in this directory for the upstream host
# tests but are not part of this firmware; demo_radio.c is reused for the
# one-time NVS/netif/event-loop setup.
idf_component_register(
    SRCS "main.c"
         "app_flow.c"
         "app_text.c"
         "app_ui.c"
         "sonic_listener.c"
         "wifi_link.c"
         "wifi_policy.c"
         "demo_radio.c"
    INCLUDE_DIRS "."
    REQUIRES bsp sonic_link esp_event esp_netif esp_timer esp_wifi nvs_flash
)
