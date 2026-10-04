/*
 * Dry-run transport: a small simulated Marlin.
 *
 * It behaves like the real firmware where it matters for the host logic:
 * prints "start" + banner on "connect", validates N<line> numbers and
 * checksums (Error / Resend / ok), answers M105/M114/M115, auto-reports
 * temperatures with M155, blocks on M109/M190/G28 with busy/temperature
 * output, and can inject checksum errors (CONFIG_CR10_DRY_RUN_ERROR_EVERY).
 */

#include "usb_printer.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "printer_sim";

static StreamBufferHandle_t s_rx; /* sim -> host */
static StreamBufferHandle_t s_tx; /* host -> sim */
static volatile bool s_connected;
static usb_printer_conn_cb_t s_cb;
static void *s_cb_arg;

static struct {
    float he, he_t, bed, bed_t;
    float pos[4]; /* X Y Z E */
    bool rel_xyz, rel_e;
    long last_n;
    uint32_t lines;
    int autoreport_s;
    int64_t next_report_us;
    int64_t last_tick_us;
} m;

static void out(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        xStreamBufferSend(s_rx, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1, pdMS_TO_TICKS(100));
    }
}

static void report_temps(const char *prefix)
{
    out("%sT:%.2f /%.2f B:%.2f /%.2f @:0 B@:0\n", prefix, m.he, m.he_t, m.bed, m.bed_t);
}

/* Thermal model + periodic auto-report. */
static void tick(void)
{
    int64_t now = esp_timer_get_time();
    float dt = (now - m.last_tick_us) / 1e6f;
    m.last_tick_us = now;
    float step = 8.0f * dt;
    float *cur[2] = {&m.he, &m.bed};
    float tgt[2] = {m.he_t > 0 ? m.he_t : 22.0f, m.bed_t > 0 ? m.bed_t : 22.0f};
    for (int i = 0; i < 2; i++) {
        float d = tgt[i] - *cur[i];
        *cur[i] += d > step ? step : (d < -step ? -step : d);
    }
    if (m.autoreport_s > 0 && now >= m.next_report_us) {
        m.next_report_us = now + (int64_t)m.autoreport_s * 1000000;
        report_temps(" ");
    }
}

static bool param(const char *cmd, char letter, float *val)
{
    for (const char *p = cmd; *p; p++) {
        if (toupper((unsigned char)*p) == letter && (p == cmd || p[-1] == ' ' || isdigit((unsigned char)p[-1]) || p[-1] == '.')) {
            char *end;
            float v = strtof(p + 1, &end);
            if (end != p + 1) {
                *val = v;
                return true;
            }
        }
    }
    return false;
}

static void wait_temp(float *cur, float target)
{
    int64_t next = 0;
    while (target > 0 && (*cur < target - 1.0f || *cur > target + 3.0f)) {
        vTaskDelay(pdMS_TO_TICKS(100));
        tick();
        int64_t now = esp_timer_get_time();
        if (now >= next) {
            next = now + 1000000;
            report_temps(" ");
        }
    }
}

static void exec(const char *cmd)
{
    float v;
    char code[8] = {0};
    sscanf(cmd, "%7s", code);
    for (char *p = code; *p; p++) {
        *p = toupper((unsigned char)*p);
    }

    if (!strcmp(code, "M105")) {
        report_temps("ok ");
        return;
    } else if (!strcmp(code, "M114")) {
        out("X:%.2f Y:%.2f Z:%.2f E:%.2f Count X:0 Y:0 Z:0\n", m.pos[0], m.pos[1], m.pos[2], m.pos[3]);
    } else if (!strcmp(code, "M115")) {
        out("FIRMWARE_NAME:Marlin 1.1.9 (Dry Run) SOURCE_CODE_URL:local PROTOCOL_VERSION:1.0 "
            "MACHINE_TYPE:CR-10 EXTRUDER_COUNT:1\nCap:AUTOREPORT_TEMP:1\n");
    } else if (!strcmp(code, "M110")) {
        m.last_n = param(cmd + 4, 'N', &v) ? (long)v : 0;
    } else if (!strcmp(code, "M155")) {
        m.autoreport_s = param(cmd + 4, 'S', &v) ? (int)v : 0;
        m.next_report_us = esp_timer_get_time();
    } else if (!strcmp(code, "M104") || !strcmp(code, "M109")) {
        if (param(cmd + 4, 'S', &v)) {
            m.he_t = v;
        }
        if (code[3] == '9') {
            wait_temp(&m.he, m.he_t);
        }
    } else if (!strcmp(code, "M140") || !strcmp(code, "M190")) {
        if (param(cmd + 4, 'S', &v)) {
            m.bed_t = v;
        }
        if (code[3] == '0' && code[2] == '9') {
            wait_temp(&m.bed, m.bed_t);
        }
    } else if (!strcmp(code, "G28")) {
        for (int i = 0; i < 2; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            out("echo:busy: processing\n");
        }
        m.pos[0] = m.pos[1] = m.pos[2] = 0;
    } else if (!strcmp(code, "G90")) {
        m.rel_xyz = false;
    } else if (!strcmp(code, "G91")) {
        m.rel_xyz = true;
    } else if (!strcmp(code, "M82")) {
        m.rel_e = false;
    } else if (!strcmp(code, "M83")) {
        m.rel_e = true;
    } else if (!strcmp(code, "G0") || !strcmp(code, "G1") || !strcmp(code, "G92")) {
        static const char axes[4] = {'X', 'Y', 'Z', 'E'};
        bool g92 = code[1] == '9';
        for (int i = 0; i < 4; i++) {
            if (param(cmd + strlen(code), axes[i], &v)) {
                bool rel = !g92 && (i == 3 ? (m.rel_e || m.rel_xyz) : m.rel_xyz);
                m.pos[i] = rel ? m.pos[i] + v : v;
            }
        }
    }
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CR10_DRY_RUN_DELAY_MS));
    out("ok\n");
}

