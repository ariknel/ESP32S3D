/*
 * Comm / print task: the only place that sends bytes to Marlin.
 *
 * Line accounting
 * ---------------
 * Every numbered line is stored in a history ring indexed by its line number.
 * Lines are always transmitted in ascending order, so the lines currently in
 * flight are the contiguous range [ack_n, sent_hi):
 *   - each "ok" acknowledges ack_n and advances it,
 *   - "Resend: N" rewinds ack_n = sent_hi = N; the lines N..next_n-1 are then
 *     replayed from history before anything new is sent.
 * Marlin answers EVERY line it rejected with its own Error/Resend/ok, so on a
 * resend all oks still owed for the rejected lines are "swallowed", and the
 * duplicate Resend requests arriving meanwhile are ignored.
 *
 * Timeouts
 * --------
 * If no ok/busy arrives for CONFIG_CR10_GCODE_OK_TIMEOUT_MS while lines are in
 * flight:
 *   - printer completely silent  -> reset/power loss assumed: job FAILED.
 *   - printer still talking      -> an ok was probably lost: the oldest line is
 *     sent again. Marlin either executes it (it never arrived) or rejects the
 *     duplicate with "Resend: N+1", which the accounting above absorbs.
 *   - long commands (M109, G28, ...) are simply waited for while the printer
 *     keeps talking.
 * A "start" banner while connected means Marlin rebooted: any job FAILS.
 */

#include "gcode_stream.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gs_internal.h"
#include "sdkconfig.h"
#include "usb_printer.h"

static const char *TAG = "gcode";

/* ---- tunables ---------------------------------------------------------- */
#define GS_WINDOW          1    /* numbered lines in flight (Marlin BUFSIZE is 4) */
#define GS_HIST            32   /* resend history, must be > 2 * GS_WINDOW */
#define GS_SYSQ            24   /* queued console / script lines */
#define GS_MAX_CMD         80   /* payload limit; Marlin MAX_CMD_SIZE is 96 */
#define GS_BANNER_SETTLE_MS 2000
#define GS_POLL_TEMP_MS    3000 /* M105 polling when auto-report is unsupported */
#define GS_FILE_BUF        4096

#define TIMEOUT_US   ((int64_t)CONFIG_CR10_GCODE_OK_TIMEOUT_MS * 1000)

/* history flags */
#define HF_FILE        (1u << 0)
#define HF_CONSOLE     (1u << 1)
#define HF_LONG        (1u << 2)  /* may legitimately block for minutes */
#define HF_SNAP_POS    (1u << 3)  /* snapshot position when acked (pause) */
#define HF_EV_PAUSED   (1u << 4)
#define HF_EV_RESUMED  (1u << 5)
#define HF_EV_STOPPED  (1u << 6)

typedef struct {
    uint32_t n;
    uint8_t  flags;
    uint32_t file_off;   /* file offset just after this line */
    uint32_t file_line;
    char     text[GS_MAX_CMD + 1];
} hist_t;

typedef struct {
    uint8_t flags;
    char    text[GS_MAX_CMD + 1];
} sysq_t;

typedef enum { HS_IDLE, HS_WAIT_BANNER, HS_SETTLE, HS_SYNC } hs_state_t;

/* ---- shared with gcode_rx.c ------------------------------------------- */
QueueHandle_t g_gs_queue;
SemaphoreHandle_t g_gs_lock;
gs_status_t g_gs_status;
gs_log_cb_t g_gs_log;

/* ---- comm task private state ------------------------------------------ */
static gs_link_t  s_link;
static gs_job_t   s_job;
static hs_state_t s_hs;
static int64_t    s_hs_deadline;

static hist_t   s_hist[GS_HIST];
static uint32_t s_next_n = 1;   /* next new line number */
static uint32_t s_sent_hi = 1;  /* next line number to transmit */
static uint32_t s_ack_n = 1;    /* oldest unacknowledged line */
static uint32_t s_swallow;      /* oks to discard (rejected / raw lines) */
static int64_t  s_progress_us;  /* last ok/busy or first send into an empty window */
static int64_t  s_last_tickle_us;

static sysq_t   s_sysq[GS_SYSQ];
static int      s_sysq_head, s_sysq_len;

static bool     s_autoreport = true;
static int64_t  s_last_poll_us;
static uint32_t s_resends, s_recoveries;

