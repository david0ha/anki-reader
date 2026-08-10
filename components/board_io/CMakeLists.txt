idf_component_register(
    SRCS "board_io.c"
    PRIV_REQUIRES
        esp_adc
        esp_driver_gpio
    INCLUDE_DIRS "./")
