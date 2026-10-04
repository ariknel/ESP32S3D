/*
 * CR-10 Wi-Fi print server for ESP32-S3.
 *
 *   storage       SD card (FAT) for G-code files + checkpoint
 *   usb_printer   USB host + CH34x VCP (or simulated Marlin in dry-run mode)
 *   gcode_stream  Marlin protocol, print task, RX parser
 *   net           Wi-Fi STA / setup AP, mDNS
 *   web           HTTP UI, REST, WebSocket, OTA
 */

#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "gcode_stream.h"
#include "net.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "storage.h"
#include "web.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "CR-10 print server starting");
#if CONFIG_CR10_PRINTER_DRY_RUN
    ESP_LOGW(TAG, "*** DRY RUN MODE: printer is simulated ***");
#endif

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    storage_init(); /* non-fatal: keeps retrying in the background */

    ESP_ERROR_CHECK(gcode_stream_init(web_log));
    ESP_ERROR_CHECK(net_init());
    ESP_ERROR_CHECK(web_start());

    /* We got this far with a freshly OTA'd image: keep it (cancel rollback). */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA image marked valid");
    }
    ESP_LOGI(TAG, "ready");
}