/* job */
static FILE    *s_file;
static char    *s_file_buf;
static char     s_job_name[STORAGE_MAX_NAME + 1];
static uint32_t s_job_size;
static uint32_t s_read_off, s_read_line;   /* read position */
static uint32_t s_acked_off, s_acked_line; /* acknowledged position */
static bool     s_eof;
static bool     s_need_sync;               /* send M110 N0 before the first line */
static int64_t  s_job_start_us, s_pause_start_us, s_paused_total_us;
static int64_t  s_last_ckpt_us;
static char     s_error[96];

/* modal state tracked from the file, restored on resume */
static bool     s_rel_xyz, s_rel_e;
static float    s_feedrate;
static bool     s_have_snap;
static float    s_snap[4];

/* ======================================================================= */

void gs_post(gs_msg_type_t type, int32_t n, const char *text)
{
    gs_msg_t m = {.type = type, .n = n};
    if (text) {
        strlcpy(m.text, text, sizeof(m.text));
    }
    xQueueSend(g_gs_queue, &m, portMAX_DELAY);
}

void gs_log(const char *dir, const char *line)
{
    if (g_gs_log) {
        g_gs_log(dir, line);
    }
}

static void logf_(const char *dir, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gs_log(dir, buf);
}

const char *gcode_stream_link_str(gs_link_t l)
{
    switch (l) {
    case GS_LINK_CONNECTING: return "connecting";
    case GS_LINK_READY:      return "ready";
    default:                 return "disconnected";
    }
}

const char *gcode_stream_job_str(gs_job_t j)
{
    static const char *const names[] = {
        "idle", "printing", "pausing", "paused", "resuming", "stopping", "finished", "failed", "cancelled",
    };
    return j <= GS_JOB_CANCELLED ? names[j] : "?";
}

static bool job_active(gs_job_t j)
{
    return j >= GS_JOB_PRINTING && j <= GS_JOB_STOPPING;
}

static uint32_t inflight(void)
{
    return (s_sent_hi - s_ack_n) + s_swallow;
}

/* ---- transmit ---------------------------------------------------------- */

static void tx_raw(const char *line)
{
    char buf[GS_MAX_CMD + 4];
    int len = snprintf(buf, sizeof(buf), "%s\n", line);
    if (usb_printer_write(buf, len, 1000) != ESP_OK) {
        ESP_LOGW(TAG, "write failed: %s", line);
    }
}

static void tx_numbered(const hist_t *h)
{
    char buf[GS_MAX_CMD + 24];
    int len = snprintf(buf, sizeof(buf), "N%" PRIu32 " %s", h->n, h->text);
    uint8_t cs = 0;
    for (int i = 0; i < len; i++) {
        cs ^= (uint8_t)buf[i];
    }
    len += snprintf(buf + len, sizeof(buf) - len, "*%u\n", cs);
    if (inflight() == 0) {
        s_progress_us = esp_timer_get_time();
    }
    if (usb_printer_write(buf, len, 1000) != ESP_OK) {
        ESP_LOGW(TAG, "write failed: N%" PRIu32, h->n);
    }
}

/* Resets numbering with an unnumbered M110 N0 (its ok is swallowed). */
static void sync_line_numbers(void)
{
    s_next_n = s_sent_hi = s_ack_n = 1;
    s_swallow = 1;
    s_progress_us = esp_timer_get_time();
    tx_raw("M110 N0");
}

static bool is_long_command(const char *t)
{
    static const char *const longs[] = {
        "M109", "M190", "G28", "G29", "M303", "M400", "G4", "M600", "M0", "M1", "M48", "G76", "M191",
    };
    for (size_t i = 0; i < sizeof(longs) / sizeof(longs[0]); i++) {
        size_t n = strlen(longs[i]);
        if (strncasecmp(t, longs[i], n) == 0 && (t[n] == 0 || t[n] == ' ')) {
            return true;
        }
    }
    return false;
}

static void send_new(const char *text, uint8_t flags)
{
    hist_t *h = &s_hist[s_next_n % GS_HIST];
    h->n = s_next_n;
    h->flags = flags | (is_long_command(text) ? HF_LONG : 0);
    h->file_off = s_read_off;
    h->file_line = s_read_line;
    strlcpy(h->text, text, sizeof(h->text));
    s_next_n++;
    tx_numbered(h);
    s_sent_hi = s_next_n;
    if (!(flags & HF_FILE)) {
        gs_log(">", text);
    }
}

/* ---- system queue (console + pause/resume/stop scripts) ---------------- */

static bool sysq_push(uint8_t flags, const char *fmt, ...)
{
    if (s_sysq_len >= GS_SYSQ) {
        return false;
    }
    sysq_t *e = &s_sysq[(s_sysq_head + s_sysq_len) % GS_SYSQ];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);
    e->flags = flags;
    s_sysq_len++;
    return true;
}

