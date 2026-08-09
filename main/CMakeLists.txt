idf_component_register(
    SRCS "main.cpp"
    # No explicit REQUIRES: the `main` component implicitly depends on every other component in
    # the build (incl. device_api / user_app / provisioning), which is how main.cpp reaches
    # esp_timer.h, board_io.h, etc. Listing one requirement here would opt out of that implicit
    # behavior and force every include to be declared.
    INCLUDE_DIRS "./")
