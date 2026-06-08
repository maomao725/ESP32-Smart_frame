if(CONFIG_BT_ENABLED AND CONFIG_BT_NIMBLE_ENABLED)
    idf_component_register(
        SRCS "ble_prov.c"
        INCLUDE_DIRS "."
        REQUIRES
            bt
            epaper_src
            epaper_port
            json
            nvs_flash
    )
else()
    idf_component_register(INCLUDE_DIRS ".")
endif()