static bool sysq_pop(sysq_t *out)
{
    if (!s_sysq_len) {
        return false;
    }
    *out = s_sysq[s_sysq_head];
    s_sysq_head = (s_sysq_head + 1) % GS_SYSQ;
    s_sysq_len--;
    return true;
}

static void sysq_clear(void)
{
    s_sysq_head = s_sysq_len = 0;
}

/* ---- job helpers ------------------------------------------------------- */

static void close_file(void)
{
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    free(s_file_buf);
    s_file_buf = NULL;
}

static void save_checkpoint(const char *reason)
{
    storage_checkpoint_t cp = {.valid = true, .offset = s_acked_off, .line = s_acked_line, .size = s_job_size};
    strlcpy(cp.file, s_job_name, sizeof(cp.file));
    strlcpy(cp.state, gcode_stream_job_str(s_job), sizeof(cp.state));
    strlcpy(cp.reason, reason ? reason : "", sizeof(cp.reason));
    if (storage_checkpoint_save(&cp) != ESP_OK) {
        ESP_LOGW(TAG, "checkpoint write failed");
    }
    xSemaphoreTake(g_gs_lock, portMAX_DELAY);
    g_gs_status.checkpoint = cp;
    xSemaphoreGive(g_gs_lock);
    s_last_ckpt_us = esp_timer_get_time();
}

static void end_job(gs_job_t final, const char *reason)
{
    if (s_job == GS_JOB_PAUSED && s_pause_start_us) {
        s_paused_total_us += esp_timer_get_time() - s_pause_start_us;
    }
    s_job = final;
    close_file();
    sysq_clear();
    if (reason) {
        strlcpy(s_error, reason, sizeof(s_error));
        logf_("!", "Print %s: %s", gcode_stream_job_str(final), reason);
    } else {
        logf_("!", "Print %s", gcode_stream_job_str(final));
    }
    save_checkpoint(reason);
}

/* Abandon all protocol state (lines in flight are lost). */
static void protocol_reset(void)
{
    s_ack_n = s_sent_hi = s_next_n;
    s_swallow = 0;
}

static void fail_job(const char *reason)
{
    ESP_LOGE(TAG, "print failed: %s", reason);
    end_job(GS_JOB_FAILED, reason);
    protocol_reset();
}

static void start_handshake(int64_t delay_ms, hs_state_t st)
{
    s_link = GS_LINK_CONNECTING;
    s_hs = st;
    s_hs_deadline = esp_timer_get_time() + delay_ms * 1000;
    protocol_reset();
    sysq_clear();
}

/* ---- G-code cleaning --------------------------------------------------- */

/* Strips comments/whitespace in place. Returns length (0 = nothing to send). */
static size_t clean_line(char *s)
{
    char *semi = strchr(s, ';');
    if (semi) {
        *semi = 0;
    }
    char *start = s;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    size_t len = strlen(start);
    while (len && isspace((unsigned char)start[len - 1])) {
        start[--len] = 0;
    }
    if (start != s) {
        memmove(s, start, len + 1);
    }
    if (len > GS_MAX_CMD) {
        /* Marlin parses "G1X10Y20" fine: squeeze out spaces */
        size_t w = 0;
        for (size_t r = 0; r < len; r++) {
            if (s[r] != ' ' && s[r] != '\t') {
                s[w++] = s[r];
            }
        }
        s[w] = 0;
        len = w;
    }
    return len;
}

static void track_modal(const char *t)
{
    char c0 = toupper((unsigned char)t[0]);
    int code = atoi(t + 1);
    if (c0 == 'G' && (code == 0 || code == 1)) {
        const char *f = strpbrk(t, "Ff");
        if (f) {
            s_feedrate = strtof(f + 1, NULL);
        }
    } else if (c0 == 'G' && code == 90) {
        s_rel_xyz = false;
    } else if (c0 == 'G' && code == 91) {
        s_rel_xyz = true;
    } else if (c0 == 'M' && code == 82) {
        s_rel_e = false;
    } else if (c0 == 'M' && code == 83) {
        s_rel_e = true;
    }
}

