idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_event app_update storage usb_printer gcode_stream net web
)
