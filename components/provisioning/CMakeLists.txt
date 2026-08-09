idf_component_register(
    SRCS
        "prov_config.c"
        "form_parse.c"
        "prov_json.c"
        "prov_store.c"
        "prov_wifi.c"
        "prov_portal.c"
        "provisioning.c"
        "net_time.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "portal.html"
    PRIV_REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_http_server
        nvs_flash
        lwip)
