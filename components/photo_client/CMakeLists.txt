idf_component_register(
    SRCS "photo_client.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_client
        sdcard_bsp
)
