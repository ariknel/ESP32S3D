/*
 * HTTP server: embedded UI, REST JSON API, WebSocket live updates, uploads
 * streamed to SD, OTA.
 *
 * esp_http_server runs every handler in ONE task. Live updates are therefore
 * queued with httpd_queue_work() and sent from that task. A consequence is
 * that pushes pause while an upload/OTA request is being received; the
 * browser's own XHR progress covers that time.
 */

#include "web.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gcode_stream.h"
#include "net.h"
#include "sdkconfig.h"
#include "storage.h"

static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_end");

#define RECV_CHUNK      4096
#define UPLOAD_TMP      STORAGE_MOUNT_POINT "/.upload.tmp"
#define LOG_RING        64
#define LOG_LINE        112
#define MAX_WS_CLIENTS  6

#ifdef CONFIG_CR10_PRINTER_DRY_RUN
#define DRY_RUN_STR "true"
#else
#define DRY_RUN_STR "false"
#endif

static httpd_handle_t s_server;

/* ======================================================================= */
/* string builder + JSON helpers                                            */

typedef struct {
    char  *buf;
    size_t cap, len;
    bool   oom;
} sb_t;

static bool sb_init(sb_t *sb, size_t cap)
{
    sb->buf = malloc(cap);
    sb->cap = cap;
    sb->len = 0;
    sb->oom = sb->buf == NULL;
    if (sb->buf) {
        sb->buf[0] = 0;
    }
    return !sb->oom;
}

static void sb_grow(sb_t *sb, size_t need)
{
    if (sb->oom || sb->len + need < sb->cap) {
        return;
    }
    size_t cap = sb->cap * 2;
    while (cap <= sb->len + need) {
        cap *= 2;
    }
    char *nb = realloc(sb->buf, cap);
    if (!nb) {
        sb->oom = true;
        return;
    }
    sb->buf = nb;
    sb->cap = cap;
}

__attribute__((format(printf, 2, 3))) static void sb_printf(sb_t *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    sb_grow(sb, n + 1);
    if (sb->oom) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += n;
}

static void sb_json_str(sb_t *sb, const char *s)
{
    sb_grow(sb, strlen(s) * 6 + 3);
    if (sb->oom) {
        return;
    }
    char *o = sb->buf + sb->len;
    *o++ = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            *o++ = '\\';
            *o++ = c;
        } else if (c == '\n') {
            *o++ = '\\';
            *o++ = 'n';
        } else if (c < 0x20) {
            o += sprintf(o, "\\u%04x", c);
        } else {
            *o++ = c;
        }
    }
    *o++ = '"';
    *o = 0;
    sb->len = o - sb->buf;
}

static void sb_free(sb_t *sb)
{
    free(sb->buf);
    sb->buf = NULL;
}

static esp_err_t send_json_sb(httpd_req_t *req, sb_t *sb)
{
    if (sb->oom) {
        sb_free(sb);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, sb->buf, sb->len);
    sb_free(sb);
    return err;
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *error)
{
    sb_t sb;
    sb_init(&sb, 128);
    if (error) {
        sb_printf(&sb, "{\"ok\":false,\"error\":");
        sb_json_str(&sb, error);
        sb_printf(&sb, "}");
        httpd_resp_set_status(req, status);
    } else {
        sb_printf(&sb, "{\"ok\":true}");
    }
    return send_json_sb(req, &sb);
}

static esp_err_t reply_ok(httpd_req_t *req)
{
    return reply(req, "200 OK", NULL);
}

static esp_err_t reply_err(httpd_req_t *req, const char *status, const char *error)
{
    return reply(req, status, error);
}

/* ======================================================================= */
/* query helpers                                                             */

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

/* Reads a decoded query parameter into out. */
static bool query_param(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (!qlen) {
        return false;
    }
    char *q = malloc(qlen + 1);
    if (!q) {
        return false;
    }
    bool ok = httpd_req_get_url_query_str(req, q, qlen + 1) == ESP_OK &&
              httpd_query_key_value(q, key, out, out_len) == ESP_OK;
    free(q);
    if (ok) {
        url_decode(out);
    }
    return ok;
}

