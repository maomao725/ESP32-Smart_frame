idf_component_register(
    SRCS "mqtt_photo_client.c"
    INCLUDE_DIRS "."
    REQUIRES
        mqtt
        json
        photo_client
)
