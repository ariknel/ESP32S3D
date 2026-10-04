idf_component_register(
    SRCS "web.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "www/index.html" "www/setup.html"
    REQUIRES esp_common
    PRIV_REQUIRES esp_http_server app_update esp_app_format esp_timer esp_system
                  storage gcode_stream net log
)