/* Reads a small request body (<= max-1 bytes) as a string. */
static int read_body(httpd_req_t *req, char *buf, size_t max)
{
    if (req->content_len >= max) {
        return -1;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    buf[got] = 0;
    return (int)got;
}

/* Upload names: keep safe characters, map the rest to '_'. */
static bool sanitize_name(char *name)
{
    for (char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-' || c == '(' || c == ')' || c == '+')) {
            *p = (c == ' ') ? ' ' : '_';
        }
    }
    while (name[0] == '.' || name[0] == ' ') {
        memmove(name, name + 1, strlen(name));
    }
    size_t len = strlen(name);
    if (len > STORAGE_MAX_NAME) {
        /* keep the extension */
        char *dot = strrchr(name, '.');
        size_t ext = dot ? strlen(dot) : 0;
        if (ext > 8) {
            ext = 0;
        }
        memmove(name + STORAGE_MAX_NAME - ext, name + len - ext, ext + 1);
    }
    return storage_name_valid(name);
}

static void maybe_set_clock(httpd_req_t *req)
{
    char ts[24];
    if (time(NULL) > 1700000000 || !query_param(req, "ts", ts, sizeof(ts))) {
        return; /* SNTP already set the clock, or no hint from the browser */
    }
    long long ms = atoll(ts);
    if (ms > 1700000000000LL) {
        struct timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "clock set from browser");
    }
}

/* ======================================================================= */
/* console log ring (filled by gcode_stream, drained by the WS pusher)      */

typedef struct {
    uint32_t seq;
    char     text[LOG_LINE];
} log_entry_t;

static log_entry_t s_log[LOG_RING];
static uint32_t s_log_seq;   /* seq of the newest entry */
static SemaphoreHandle_t s_log_lock;

void web_log(const char *dir, const char *line)
{
    if (!s_log_lock) {
        return;
    }
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    s_log_seq++;
    log_entry_t *e = &s_log[s_log_seq % LOG_RING];
    e->seq = s_log_seq;
    snprintf(e->text, sizeof(e->text), "%s %s", dir, line);
    xSemaphoreGive(s_log_lock);
}

/* Appends ["...",...] of entries newer than *since; updates *since. */
static void append_logs(sb_t *sb, uint32_t *since)
{
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    uint32_t newest = s_log_seq;
    uint32_t first = *since + 1;
    if (newest >= LOG_RING && first < newest - LOG_RING + 1) {
        first = newest - LOG_RING + 1;
    }
    sb_printf(sb, "[");
    for (uint32_t s = first; s <= newest && s; s++) {
        const log_entry_t *e = &s_log[s % LOG_RING];
        if (s != first) {
            sb_printf(sb, ",");
        }
        sb_json_str(sb, e->text);
    }
    sb_printf(sb, "]");
    *since = newest;
    xSemaphoreGive(s_log_lock);
}

/* ======================================================================= */
/* status JSON                                                               */

