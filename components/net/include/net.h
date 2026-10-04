#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    NET_MODE_STA = 0,  /* connected (or reconnecting) to the configured network */
    NET_MODE_AP,       /* setup AP only: no credentials stored */
    NET_MODE_APSTA,    /* setup AP up because STA failed; STA keeps retrying */
} net_mode_t;

typedef struct {
    net_mode_t mode;
    bool  sta_connected;
    char  sta_ssid[33];
    char  sta_ip[16];
    int   rssi;
    char  ap_ssid[33];
    char  ap_ip[16];
    char  hostname[32];
} net_info_t;

/* Starts Wi-Fi (STA, falling back to the setup AP), mDNS and SNTP.
 * Requires nvs_flash_init() and the default event loop. */
esp_err_t net_init(void);
void      net_get_info(net_info_t *info);

/* True while the setup AP (captive portal) is active. */
bool      net_ap_active(void);

/* Stores credentials in NVS. Takes effect after a reboot. */
esp_err_t net_save_credentials(const char *ssid, const char *password);
esp_err_t net_forget_credentials(void);
