/*
 * USB host transport: ESP32-S3 OTG (GPIO19/20) -> CH340 on the CR-10 Melzi board.
 *
 * - usb_lib task  : runs the USB host library event loop.
 * - usb_conn task : waits for a CH34x device, opens it ONCE, configures
 *                   115200 8N1 and DTR/RTS, then sleeps until it disconnects.
 *                   The port is never reopened while the device stays attached,
 *                   so Marlin is never reset by us in the middle of a print.
 * - RX bytes arrive in the CDC driver's callback and go into a stream buffer.
 */

#include "usb_printer.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "usb/vcp_ch34x.h"

static const char *TAG = "usb_printer";

#define RX_STREAM_SIZE   4096
#define CDC_IN_BUF_SIZE  512
#define CDC_OUT_BUF_SIZE 256

static StreamBufferHandle_t s_rx;
static SemaphoreHandle_t s_disconnected;
static SemaphoreHandle_t s_hdl_lock;
static cdc_acm_dev_hdl_t s_hdl;
static volatile bool s_connected;
static volatile uint32_t s_rx_dropped;
static usb_printer_conn_cb_t s_cb;
static void *s_cb_arg;

static void usb_lib_task(void *arg)
{
    const usb_host_config_t cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&cfg));
    xTaskNotifyGive((TaskHandle_t)arg);

    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* Runs in the CDC driver task: must not block. */
static bool on_rx(const uint8_t *data, size_t len, void *arg)
{
    size_t sent = xStreamBufferSend(s_rx, data, len, 0);
    if (sent < len) {
        s_rx_dropped += len - sent;
    }
    return true;
}

static void on_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "printer USB disconnected");
        xSemaphoreGive(s_disconnected);
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC error %d", event->data.error);
        break;
    default:
        break;
    }
}

static esp_err_t configure_port(cdc_acm_dev_hdl_t hdl)
{
    cdc_acm_line_coding_t lc = {
        .dwDTERate = CONFIG_CR10_PRINTER_BAUD,
        .bCharFormat = 0, /* 1 stop bit */
        .bParityType = 0, /* none */
        .bDataBits = 8,
    };
    esp_err_t err = cdc_acm_host_line_coding_set(hdl, &lc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "line coding: %s", esp_err_to_name(err));
        return err;
    }
    /* Set explicitly, exactly once per USB connect. With DTR asserted the
     * Melzi auto-reset circuit restarts Marlin; we then wait for "start". */
#ifdef CONFIG_CR10_PRINTER_DTR_ON_CONNECT
    const bool dtr = true;
#else
    const bool dtr = false;
#endif
#ifdef CONFIG_CR10_PRINTER_RTS_ON_CONNECT
    const bool rts = true;
#else
    const bool rts = false;
#endif
    err = cdc_acm_host_set_control_line_state(hdl, dtr, rts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "control line state: %s", esp_err_to_name(err));
    }
    return err;
}

static void usb_conn_task(void *arg)
{
    const cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = CDC_OUT_BUF_SIZE,
        .in_buffer_size = CDC_IN_BUF_SIZE,
        .event_cb = on_event,
        .data_cb = on_rx,
        .user_arg = NULL,
    };

    for (;;) {
        cdc_acm_dev_hdl_t hdl = NULL;
        esp_err_t err = ch34x_vcp_open(CH34X_PID_AUTO, 0, &dev_cfg, &hdl);
        if (err != ESP_OK) {
            /* ESP_ERR_NOT_FOUND / timeout: nothing plugged in (or printer off) */
            continue;
        }
        ESP_LOGI(TAG, "CH34x opened");
        xSemaphoreTake(s_disconnected, 0); /* clear stale signal */
        xStreamBufferReset(s_rx);

        if (configure_port(hdl) != ESP_OK) {
            cdc_acm_host_close(hdl);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        xSemaphoreTake(s_hdl_lock, portMAX_DELAY);
        s_hdl = hdl;
        s_connected = true;
        xSemaphoreGive(s_hdl_lock);
        if (s_cb) {
            s_cb(true, s_cb_arg);
        }

        xSemaphoreTake(s_disconnected, portMAX_DELAY);

        xSemaphoreTake(s_hdl_lock, portMAX_DELAY);
        s_connected = false;
        s_hdl = NULL;
        xSemaphoreGive(s_hdl_lock);
        cdc_acm_host_close(hdl);
        if (s_cb) {
            s_cb(false, s_cb_arg);
        }
    }
}

esp_err_t usb_printer_init(usb_printer_conn_cb_t cb, void *arg)
{
    s_cb = cb;
    s_cb_arg = arg;
    s_rx = xStreamBufferCreate(RX_STREAM_SIZE, 1);
    s_disconnected = xSemaphoreCreateBinary();
    s_hdl_lock = xSemaphoreCreateMutex();
    if (!s_rx || !s_disconnected || !s_hdl_lock) {
        return ESP_ERR_NO_MEM;
    }

    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 10, NULL, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* wait for usb_host_install */

    esp_err_t err = cdc_acm_host_install(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install: %s", esp_err_to_name(err));
        return err;
    }
    xTaskCreatePinnedToCore(usb_conn_task, "usb_conn", 4096, NULL, 6, NULL, 0);
    ESP_LOGI(TAG, "USB host ready, waiting for CH34x printer");
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
    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_hdl_lock, portMAX_DELAY);
    if (s_hdl) {
        err = cdc_acm_host_data_tx_blocking(s_hdl, data, len, timeout_ms);
    }
    xSemaphoreGive(s_hdl_lock);
    return err;
}

uint32_t usb_printer_rx_dropped(void)
{
    return s_rx_dropped;
}

const char *usb_printer_desc(void)
{
    return "CH34x USB";
}