static void build_status(sb_t *sb)
{
    gs_status_t st;
    gcode_stream_get_status(&st);
    net_info_t ni;
    net_get_info(&ni);
    static const char *const modes[] = {"sta", "ap", "apsta"};

    float pct = st.size ? 100.0f * st.acked_bytes / st.size : 0;
    sb_printf(sb, "{\"t\":\"status\",\"link\":\"%s\",\"job\":\"%s\",\"file\":",
              gcode_stream_link_str(st.link), gcode_stream_job_str(st.job));
    sb_json_str(sb, st.file);
    sb_printf(sb,
              ",\"size\":%" PRIu32 ",\"sent\":%" PRIu32 ",\"line\":%" PRIu32 ",\"pct\":%.2f,\"elapsed\":%" PRIu32
              ",\"tv\":%s,\"he\":%.1f,\"het\":%.1f,\"bed\":%.1f,\"bedt\":%.1f"
              ",\"pos\":[%.2f,%.2f,%.2f,%.2f],\"resends\":%" PRIu32 ",\"recov\":%" PRIu32 ",\"autorep\":%s,\"err\":",
              st.size, st.acked_bytes, st.file_line, pct, st.elapsed_s, st.temps_valid ? "true" : "false",
              st.hotend, st.hotend_target, st.bed, st.bed_target, st.pos[0], st.pos[1], st.pos[2], st.pos[3],
              st.resends, st.recoveries, st.autoreport ? "true" : "false");
    sb_json_str(sb, st.error);
    sb_printf(sb, ",\"fw\":");
    sb_json_str(sb, st.firmware);
    if (st.checkpoint.valid) {
        sb_printf(sb, ",\"ckpt\":{\"file\":");
        sb_json_str(sb, st.checkpoint.file);
        sb_printf(sb, ",\"state\":");
        sb_json_str(sb, st.checkpoint.state);
        sb_printf(sb, ",\"line\":%" PRIu32 ",\"offset\":%" PRIu32 ",\"size\":%" PRIu32 ",\"reason\":",
                  st.checkpoint.line, st.checkpoint.offset, st.checkpoint.size);
        sb_json_str(sb, st.checkpoint.reason);
        sb_printf(sb, "}");
    } else {
        sb_printf(sb, ",\"ckpt\":null");
    }
    sb_printf(sb, ",\"sd\":%s,\"dry\":%s,\"net\":{\"mode\":\"%s\",\"sta\":%s,\"ip\":\"%s\",\"ssid\":",
              storage_mounted() ? "true" : "false", DRY_RUN_STR,
              modes[ni.mode], ni.sta_connected ? "true" : "false", ni.sta_ip);
    sb_json_str(sb, ni.sta_ssid);
    sb_printf(sb, ",\"rssi\":%d,\"ap\":", ni.rssi);
    sb_json_str(sb, ni.ap_ssid);
    sb_printf(sb, ",\"apip\":\"%s\",\"host\":\"%s\"},\"heap\":%" PRIu32 ",\"up\":%lld,\"ver\":",
              ni.ap_ip, ni.hostname, esp_get_free_heap_size(), esp_timer_get_time() / 1000000);
    sb_json_str(sb, esp_app_get_description()->version);
    sb_printf(sb, "}");
}

/* ======================================================================= */
/* WebSocket push                                                            */

typedef struct {
    size_t len;
    char   data[];
} ws_msg_t;

static volatile bool s_work_pending;

static void ws_send_work(void *arg)
{
    ws_msg_t *m = arg;
    size_t n = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(s_server, &n, fds) == ESP_OK) {
        httpd_ws_frame_t f = {
            .final = true,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)m->data,
            .len = m->len,
        };
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(s_server, fds[i], &f);
            }
        }
    }
    free(m);
    s_work_pending = false;
}

static bool ws_has_clients(void)
{
    size_t n = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(s_server, &n, fds) != ESP_OK) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            return true;
        }
    }
    return false;
}

static void ws_push_task(void *arg)
{
    uint32_t log_since = 0;
    char *last_status = NULL;
    int64_t last_status_us = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (s_work_pending || !ws_has_clients()) {
            if (!ws_has_clients()) {
                xSemaphoreTake(s_log_lock, portMAX_DELAY);
                log_since = s_log_seq; /* nobody listening: drop backlog */
                xSemaphoreGive(s_log_lock);
            }
            continue;
        }
        sb_t sb;
        if (!sb_init(&sb, 1024)) {
            continue;
        }
        /* {"t":"push","status":{...}|null,"log":[...]} */
        sb_t st;
        sb_init(&st, 1024);
        build_status(&st);
        int64_t now = esp_timer_get_time();
        bool status_changed = !st.oom && (!last_status || strcmp(last_status, st.buf) != 0 ||
                                          now - last_status_us > 5000000);
        bool have_logs;
        xSemaphoreTake(s_log_lock, portMAX_DELAY);
        have_logs = s_log_seq != log_since;
        xSemaphoreGive(s_log_lock);

        if (!status_changed && !have_logs) {
            sb_free(&st);
            sb_free(&sb);
            continue;
        }
        sb_printf(&sb, "{\"t\":\"push\",\"status\":%s,\"log\":", status_changed ? st.buf : "null");
        append_logs(&sb, &log_since);
        sb_printf(&sb, "}");
        if (status_changed) {
            free(last_status);
            last_status = st.buf; /* keep for comparison */
            st.buf = NULL;
            last_status_us = now;
        }
        sb_free(&st);

        if (sb.oom) {
            sb_free(&sb);
            continue;
        }
        ws_msg_t *m = malloc(sizeof(ws_msg_t) + sb.len + 1);
        if (m) {
            m->len = sb.len;
            memcpy(m->data, sb.buf, sb.len + 1);
            s_work_pending = true;
            if (httpd_queue_work(s_server, ws_send_work, m) != ESP_OK) {
                free(m);
                s_work_pending = false;
            }
        }
        sb_free(&sb);
    }
}

