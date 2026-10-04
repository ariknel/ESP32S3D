#pragma once

/*
 * Byte transport to the printer.
 *
 * Real backend: ESP32-S3 USB host + CH34x VCP driver (espressif/usb_host_ch34x_vcp).
 * Dry-run backend (CONFIG_CR10_PRINTER_DRY_RUN): a simulated Marlin.
 *
 * Received bytes are pushed into an internal stream buffer and pulled with
 * usb_printer_read() by exactly one reader task. usb_printer_write() must
 * likewise be called from a single writer task.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* Called from the transport's own task on connect/disconnect. Keep it short. */
typedef void (*usb_printer_conn_cb_t)(bool connected, void *arg);

esp_err_t usb_printer_init(usb_printer_conn_cb_t cb, void *arg);
bool      usb_printer_connected(void);

/* Blocks up to `timeout` for at least one byte. Returns number of bytes read. */
size_t    usb_printer_read(uint8_t *buf, size_t max_len, TickType_t timeout);

/* Sends bytes; fails with ESP_ERR_INVALID_STATE when no printer is connected. */
esp_err_t usb_printer_write(const void *data, size_t len, uint32_t timeout_ms);

/* Number of RX bytes dropped because the stream buffer was full (diagnostics). */
uint32_t  usb_printer_rx_dropped(void);

/* Human readable transport description ("CH340 1a86:7523", "dry run"). */
const char *usb_printer_desc(void);
