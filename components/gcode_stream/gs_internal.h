#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "gcode_stream.h"

#define GS_MAX_TEXT 100

typedef enum {
    /* from gs_rx */
    MSG_OK,          /* text = full ok line */
    MSG_RESEND,      /* n = requested line */
    MSG_BUSY,
    MSG_BANNER,      /* "start" seen: Marlin (re)booted */
    MSG_FATAL,       /* text = error that halts the printer */
    MSG_UNKNOWN_CMD, /* text = echo line */
    MSG_CAP_AUTOREPORT, /* n = 0/1 */
    /* from the USB transport */
    MSG_LINK_UP,
    MSG_LINK_DOWN,
    /* from the API */
    MSG_START,       /* text = file name */
    MSG_PAUSE,
    MSG_RESUME,
    MSG_STOP,
    MSG_CONSOLE,     /* text = command */
} gs_msg_type_t;

typedef struct {
    gs_msg_type_t type;
    int32_t n;
    char text[GS_MAX_TEXT];
} gs_msg_t;

extern QueueHandle_t g_gs_queue;
extern SemaphoreHandle_t g_gs_lock;    /* protects g_gs_status */
extern gs_status_t g_gs_status;
extern gs_log_cb_t g_gs_log;

void gs_post(gs_msg_type_t type, int32_t n, const char *text);
void gs_log(const char *dir, const char *line);

/* Microsecond timestamp of the last byte line received from the printer. */
int64_t gs_last_rx_us(void);

void gs_rx_start(void);
