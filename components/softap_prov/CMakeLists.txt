idf_component_register(
    SRCS "softap_prov.c"
    INCLUDE_DIRS "."
    EMBED_FILES "html/index.html"
    REQUIRES
        esp_wifi
        esp_event
        esp_http_server
        esp_netif
        json
        epaper_src
        epaper_port
)