/* Reads the next sendable line. Returns false at EOF or error. */
static bool next_file_line(char *out, size_t out_len)
{
    char buf[256];
    while (fgets(buf, sizeof(buf), s_file)) {
        size_t raw = strlen(buf);
        s_read_off += raw;
        bool complete = raw && buf[raw - 1] == '\n';
        if (!complete && !feof(s_file)) {
            /* Over-long line: consume the rest. Fine if the overflow is part of
             * a comment (the command fits in the first chunk), fatal otherwise. */
            int c;
            while ((c = fgetc(s_file)) != EOF) {
                s_read_off++;
                if (c == '\n') {
                    break;
                }
            }
            if (!strchr(buf, ';')) {
                s_read_line++;
                fail_job("G-code line longer than 255 bytes");
                return false;
            }
        }
        s_read_line++;
        size_t len = clean_line(buf);
        if (!len) {
            continue;
        }
        if (len > GS_MAX_CMD || len >= out_len) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Line %" PRIu32 " too long for Marlin", s_read_line);
            fail_job(msg);
            return false;
        }
        memcpy(out, buf, len + 1);
        return true;
    }
    if (ferror(s_file)) {
        fail_job("SD read error");
    }
    return false;
}

/* ---- request handlers -------------------------------------------------- */

static void do_start(const char *name)
{
    if (s_link != GS_LINK_READY || job_active(s_job)) {
        logf_("!", "Cannot start: printer busy or not ready");
        return;
    }
    char path[STORAGE_PATH_MAX];
    if (!storage_path(name, path, sizeof(path))) {
        logf_("!", "Invalid file name");
        return;
    }
    s_file = fopen(path, "r");
    if (!s_file) {
        logf_("!", "Cannot open %s", name);
        return;
    }
    s_file_buf = malloc(GS_FILE_BUF);
    if (s_file_buf) {
        setvbuf(s_file, s_file_buf, _IOFBF, GS_FILE_BUF);
    }
    fseek(s_file, 0, SEEK_END);
    s_job_size = (uint32_t)ftell(s_file);
    fseek(s_file, 0, SEEK_SET);

    strlcpy(s_job_name, name, sizeof(s_job_name));
    s_read_off = s_read_line = s_acked_off = s_acked_line = 0;
    s_eof = false;
    s_need_sync = true;
    s_rel_xyz = s_rel_e = false;
    s_feedrate = 0;
    s_have_snap = false;
    s_error[0] = 0;
    s_job_start_us = esp_timer_get_time();
    s_pause_start_us = s_paused_total_us = 0;
    s_job = GS_JOB_PRINTING;
    logf_("!", "Printing %s (%" PRIu32 " bytes)", name, s_job_size);
    save_checkpoint(NULL);
}

static void do_pause(void)
{
    if (s_job != GS_JOB_PRINTING) {
        return;
    }
    s_job = GS_JOB_PAUSING;
    s_have_snap = false;
    /* Queued behind the lines already in flight; no new file lines are read. */
    sysq_push(HF_LONG, "M400");
    sysq_push(HF_SNAP_POS, "M114");
    sysq_push(0, "G91");
    sysq_push(0, "G1 E-%d.%d F2400", CONFIG_CR10_PAUSE_RETRACT_MM10 / 10, CONFIG_CR10_PAUSE_RETRACT_MM10 % 10);
    sysq_push(0, "G1 Z%d F600", CONFIG_CR10_PAUSE_Z_LIFT_MM);
    sysq_push(HF_EV_PAUSED, "G90");
    logf_("!", "Pausing...");
}

static void do_resume(void)
{
    if (s_job != GS_JOB_PAUSED) {
        return;
    }
    s_job = GS_JOB_RESUMING;
    if (s_have_snap) {
        /* Absolute restore: correct even if the head was jogged while paused. */
        sysq_push(0, "G90");
        sysq_push(0, "G1 X%.3f Y%.3f F3000", s_snap[0], s_snap[1]);
        sysq_push(0, "G1 Z%.3f F600", s_snap[2]);
        sysq_push(0, "G91");
        sysq_push(0, "G1 E%d.%d F2400", CONFIG_CR10_PAUSE_RETRACT_MM10 / 10, CONFIG_CR10_PAUSE_RETRACT_MM10 % 10);
        sysq_push(0, "G90");
        sysq_push(0, "G92 E%.5f", s_snap[3]);
    } else {
        /* No position report: undo the relative pause moves. */
        sysq_push(0, "G91");
        sysq_push(0, "G1 Z-%d F600", CONFIG_CR10_PAUSE_Z_LIFT_MM);
        sysq_push(0, "G1 E%d.%d F2400", CONFIG_CR10_PAUSE_RETRACT_MM10 / 10, CONFIG_CR10_PAUSE_RETRACT_MM10 % 10);
        sysq_push(0, "G90");
    }
    sysq_push(0, "%s", s_rel_e ? "M83" : "M82");
    if (s_feedrate > 0) {
        sysq_push(0, "G1 F%.0f", s_feedrate);
    }
    sysq_push(HF_EV_RESUMED, "%s", s_rel_xyz ? "G91" : "G90");
    logf_("!", "Resuming...");
}