static esp_err_t h_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket client connected (fd %d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    /* Clients don't send anything meaningful; read and discard frames. */
    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (f.len && f.len < 512) {
        uint8_t buf[512];
        f.payload = buf;
        err = httpd_ws_recv_frame(req, &f, f.len);
    }
    return err;
}

/* ======================================================================= */
/* pages                                                                     */

static esp_err_t send_page(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t h_index(httpd_req_t *req)
{
    return send_page(req, index_html_start, index_html_end);
}

static esp_err_t h_setup(httpd_req_t *req)
{
    return send_page(req, setup_html_start, setup_html_end);
}

/* ======================================================================= */
/* REST API                                                                  */

static esp_err_t h_status(httpd_req_t *req)
{
    sb_t sb;
    sb_init(&sb, 1024);
    build_status(&sb);
    return send_json_sb(req, &sb);
}

static esp_err_t h_log(httpd_req_t *req)
{
    sb_t sb;
    sb_init(&sb, 2048);
    uint32_t since = 0;
    sb_printf(&sb, "{\"log\":");
    append_logs(&sb, &since);
    sb_printf(&sb, "}");
    return send_json_sb(req, &sb);
}

static void list_cb(const storage_file_info_t *info, void *arg)
{
    sb_t *sb = arg;
    if (sb->len && sb->buf[sb->len - 1] == '}') {
        sb_printf(sb, ",");
    }
    sb_printf(sb, "{\"name\":");
    sb_json_str(sb, info->name);
    sb_printf(sb, ",\"size\":%" PRIu32 ",\"mtime\":%lld}", info->size, (long long)info->mtime);
}

static esp_err_t h_files(httpd_req_t *req)
{
    sb_t sb;
    sb_init(&sb, 2048);
    uint64_t total = 0, free_b = 0;
    char q[4];
    if (query_param(req, "space", q, sizeof(q))) {
        storage_card_info(&total, &free_b); /* can be slow on big FAT32 cards */
    }
    sb_printf(&sb, "{\"sd\":%s,\"total\":%llu,\"free\":%llu,\"files\":[", storage_mounted() ? "true" : "false",
              (unsigned long long)total, (unsigned long long)free_b);
    storage_list(list_cb, &sb);
    sb_printf(&sb, "]}");
    return send_json_sb(req, &sb);
}

static esp_err_t h_delete(httpd_req_t *req)
{
    char name[STORAGE_MAX_NAME + 1];
    if (!query_param(req, "name", name, sizeof(name))) {
        return reply_err(req, "400 Bad Request", "missing name");
    }
    if (gcode_stream_job_active()) {
        return reply_err(req, "409 Conflict", "Not allowed while printing");
    }
    esp_err_t err = storage_delete(name);
    if (err != ESP_OK) {
        return reply_err(req, "404 Not Found", esp_err_to_name(err));
    }
    return reply_ok(req);
}

typedef enum { STRIP_NORMAL, STRIP_PASS, STRIP_SKIP } strip_state_t;

typedef struct {
    FILE *f;
    char line[256];
    size_t len;
    strip_state_t st;
    uint32_t written;
    bool err;
} strip_ctx_t;

static void out_write(strip_ctx_t *c, const void *p, size_t n)
{
    if (n && fwrite(p, 1, n, c->f) != n) {
        c->err = true;
    }
    c->written += n;
}

static bool comment_or_blank(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ';') {
            return true;
        }
        if (!isspace((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

/* Drops comment-only and blank lines; everything else is written unchanged. */
static void strip_feed(strip_ctx_t *c, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ch = data[i];
        switch (c->st) {
        case STRIP_PASS:
            out_write(c, &ch, 1);
            if (ch == '\n') {
                c->st = STRIP_NORMAL;
            }
            break;
        case STRIP_SKIP:
            if (ch == '\n') {
                c->st = STRIP_NORMAL;
            }
            break;
        case STRIP_NORMAL:
            c->line[c->len++] = ch;
            if (ch == '\n') {
                if (!comment_or_blank(c->line, c->len)) {
                    out_write(c, c->line, c->len);
                }
                c->len = 0;
            } else if (c->len == sizeof(c->line)) {
                bool drop = comment_or_blank(c->line, c->len);
                if (!drop) {
                    out_write(c, c->line, c->len);
                }
                c->st = drop ? STRIP_SKIP : STRIP_PASS;
                c->len = 0;
            }
            break;
        }
    }
}

static void strip_finish(strip_ctx_t *c)
{
    if (c->st == STRIP_NORMAL && c->len && !comment_or_blank(c->line, c->len)) {
        out_write(c, c->line, c->len);
        out_write(c, "\n", 1);
    }
}

static esp_err_t h_upload(httpd_req_t *req)
{
    char name[STORAGE_MAX_NAME * 3 + 1];
    char strip_s[4] = "0";
    if (!query_param(req, "name", name, sizeof(name)) || !sanitize_name(name)) {
        return reply_err(req, "400 Bad Request", "missing or invalid file name");
    }
    if (gcode_stream_job_active()) {
        return reply_err(req, "409 Conflict", "Uploads are blocked while printing");
    }
    if (!storage_mounted()) {
        return reply_err(req, "503 Service Unavailable", "SD card not mounted");
    }
    query_param(req, "strip", strip_s, sizeof(strip_s));
    bool strip = strip_s[0] == '1';
    maybe_set_clock(req);

    char dest[STORAGE_PATH_MAX];
    storage_path(name, dest, sizeof(dest));

    strip_ctx_t *c = calloc(1, sizeof(*c));
    char *buf = malloc(RECV_CHUNK);
    char *fbuf = malloc(16 * 1024);
    if (!c || !buf || !fbuf) {
        free(c);
        free(buf);
        free(fbuf);
        return reply_err(req, "500 Internal Server Error", "out of memory");
    }
    c->f = fopen(UPLOAD_TMP, "w");
    if (!c->f) {
        free(c);
        free(buf);
        free(fbuf);
        return reply_err(req, "500 Internal Server Error", "cannot create file");
    }
    setvbuf(c->f, fbuf, _IOFBF, 16 * 1024);

    ESP_LOGI(TAG, "upload %s (%u bytes, strip=%d)", name, (unsigned)req->content_len, strip);
    size_t remaining = req->content_len;
    int timeouts = 0;
    const char *fail = NULL;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < RECV_CHUNK ? remaining : RECV_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) {
                fail = "receive timeout";
                break;
            }
            continue;
        }
        if (r <= 0) {
            fail = "connection lost";
            break;
        }
        timeouts = 0;
        remaining -= r;
        if (strip) {
            strip_feed(c, buf, r);
        } else {
            out_write(c, buf, r);
        }
        if (c->err) {
            fail = "SD write failed (card full?)";
            break;
        }
    }
    if (!fail && strip) {
        strip_finish(c);
    }
    if (fclose(c->f) != 0 && !fail) {
        fail = "SD write failed";
    }
    uint32_t written = c->written;
    free(c);
    free(buf);
    free(fbuf);

    if (fail) {
        unlink(UPLOAD_TMP);
        ESP_LOGE(TAG, "upload failed: %s", fail);
        return reply_err(req, "500 Internal Server Error", fail);
    }
    unlink(dest);
    if (rename(UPLOAD_TMP, dest) != 0) {
        unlink(UPLOAD_TMP);
        return reply_err(req, "500 Internal Server Error", "rename failed");
    }
    ESP_LOGI(TAG, "upload done: %s, %" PRIu32 " bytes written", name, written);

    sb_t sb;
    sb_init(&sb, 160);
    sb_printf(&sb, "{\"ok\":true,\"name\":");
    sb_json_str(&sb, name);
    sb_printf(&sb, ",\"size\":%" PRIu32 ",\"received\":%u}", written, (unsigned)req->content_len);
    return send_json_sb(req, &sb);
}

