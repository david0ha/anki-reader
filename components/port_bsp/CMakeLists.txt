idf_component_register(
    SRCS
    "epd_panel.c"
    # Public: epd_panel.h names spi_host_device_t in epd_pins_t, so anyone who
    # includes it needs the SPI driver's headers too.
    REQUIRES
    esp_driver_spi
    PRIV_REQUIRES
    driver
    esp_lcd
    esp_timer
    INCLUDE_DIRS
    "./")