static void do_stop(void)
{
    if (!job_active(s_job)) {
        return;
    }
    if (s_job == GS_JOB_STOPPING) {
        return;
    }
    if (s_job == GS_JOB_PAUSED && s_pause_start_us) {
        s_paused_total_us += esp_timer_get_time() - s_pause_start_us;
        s_pause_start_us = 0;
    }
    close_file();
    sysq_clear();
    s_job = GS_JOB_STOPPING;
    logf_("!", "Stopping...");
    if (s_link != GS_LINK_READY) {
        end_job(GS_JOB_CANCELLED, NULL);
        return;
    }
    /* M108 breaks a blocking M109/M190 wait (emergency parser). Unnumbered,
     * bypasses the window; Marlin answers it with one ok. */
    tx_raw("M108");
    s_swallow++;
    gs_log(">", "M108");
    sysq_push(0, "M104 S0");
    sysq_push(0, "M140 S0");
    sysq_push(0, "M107");
    sysq_push(0, "G91");
    sysq_push(0, "G1 Z%d F600", CONFIG_CR10_STOP_Z_LIFT_MM);
    sysq_push(0, "G90");
    sysq_push(HF_EV_STOPPED, "M84");
}

static void do_console(const char *line)
{
    if (s_link != GS_LINK_READY) {
        logf_("!", "Printer not ready");
        return;
    }
    char buf[GS_MAX_TEXT];
    strlcpy(buf, line, sizeof(buf));
    if (!clean_line(buf)) {
        return;
    }
    if (strlen(buf) > GS_MAX_CMD) {
        logf_("!", "Command too long");
        return;
    }
    if (!sysq_push(HF_CONSOLE, "%s", buf)) {
        logf_("!", "Command queue full");
    }
}

/* ---- RX event handlers ------------------------------------------------- */

static void on_ok(const char *text)
{
    s_progress_us = esp_timer_get_time();
    if (s_swallow) {
        s_swallow--;
        return;
    }
    if (s_ack_n == s_sent_hi) {
        ESP_LOGW(TAG, "unexpected ok");
        return;
    }
    const hist_t *h = &s_hist[s_ack_n % GS_HIST];
    s_ack_n++;

    if (h->flags & HF_FILE) {
        s_acked_off = h->file_off;
        s_acked_line = h->file_line;
    }
    if (h->flags & HF_CONSOLE) {
        gs_log("<", text);
    }
    if (h->flags & HF_SNAP_POS) {
        xSemaphoreTake(g_gs_lock, portMAX_DELAY);
        memcpy(s_snap, g_gs_status.pos, sizeof(s_snap));
        xSemaphoreGive(g_gs_lock);
        s_have_snap = true;
        logf_("!", "Paused at X%.2f Y%.2f Z%.2f E%.3f", s_snap[0], s_snap[1], s_snap[2], s_snap[3]);
    }
    if ((h->flags & HF_EV_PAUSED) && s_job == GS_JOB_PAUSING) {
        s_job = GS_JOB_PAUSED;
        s_pause_start_us = esp_timer_get_time();
        logf_("!", "Paused");
        save_checkpoint(NULL);
    }
    if ((h->flags & HF_EV_RESUMED) && s_job == GS_JOB_RESUMING) {
        s_paused_total_us += esp_timer_get_time() - s_pause_start_us;
        s_pause_start_us = 0;
        s_job = GS_JOB_PRINTING;
        logf_("!", "Resumed");
    }
    if ((h->flags & HF_EV_STOPPED) && s_job == GS_JOB_STOPPING) {
        end_job(GS_JOB_CANCELLED, NULL);
    }
}

static void on_resend(uint32_t n)
{
    s_resends++;
    if (s_swallow) {
        /* Belongs to a line already rejected in the current resend batch. */
        return;
    }
    if (n > s_next_n) {
        n = s_next_n;
    }
    if (n == 0 || s_next_n - n >= GS_HIST) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Resend of line %" PRIu32 " not in history", n);
        if (job_active(s_job)) {
            fail_job(msg);
        }
        gs_log("!", msg);
        start_handshake(0, HS_SETTLE);
        return;
    }
    ESP_LOGW(TAG, "resend N%" PRIu32 " (in flight %" PRIu32 "..%" PRIu32 ")", n, s_ack_n, s_sent_hi);
    /* Each line in flight gets exactly one more ok (this resend's ok included). */
    s_swallow = s_sent_hi - s_ack_n;
    if (s_swallow == 0) {
        s_swallow = 1; /* the ok that follows this Resend */
    }
    s_ack_n = s_sent_hi = n;
}