static esp_err_t gs_result(httpd_req_t *req, esp_err_t err, const char *why)
{
    if (err == ESP_OK) {
        return reply_ok(req);
    }
    return reply_err(req, "409 Conflict", why[0] ? why : esp_err_to_name(err));
}

static esp_err_t h_print(httpd_req_t *req)
{
    char name[STORAGE_MAX_NAME + 1];
    char why[96] = "";
    if (!query_param(req, "name", name, sizeof(name))) {
        return reply_err(req, "400 Bad Request", "missing name");
    }
    return gs_result(req, gcode_stream_start(name, why, sizeof(why)), why);
}

static esp_err_t h_pause(httpd_req_t *req)
{
    char why[96] = "";
    return gs_result(req, gcode_stream_pause(why, sizeof(why)), why);
}

static esp_err_t h_resume(httpd_req_t *req)
{
    char why[96] = "";
    return gs_result(req, gcode_stream_resume(why, sizeof(why)), why);
}

static esp_err_t h_stop(httpd_req_t *req)
{
    char why[96] = "";
    return gs_result(req, gcode_stream_stop(why, sizeof(why)), why);
}

/* Body: one or more G-code lines. */
static esp_err_t h_gcode(httpd_req_t *req)
{
    char body[1024];
    char why[96] = "";
    if (read_body(req, body, sizeof(body)) < 0) {
        return reply_err(req, "400 Bad Request", "body too large");
    }
    char *save = NULL;
    for (char *line = strtok_r(body, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        esp_err_t err = gcode_stream_console(line, why, sizeof(why));
        if (err != ESP_OK) {
            return gs_result(req, err, why);
        }
    }
    return reply_ok(req);
}

static esp_err_t h_preheat(httpd_req_t *req)
{
    char m[8] = "";
    char why[96] = "";
    char cmd[2][24];
    query_param(req, "m", m, sizeof(m));
    if (!strcmp(m, "pla")) {
        snprintf(cmd[0], sizeof(cmd[0]), "M104 S%d", CONFIG_CR10_PREHEAT_PLA_HOTEND);
        snprintf(cmd[1], sizeof(cmd[1]), "M140 S%d", CONFIG_CR10_PREHEAT_PLA_BED);
    } else if (!strcmp(m, "petg")) {
        snprintf(cmd[0], sizeof(cmd[0]), "M104 S%d", CONFIG_CR10_PREHEAT_PETG_HOTEND);
        snprintf(cmd[1], sizeof(cmd[1]), "M140 S%d", CONFIG_CR10_PREHEAT_PETG_BED);
    } else if (!strcmp(m, "cool")) {
        strcpy(cmd[0], "M104 S0");
        strcpy(cmd[1], "M140 S0");
    } else {
        return reply_err(req, "400 Bad Request", "m must be pla, petg or cool");
    }
    if (gcode_stream_job_active()) {
        return reply_err(req, "409 Conflict", "Not allowed while printing");
    }
    for (int i = 0; i < 2; i++) {
        esp_err_t err = gcode_stream_console(cmd[i], why, sizeof(why));
        if (err != ESP_OK) {
            return gs_result(req, err, why);
        }
    }
    return reply_ok(req);
}

static void restart_cb(void *arg)
{
    esp_restart();
}

static void schedule_restart(void)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = restart_cb, .name = "restart"};
    if (!t && esp_timer_create(&a, &t) != ESP_OK) {
        esp_restart();
    }
    esp_timer_start_once(t, 1500 * 1000);
}

