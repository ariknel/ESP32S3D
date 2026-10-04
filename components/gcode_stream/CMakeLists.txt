idf_component_register(
    SRCS "gcode_stream.c" "gcode_rx.c"
    INCLUDE_DIRS "include"
    REQUIRES storage
    PRIV_REQUIRES usb_printer esp_timer log
)