static void on_banner(void)
{
    if (s_link == GS_LINK_READY) {
        if (job_active(s_job)) {
            fail_job("Printer reset during print (startup banner seen)");
        }
        gs_log("!", "Printer restarted, re-syncing");
    }
    if (s_link != GS_LINK_DISCONNECTED) {
        start_handshake(GS_BANNER_SETTLE_MS, HS_SETTLE);
    }
}

static void handle_msg(const gs_msg_t *m)
{
    switch (m->type) {
    case MSG_OK:
        if (s_link == GS_LINK_CONNECTING && s_hs == HS_SYNC && s_swallow) {
            s_swallow--; /* ok for M110 N0 */
            break;
        }
        on_ok(m->text);
        break;
    case MSG_RESEND:
        if (s_link == GS_LINK_READY) {
            on_resend((uint32_t)m->n);
        }
        break;
    case MSG_BUSY:
        s_progress_us = esp_timer_get_time();
        break;
    case MSG_BANNER:
        on_banner();
        break;
    case MSG_FATAL:
        if (job_active(s_job)) {
            fail_job(m->text);
        } else {
            strlcpy(s_error, m->text, sizeof(s_error));
        }
        break;
    case MSG_UNKNOWN_CMD:
        if (strstr(m->text, "M155")) {
            s_autoreport = false;
            gs_log("!", "No temperature auto-report: polling with M105");
        }
        break;
    case MSG_CAP_AUTOREPORT:
        s_autoreport = m->n != 0;
        break;
    case MSG_LINK_UP:
        gs_log("!", "Printer USB connected, waiting for firmware banner");
        s_autoreport = true;
        start_handshake(CONFIG_CR10_PRINTER_BANNER_TIMEOUT_MS, HS_WAIT_BANNER);
        break;
    case MSG_LINK_DOWN:
        if (job_active(s_job)) {
            fail_job("Printer disconnected / powered off");
        }
        gs_log("!", "Printer disconnected");
        s_link = GS_LINK_DISCONNECTED;
        s_hs = HS_IDLE;
        protocol_reset();
        sysq_clear();
        break;
    case MSG_START:
        do_start(m->text);
        break;
    case MSG_PAUSE:
        do_pause();
        break;
    case MSG_RESUME:
        do_resume();
        break;
    case MSG_STOP:
        do_stop();
        break;
    case MSG_CONSOLE:
        do_console(m->text);
        break;
    }
}

/* ---- periodic work ----------------------------------------------------- */

static void handshake_step(int64_t now)
{
    if (s_link != GS_LINK_CONNECTING) {
        return;
    }
    switch (s_hs) {
    case HS_WAIT_BANNER:
    case HS_SETTLE:
        if (now >= s_hs_deadline) {
            if (s_hs == HS_WAIT_BANNER) {
                gs_log("!", "No banner received, syncing anyway");
            }
            s_hs = HS_SYNC;
            sync_line_numbers();
        }
        break;
    case HS_SYNC:
        if (s_swallow == 0) {
            s_hs = HS_IDLE;
            s_link = GS_LINK_READY;
            s_last_poll_us = now;
            gs_log("!", "Printer ready");
            sysq_push(0, "M115");
            sysq_push(0, "M155 S2");
        } else if (now - s_progress_us > TIMEOUT_US) {
            gs_log("!", "No answer to M110, retrying");
            sync_line_numbers();
        }
        break;
    default:
        break;
    }
}

static void check_timeouts(int64_t now)
{
    if (s_link != GS_LINK_READY || inflight() == 0 || now - s_progress_us <= TIMEOUT_US) {
        return;
    }
    bool silent = now - gs_last_rx_us() > TIMEOUT_US;
    if (silent) {
        if (job_active(s_job)) {
            fail_job("Printer stopped responding (reset or power loss?)");
        } else {
            gs_log("!", "Printer not responding, re-syncing");
        }
        start_handshake(0, HS_SETTLE);
        return;
    }
    if (s_sent_hi == s_ack_n) {
        /* only raw/swallowed oks outstanding (e.g. M108 discarded): forget them */
        s_swallow = 0;
        return;
    }
    const hist_t *h = &s_hist[s_ack_n % GS_HIST];
    if (h->flags & HF_LONG) {
        s_progress_us = now; /* printer alive, long command: keep waiting */
        return;
    }
    if (now - s_last_tickle_us > TIMEOUT_US) {
        s_last_tickle_us = now;
        s_recoveries++;
        ESP_LOGW(TAG, "no ok for N%" PRIu32 ", re-sending", h->n);
        logf_("!", "No ok for line N%" PRIu32 ", re-sending", h->n);
        tx_numbered(h);
        s_progress_us = now;
    }
}

