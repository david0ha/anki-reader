idf_component_register(
    SRCS "board_io.c"
    PRIV_REQUIRES
        esp_driver_i2c
        esp_adc
    INCLUDE_DIRS "./")