static void request_resend(const char *why)
{
    out("Error:%s, Last Line: %ld\nResend: %ld\nok\n", why, m.last_n, m.last_n + 1);
}

/* Mirrors Marlin's line validation (gcode_line_error / FlushSerialRequestResend). */
static void handle_line(char *line)
{
    while (*line == ' ') {
        line++;
    }
    if (!*line) {
        return;
    }
    if (*line == 'N' || *line == 'n') {
        char *end;
        long n = strtol(line + 1, &end, 10);
        char *star = strchr(line, '*');
        if (!star) {
            request_resend("No Checksum with line number");
            return;
        }
        uint8_t cs = 0;
        for (char *p = line; p < star; p++) {
            cs ^= (uint8_t)*p;
        }
        int want = atoi(star + 1);
        *star = 0;
        while (*end == ' ') {
            end++;
        }
        bool is_m110 = strncasecmp(end, "M110", 4) == 0;
        if (n != m.last_n + 1 && !is_m110) {
            request_resend("Line Number is not Last Line Number+1");
            return;
        }
        m.lines++;
        bool inject = CONFIG_CR10_DRY_RUN_ERROR_EVERY > 0 && (m.lines % CONFIG_CR10_DRY_RUN_ERROR_EVERY) == 0;
        if (cs != want || inject) {
            request_resend("checksum mismatch");
            return;
        }
        m.last_n = n;
        exec(end);
    } else {
        exec(line);
    }
}

static void sim_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    memset(&m, 0, sizeof(m));
    m.he = m.bed = 22.0f;
    m.last_tick_us = esp_timer_get_time();
    s_connected = true;
    ESP_LOGW(TAG, "DRY RUN: simulated printer connected");
    if (s_cb) {
        s_cb(true, s_cb_arg);
    }
    vTaskDelay(pdMS_TO_TICKS(800)); /* "bootloader" */
    out("start\necho:Marlin 1.1.9 (Dry Run)\necho: Last Updated: simulated\necho:SD init fail\n");

    char line[128];
    size_t len = 0;
    for (;;) {
        uint8_t buf[64];
        size_t n = xStreamBufferReceive(s_tx, buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (size_t i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                line[len] = 0;
                if (len) {
                    handle_line(line);
                }
                len = 0;
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
            }
        }
        tick();
    }
}

esp_err_t usb_printer_init(usb_printer_conn_cb_t cb, void *arg)
{
    s_cb = cb;
    s_cb_arg = arg;
    s_rx = xStreamBufferCreate(4096, 1);
    s_tx = xStreamBufferCreate(2048, 1);
    if (!s_rx || !s_tx) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(sim_task, "printer_sim", 4096, NULL, 5, NULL);
    return ESP_OK;
}

bool usb_printer_connected(void)
{
    return s_connected;
}

size_t usb_printer_read(uint8_t *buf, size_t max_len, TickType_t timeout)
{
    return xStreamBufferReceive(s_rx, buf, max_len, timeout);
}

esp_err_t usb_printer_write(const void *data, size_t len, uint32_t timeout_ms)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t sent = xStreamBufferSend(s_tx, data, len, pdMS_TO_TICKS(timeout_ms));
    return sent == len ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint32_t usb_printer_rx_dropped(void)
{
    return 0;
}

const char *usb_printer_desc(void)
{
    return "dry run (simulated Marlin)";
}