static void pump(void)
{
    while (s_link == GS_LINK_READY && inflight() < GS_WINDOW) {
        if (s_sent_hi < s_next_n) {
            /* replay after Resend */
            tx_numbered(&s_hist[s_sent_hi % GS_HIST]);
            s_sent_hi++;
            continue;
        }
        sysq_t e;
        if (sysq_pop(&e)) {
            track_modal(e.text);
            send_new(e.text, e.flags);
            continue;
        }
        if (s_job == GS_JOB_PRINTING && s_file && !s_eof) {
            if (s_need_sync) {
                if (inflight() == 0) {
                    s_need_sync = false;
                    sync_line_numbers();
                }
                return;
            }
            char line[GS_MAX_CMD + 1];
            if (next_file_line(line, sizeof(line))) {
                track_modal(line);
                send_new(line, HF_FILE);
                continue;
            }
            if (s_job == GS_JOB_PRINTING) {
                s_eof = true;
            }
        }
        break;
    }
}

static void periodic(int64_t now)
{
    if (s_job == GS_JOB_PRINTING && s_eof && inflight() == 0 && s_sysq_len == 0) {
        s_acked_off = s_job_size;
        end_job(GS_JOB_FINISHED, NULL);
    }
    if ((s_job == GS_JOB_PRINTING || s_job == GS_JOB_PAUSED) &&
        now - s_last_ckpt_us > (int64_t)CONFIG_CR10_CHECKPOINT_INTERVAL_S * 1000000) {
        save_checkpoint(NULL);
    }
    if (s_link == GS_LINK_READY && !s_autoreport && now - s_last_poll_us > GS_POLL_TEMP_MS * 1000LL &&
        s_sysq_len == 0) {
        s_last_poll_us = now;
        sysq_push(0, "M105");
    }
}

static void publish_status(int64_t now)
{
    xSemaphoreTake(g_gs_lock, portMAX_DELAY);
    gs_status_t *st = &g_gs_status;
    st->link = s_link;
    st->job = s_job;
    strlcpy(st->file, s_job_name, sizeof(st->file));
    st->size = s_job_size;
    st->acked_bytes = s_acked_off;
    st->file_line = s_acked_line;
    if (job_active(s_job)) {
        int64_t paused = s_paused_total_us + (s_pause_start_us ? now - s_pause_start_us : 0);
        st->elapsed_s = (uint32_t)((now - s_job_start_us - paused) / 1000000);
    } /* else: keep the final value of the last job */
    strlcpy(st->error, s_error, sizeof(st->error));
    st->resends = s_resends;
    st->recoveries = s_recoveries;
    st->autoreport = s_autoreport;
    if (s_link == GS_LINK_DISCONNECTED) {
        st->temps_valid = false;
    }
    xSemaphoreGive(g_gs_lock);
}

static void comm_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    gs_msg_t m;
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(g_gs_queue, &m, pdMS_TO_TICKS(20)) == pdTRUE) {
            do {
                handle_msg(&m);
            } while (xQueueReceive(g_gs_queue, &m, 0) == pdTRUE);
        }
        int64_t now = esp_timer_get_time();
        handshake_step(now);
        check_timeouts(now);
        pump();
        periodic(now);
        publish_status(now);
    }
}

/* ---- public API -------------------------------------------------------- */

static void on_link(bool connected, void *arg)
{
    gs_post(connected ? MSG_LINK_UP : MSG_LINK_DOWN, 0, NULL);
}

esp_err_t gcode_stream_init(gs_log_cb_t log_cb)
{
    g_gs_log = log_cb;
    g_gs_queue = xQueueCreate(48, sizeof(gs_msg_t));
    g_gs_lock = xSemaphoreCreateMutex();
    if (!g_gs_queue || !g_gs_lock) {
        return ESP_ERR_NO_MEM;
    }
    storage_checkpoint_t cp;
    if (storage_checkpoint_load(&cp) == ESP_OK) {
        g_gs_status.checkpoint = cp;
        ESP_LOGI(TAG, "last job: %s, %s at line %" PRIu32, cp.file, cp.state, cp.line);
    }
    xTaskCreatePinnedToCore(comm_task, "gs_comm", 6144, NULL, 7, NULL, 1);
    /* The transport creates the RX stream buffer: init it before the reader. */
    esp_err_t err = usb_printer_init(on_link, NULL);
    if (err == ESP_OK) {
        gs_rx_start();
    }
    return err;
}

