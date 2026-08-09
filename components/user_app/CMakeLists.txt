idf_component_register(
    SRCS "user_app.cpp"
    # Public deps: user_app.h exposes prov_config_t (provisioning); user_app_api.h exposes
    # device_state_t (fortune_core) to the companion-app HTTP server (components/device_api).
    REQUIRES provisioning fortune_core
    PRIV_REQUIRES app_bsp buttons port_bsp esp_timer board_io
    INCLUDE_DIRS "./")