/* Body: ssid=..&pass=.. (form encoded) */
static esp_err_t h_wifi(httpd_req_t *req)
{
    char body[256];
    char ssid[64] = "", pass[128] = "";
    if (read_body(req, body, sizeof(body)) < 0) {
        return reply_err(req, "400 Bad Request", "bad body");
    }
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);
    if (!ssid[0]) {
        net_forget_credentials();
        reply_ok(req);
        schedule_restart();
        return ESP_OK;
    }
    if (pass[0] && strlen(pass) < 8) {
        return reply_err(req, "400 Bad Request", "WPA password must be at least 8 characters");
    }
    esp_err_t err = net_save_credentials(ssid, pass);
    if (err != ESP_OK) {
        return reply_err(req, "400 Bad Request", esp_err_to_name(err));
    }
    reply_ok(req);
    schedule_restart();
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    if (gcode_stream_job_active()) {
        return reply_err(req, "409 Conflict", "Not allowed while printing");
    }
    reply_ok(req);
    schedule_restart();
    return ESP_OK;
}

static esp_err_t h_ota(httpd_req_t *req)
{
    if (gcode_stream_job_active()) {
        return reply_err(req, "409 Conflict", "Firmware update blocked while printing");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return reply_err(req, "500 Internal Server Error", "no OTA partition");
    }
    if (req->content_len == 0 || req->content_len > part->size) {
        return reply_err(req, "400 Bad Request", "image size invalid");
    }
    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        return reply_err(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "OTA: writing %u bytes to %s", (unsigned)req->content_len, part->label);
    char *buf = malloc(RECV_CHUNK);
    size_t remaining = req->content_len;
    int timeouts = 0;
    while (buf && remaining > 0 && err == ESP_OK) {
        int r = httpd_req_recv(req, buf, remaining < RECV_CHUNK ? remaining : RECV_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 5) {
            continue;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        err = esp_ota_write(ota, buf, r);
        remaining -= r;
    }
    free(buf);
    if (!buf) {
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        esp_ota_abort(ota);
        return reply_err(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    err = esp_ota_end(ota);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    if (err != ESP_OK) {
        return reply_err(req, "400 Bad Request", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "OTA complete, rebooting into %s", part->label);
    reply_ok(req);
    schedule_restart();
    return ESP_OK;
}

/* Captive portal: in setup-AP mode every unknown URL (phone connectivity
 * checks like /generate_204 or /hotspot-detect.html) goes to the setup page. */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t code)
{
    if (net_ap_active()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, "Redirect to setup", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

/* ======================================================================= */

esp_err_t web_start(void)
{
    s_log_lock = xSemaphoreCreateMutex();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 20;
    cfg.max_open_sockets = MAX_WS_CLIENTS + 1;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.core_id = 0;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/setup", .method = HTTP_GET, .handler = h_setup},
        {.uri = "/ws", .method = HTTP_GET, .handler = h_ws, .is_websocket = true},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/log", .method = HTTP_GET, .handler = h_log},
        {.uri = "/api/files", .method = HTTP_GET, .handler = h_files},
        {.uri = "/api/upload", .method = HTTP_POST, .handler = h_upload},
        {.uri = "/api/delete", .method = HTTP_POST, .handler = h_delete},
        {.uri = "/api/print", .method = HTTP_POST, .handler = h_print},
        {.uri = "/api/pause", .method = HTTP_POST, .handler = h_pause},
        {.uri = "/api/resume", .method = HTTP_POST, .handler = h_resume},
        {.uri = "/api/stop", .method = HTTP_POST, .handler = h_stop},
        {.uri = "/api/gcode", .method = HTTP_POST, .handler = h_gcode},
        {.uri = "/api/preheat", .method = HTTP_POST, .handler = h_preheat},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = h_ota},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, h_404);

    xTaskCreatePinnedToCore(ws_push_task, "ws_push", 4096, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
