#pragma once

#include "esp_err.h"

/* Starts the HTTP server (UI, REST API, WebSocket push, OTA). */
esp_err_t web_start(void);

/* Console sink for gcode_stream (thread-safe, non-blocking). */
void web_log(const char *dir, const char *line);
