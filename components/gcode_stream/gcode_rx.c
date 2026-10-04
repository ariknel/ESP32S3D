/*
 * RX task: splits the printer's byte stream into lines and classifies them.
 * State that only needs displaying (temperatures, position, firmware name)
 * is written straight into the status; anything that affects the protocol
 * is forwarded to the comm task in arrival order.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gs_internal.h"
#include "usb_printer.h"

static const char *TAG = "gs_rx";

static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_rx_us;

int64_t gs_last_rx_us(void)
{
    taskENTER_CRITICAL(&s_rx_mux);
    int64_t t = s_last_rx_us;
    taskEXIT_CRITICAL(&s_rx_mux);
    return t;
}

/* Finds "<key>" (e.g. "T:") at a word boundary and parses "cur /target". */
static bool parse_temp(const char *line, const char *key, float *cur, float *target)
{
    size_t klen = strlen(key);
    for (const char *p = strstr(line, key); p; p = strstr(p + 1, key)) {
        if (p != line && p[-1] != ' ') {
            continue; /* e.g. the "B" inside "B@:" or "T0:" handled by key */
        }
        char *end;
        float c = strtof(p + klen, &end);
        if (end == p + klen) {
            continue;
        }
        *cur = c;
        while (*end == ' ') {
            end++;
        }
        if (*end == '/') {
            *target = strtof(end + 1, NULL);
        }
        return true;
    }
    return false;
}

/* Returns true if the line carried temperatures. */
static bool handle_temps(const char *line)
{
    float he = 0, he_t = -1, bed = 0, bed_t = -1;
    bool got_he = parse_temp(line, "T:", &he, &he_t) || parse_temp(line, "T0:", &he, &he_t);
    bool got_bed = parse_temp(line, "B:", &bed, &bed_t);
    if (!got_he && !got_bed) {
        return false;
    }
    xSemaphoreTake(g_gs_lock, portMAX_DELAY);
    if (got_he) {
        g_gs_status.hotend = he;
        if (he_t >= 0) {
            g_gs_status.hotend_target = he_t;
        }
    }
    if (got_bed) {
        g_gs_status.bed = bed;
        if (bed_t >= 0) {
            g_gs_status.bed_target = bed_t;
        }
    }
    g_gs_status.temps_valid = true;
    xSemaphoreGive(g_gs_lock);
    return true;
}

/* "X:10.00 Y:20.00 Z:0.30 E:5.00 Count X: ..." */
static bool handle_position(const char *line)
{
    if (strncmp(line, "X:", 2) != 0 || !strstr(line, "Y:") || !strstr(line, "Z:")) {
        return false;
    }
    static const char *keys[4] = {"X:", "Y:", "Z:", "E:"};
    float pos[4] = {0};
    const char *count = strstr(line, "Count");
    for (int i = 0; i < 4; i++) {
        const char *p = strstr(line, keys[i]);
        if (!p || (count && p > count)) {
            return false;
        }
        pos[i] = strtof(p + 2, NULL);
    }
    xSemaphoreTake(g_gs_lock, portMAX_DELAY);
    memcpy(g_gs_status.pos, pos, sizeof(pos));
    xSemaphoreGive(g_gs_lock);
    return true;
}

static bool is_fatal_error(const char *line)
{
    static const char *const fatal[] = {
        "halted", "kill", "Thermal", "THERMAL", "MINTEMP", "MAXTEMP", "Heating failed", "runaway",
    };
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
        if (strstr(line, fatal[i])) {
            return true;
        }
    }
    return false;
}

static void handle_line(char *line)
{
    /* trim */
    size_t len = strlen(line);
    while (len && isspace((unsigned char)line[len - 1])) {
        line[--len] = 0;
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (!*line) {
        return;
    }

    taskENTER_CRITICAL(&s_rx_mux);
    s_last_rx_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_rx_mux);

    ESP_LOGD(TAG, "< %s", line);

    if (strncmp(line, "ok", 2) == 0 && (line[2] == 0 || line[2] == ' ')) {
        handle_temps(line);
        gs_post(MSG_OK, 0, line);
        return;
    }
    if (strncasecmp(line, "Resend:", 7) == 0 || strncmp(line, "rs ", 3) == 0) {
        const char *p = line + (line[0] == 'r' && line[1] == 's' ? 3 : 7);
        while (*p == ' ' || *p == 'N') {
            p++;
        }
        gs_post(MSG_RESEND, (int32_t)strtol(p, NULL, 10), NULL);
        gs_log("<", line);
        return;
    }
    if (strncmp(line, "echo:busy:", 10) == 0 || strncmp(line, "busy:", 5) == 0) {
        gs_post(MSG_BUSY, 0, NULL);
        return;
    }
    if (strcmp(line, "start") == 0) {
        gs_log("!", "Printer firmware started (reset)");
        gs_post(MSG_BANNER, 0, NULL);
        return;
    }
    if (strncmp(line, "Error:", 6) == 0 || strncmp(line, "!!", 2) == 0) {
        gs_log("!", line);
        /* Checksum / line number errors are followed by Resend: and handled there. */
        if (is_fatal_error(line)) {
            gs_post(MSG_FATAL, 0, line);
        }
        return;
    }
    if (strstr(line, "Unknown command")) {
        gs_log("<", line);
        gs_post(MSG_UNKNOWN_CMD, 0, line);
        return;
    }
    if (strncmp(line, "FIRMWARE_NAME:", 14) == 0) {
        xSemaphoreTake(g_gs_lock, portMAX_DELAY);
        const char *src = line + 14;
        const char *end = strstr(src, " SOURCE_CODE_URL");
        size_t n = end ? (size_t)(end - src) : strlen(src);
        if (n >= sizeof(g_gs_status.firmware)) {
            n = sizeof(g_gs_status.firmware) - 1;
        }
        memcpy(g_gs_status.firmware, src, n);
        g_gs_status.firmware[n] = 0;
        xSemaphoreGive(g_gs_lock);
        gs_log("<", line);
        return;
    }
    if (strncmp(line, "Cap:AUTOREPORT_TEMP:", 20) == 0) {
        gs_post(MSG_CAP_AUTOREPORT, atoi(line + 20), NULL);
        gs_log("<", line);
        return;
    }
    if (handle_position(line)) {
        gs_log("<", line);
        return;
    }
    if (handle_temps(line)) {
        return; /* periodic temperature report: not echoed to the console */
    }
    gs_log("<", line);
}

static void rx_task(void *arg)
{
    char line[256];
    size_t len = 0;
    bool overflow = false;
    uint8_t buf[128];

    for (;;) {
        size_t n = usb_printer_read(buf, sizeof(buf), portMAX_DELAY);
        for (size_t i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n') {
                line[len] = 0;
                if (!overflow) {
                    handle_line(line);
                }
                len = 0;
                overflow = false;
            } else if (c != '\r' && c != 0) {
                if (len < sizeof(line) - 1) {
                    line[len++] = c;
                } else {
                    overflow = true;
                }
            }
        }
    }
}

void gs_rx_start(void)
{
    xTaskCreatePinnedToCore(rx_task, "gs_rx", 4096, NULL, 8, NULL, 1);
}