void gcode_stream_get_status(gs_status_t *out)
{
    xSemaphoreTake(g_gs_lock, portMAX_DELAY);
    *out = g_gs_status;
    xSemaphoreGive(g_gs_lock);
}

bool gcode_stream_job_active(void)
{
    xSemaphoreTake(g_gs_lock, portMAX_DELAY);
    bool a = job_active(g_gs_status.job);
    xSemaphoreGive(g_gs_lock);
    return a;
}

static esp_err_t refuse(char *why, size_t why_len, const char *msg)
{
    if (why && why_len) {
        strlcpy(why, msg, why_len);
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t post_api(gs_msg_type_t type, const char *text)
{
    gs_msg_t m = {.type = type};
    if (text) {
        strlcpy(m.text, text, sizeof(m.text));
    }
    return xQueueSend(g_gs_queue, &m, pdMS_TO_TICKS(200)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t gcode_stream_start(const char *name, char *why, size_t why_len)
{
    gs_status_t st;
    gcode_stream_get_status(&st);
    if (st.link != GS_LINK_READY) {
        return refuse(why, why_len, "Printer not connected/ready");
    }
    if (job_active(st.job)) {
        return refuse(why, why_len, "A print is already running");
    }
    if (!storage_mounted()) {
        return refuse(why, why_len, "SD card not mounted");
    }
    if (!storage_name_valid(name)) {
        return refuse(why, why_len, "Invalid file name");
    }
    return post_api(MSG_START, name);
}

esp_err_t gcode_stream_pause(char *why, size_t why_len)
{
    gs_status_t st;
    gcode_stream_get_status(&st);
    if (st.job != GS_JOB_PRINTING) {
        return refuse(why, why_len, "Not printing");
    }
    return post_api(MSG_PAUSE, NULL);
}

esp_err_t gcode_stream_resume(char *why, size_t why_len)
{
    gs_status_t st;
    gcode_stream_get_status(&st);
    if (st.job != GS_JOB_PAUSED) {
        return refuse(why, why_len, "Not paused");
    }
    return post_api(MSG_RESUME, NULL);
}

esp_err_t gcode_stream_stop(char *why, size_t why_len)
{
    gs_status_t st;
    gcode_stream_get_status(&st);
    if (!job_active(st.job)) {
        return refuse(why, why_len, "No active print");
    }
    return post_api(MSG_STOP, NULL);
}

/* Commands that cannot disturb a running print. */
static bool console_allowed(const char *cmd, gs_job_t job)
{
    static const char *const safe[] = {
        "M105", "M114", "M115", "M119", "M27", "M31", "M503", "M220", "M221", "M155",
    };
    /* While paused the head may be jogged, extruded and fans changed: the
     * resume script restores the exact position (M114 snapshot). */
    static const char *const paused_extra[] = {
        "G0", "G1", "G90", "G91", "M82", "M83", "M104", "M106", "M107", "M400",
    };
    if (!job_active(job)) {
        return true;
    }
    char code[8] = {0};
    size_t i = 0;
    while (cmd[i] && cmd[i] != ' ' && i < sizeof(code) - 1) {
        code[i] = (char)toupper((unsigned char)cmd[i]);
        i++;
    }
    for (i = 0; i < sizeof(safe) / sizeof(safe[0]); i++) {
        if (!strcmp(code, safe[i])) {
            return true;
        }
    }
    if (job == GS_JOB_PAUSED) {
        for (i = 0; i < sizeof(paused_extra) / sizeof(paused_extra[0]); i++) {
            if (!strcmp(code, paused_extra[i])) {
                return true;
            }
        }
    }
    return false;
}

esp_err_t gcode_stream_console(const char *line, char *why, size_t why_len)
{
    gs_status_t st;
    gcode_stream_get_status(&st);
    while (*line == ' ') {
        line++;
    }
    if (!*line) {
        return refuse(why, why_len, "Empty command");
    }
    if (st.link != GS_LINK_READY) {
        return refuse(why, why_len, "Printer not connected/ready");
    }
    if (strlen(line) >= GS_MAX_TEXT) {
        return refuse(why, why_len, "Command too long");
    }
    if (!console_allowed(line, st.job)) {
        return refuse(why, why_len, st.job == GS_JOB_PAUSED
                                        ? "Not allowed while paused"
                                        : "Only M105/M114/M115/M119/M27/M31/M503/M220/M221 while printing");
    }
    return post_api(MSG_CONSOLE, line);
}
