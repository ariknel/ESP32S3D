#pragma once

/*
 * Marlin host protocol + print streaming.
 *
 * Tasks:
 *  - gs_comm ("print task", task-watchdog protected): the ONLY sender. Owns
 *    line numbering, the in-flight window, resend history, the job state
 *    machine, pause/resume/stop scripts and checkpointing.
 *  - gs_rx: parses everything the printer sends (ok, Resend, busy, Error,
 *    temperatures, positions, banner) and forwards events to gs_comm.
 *
 * All public functions are thread-safe and non-blocking (they post requests).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "storage.h"

typedef enum {
    GS_LINK_DISCONNECTED = 0,
    GS_LINK_CONNECTING,   /* USB up, waiting for banner / M110 sync */
    GS_LINK_READY,
} gs_link_t;

typedef enum {
    GS_JOB_IDLE = 0,
    GS_JOB_PRINTING,
    GS_JOB_PAUSING,   /* draining + running the pause script */
    GS_JOB_PAUSED,
    GS_JOB_RESUMING,  /* running the resume script */
    GS_JOB_STOPPING,  /* running the stop script */
    GS_JOB_FINISHED,
    GS_JOB_FAILED,
    GS_JOB_CANCELLED,
} gs_job_t;

typedef struct {
    gs_link_t link;
    gs_job_t  job;
    char      file[STORAGE_MAX_NAME + 1];
    uint32_t  size;          /* file size in bytes */
    uint32_t  acked_bytes;   /* bytes of the file acknowledged by Marlin */
    uint32_t  file_line;     /* last acknowledged file line */
    uint32_t  elapsed_s;     /* print time excluding pauses */
    float     hotend, hotend_target, bed, bed_target;
    bool      temps_valid;
    float     pos[4];        /* X Y Z E from the last M114 */
    char      error[96];     /* last error (sticky until next print) */
    char      firmware[64];
    uint32_t  resends;
    uint32_t  recoveries;    /* lost-ok recoveries */
    bool      autoreport;
    storage_checkpoint_t checkpoint;
} gs_status_t;

/* Console/log sink: dir is ">" (sent), "<" (received) or "!" (error/info). */
typedef void (*gs_log_cb_t)(const char *dir, const char *line);

esp_err_t gcode_stream_init(gs_log_cb_t log_cb);
void      gcode_stream_get_status(gs_status_t *out);

/* True while a job holds the printer (printing, paused, pausing, ...). */
bool      gcode_stream_job_active(void);

const char *gcode_stream_link_str(gs_link_t l);
const char *gcode_stream_job_str(gs_job_t j);

/* Requests. On refusal they return an error and fill `why` (may be NULL). */
esp_err_t gcode_stream_start(const char *name, char *why, size_t why_len);
esp_err_t gcode_stream_pause(char *why, size_t why_len);
esp_err_t gcode_stream_resume(char *why, size_t why_len);
esp_err_t gcode_stream_stop(char *why, size_t why_len);

/* One manual G-code line. While a job is active only safe commands pass. */
esp_err_t gcode_stream_console(const char *line, char *why, size_t why_len);
