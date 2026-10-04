#include "net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "net";

#define NVS_NS        "net"
#define BIT_CONNECTED BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta, *s_ap;
static esp_timer_handle_t s_retry_timer;
static net_mode_t s_mode;
static char s_ssid[33];
static char s_ip[16];
static int s_retry_ms = 2000;
static bool s_sntp_started;

static void retry_cb(void *arg)
{
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        s_ip[0] = 0;
        ESP_LOGW(TAG, "STA disconnected (reason %d), retry in %d ms", d->reason, s_retry_ms);
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000);
        /* back off; scanning while the setup AP is up disturbs its clients */
        int max = s_mode == NET_MODE_APSTA ? 60000 : 15000;
        s_retry_ms = s_retry_ms * 2 > max ? max : s_retry_ms * 2;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "client joined setup AP");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "connected to '%s', IP %s  ->  http://%s.local/", s_ssid, s_ip, CONFIG_CR10_HOSTNAME);
        s_retry_ms = 2000;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        if (!s_sntp_started) {
            /* file timestamps; harmless when there is no internet access */
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            cfg.start = true;
            esp_netif_sntp_init(&cfg);
            s_sntp_started = true;
        }
    }
}

static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK && ssid[0];
    if (ok && nvs_get_str(h, "pass", pass, &pass_len) != ESP_OK) {
        pass[0] = 0;
    }
    nvs_close(h);
    return ok;
}

esp_err_t net_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (password && strlen(password) > 63)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t net_forget_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_all(h);
        err = nvs_commit(h);
        nvs_close(h);
    }
    return err;
}

/*
 * Captive-portal DNS: while the setup AP is up, every A query is answered with
 * the AP address. Phones then probe their "connectivity check" URL, hit our
 * HTTP server, get redirected to /setup and pop up the sign-in page by itself.
 */
static uint32_t s_ap_ip; /* network byte order */

static void captive_dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "captive DNS: cannot bind port 53");
        if (sock >= 0) {
            close(sock);
        }
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &flen);
        if (n < 12 || (buf[2] & 0x80)) {
            continue; /* too short or not a query */
        }
        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if (qdcount != 1) {
            continue;
        }
        /* walk the question name */
        int p = 12;
        while (p < n && buf[p] != 0) {
            p += buf[p] + 1;
        }
        if (p + 5 > n) {
            continue;
        }
        uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
        int qend = p + 5;

        buf[2] = 0x81; /* response, recursion desired */
        buf[3] = 0x80; /* recursion available, no error */
        buf[6] = buf[7] = 0;        /* ancount */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = qend;
        if (qtype == 1 /* A */) {
            buf[7] = 1;
            const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
            memcpy(buf + len, ans, sizeof(ans));
            len += sizeof(ans);
            memcpy(buf + len, &s_ap_ip, 4);
            len += 4;
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, flen);
    }
}

static void start_ap(wifi_mode_t mode)
{
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, CONFIG_CR10_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(CONFIG_CR10_AP_SSID);
    ap.ap.max_connection = 4;
    ap.ap.channel = 6;
    if (strlen(CONFIG_CR10_AP_PASSWORD) >= 8) {
        strlcpy((char *)ap.ap.password, CONFIG_CR10_AP_PASSWORD, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    /* Hand out our own address as DNS server so the captive DNS below is used. */
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap, &ip);
    esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = ip.ip.addr};
    uint8_t offer_dns = 1;
    esp_netif_dhcps_stop(s_ap);
    esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
    esp_netif_dhcps_start(s_ap);

    s_ap_ip = ip.ip.addr;
    xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 3, NULL);
    ESP_LOGW(TAG, "setup AP '%s' up -> http://192.168.4.1/setup", CONFIG_CR10_AP_SSID);
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed");
        return;
    }
    mdns_hostname_set(CONFIG_CR10_HOSTNAME);
    mdns_instance_name_set("CR-10 Print Server");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

esp_err_t net_init(void)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, CONFIG_CR10_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    char pass[65] = {0};
    if (!load_credentials(s_ssid, sizeof(s_ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "no Wi-Fi credentials stored");
        s_mode = NET_MODE_AP;
        start_ap(WIFI_MODE_AP);
        ESP_ERROR_CHECK(esp_wifi_start());
        start_mdns();
        return ESP_OK;
    }

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    s_mode = NET_MODE_STA;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); /* lower latency for the web UI */
    start_mdns();

    ESP_LOGI(TAG, "connecting to '%s'...", s_ssid);
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(CONFIG_CR10_STA_CONNECT_TIMEOUT_S * 1000));
    if (!(bits & BIT_CONNECTED)) {
        ESP_LOGW(TAG, "could not join '%s', enabling setup AP (STA keeps retrying)", s_ssid);
        s_mode = NET_MODE_APSTA;
        start_ap(WIFI_MODE_APSTA);
    }
    return ESP_OK;
}

bool net_ap_active(void)
{
    return s_mode != NET_MODE_STA;
}

void net_get_info(net_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->mode = s_mode;
    info->sta_connected = (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
    strlcpy(info->sta_ssid, s_ssid, sizeof(info->sta_ssid));
    strlcpy(info->sta_ip, s_ip, sizeof(info->sta_ip));
    strlcpy(info->hostname, CONFIG_CR10_HOSTNAME, sizeof(info->hostname));
    if (info->sta_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            info->rssi = ap.rssi;
        }
    }
    if (s_mode != NET_MODE_STA) {
        strlcpy(info->ap_ssid, CONFIG_CR10_AP_SSID, sizeof(info->ap_ssid));
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_ap, &ip) == ESP_OK) {
            snprintf(info->ap_ip, sizeof(info->ap_ip), IPSTR, IP2STR(&ip.ip));
        }
    }
}
