#include "storage.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#if CONFIG_CR10_SD_MODE_SDMMC
#include "driver/sdmmc_host.h"
#else
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#endif

static const char *TAG = "storage";

#define CHECKPOINT_PATH     STORAGE_MOUNT_POINT "/.checkpoint"
#define CHECKPOINT_TMP_PATH STORAGE_MOUNT_POINT "/.checkpoint.tmp"

static sdmmc_card_t *s_card;
static volatile bool s_mounted;

#if CONFIG_CR10_SD_MODE_SPI
static bool s_spi_bus_ready;
#endif

#if CONFIG_CR10_SD_MODE_SPI
/*
 * One-time wiring diagnostic: bit-bangs the SD SPI lines slowly (~50 kHz) and
 * logs what the card answers to CMD0/CMD8. Independent of the SPI driver.
 */
#include "esp_rom_sys.h"

#define D_CS   CONFIG_CR10_SD_SPI_CS
#define D_MOSI CONFIG_CR10_SD_SPI_MOSI
#define D_SCK  CONFIG_CR10_SD_SPI_SCK
#define D_MISO CONFIG_CR10_SD_SPI_MISO

static uint8_t bb_xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(D_MOSI, (out >> i) & 1);
        esp_rom_delay_us(10);
        gpio_set_level(D_SCK, 1);
        esp_rom_delay_us(5);
        in = (in << 1) | gpio_get_level(D_MISO);
        esp_rom_delay_us(5);
        gpio_set_level(D_SCK, 0);
    }
    return in;
}

static void bb_cmd(const char *name, const uint8_t cmd[6], int resp_len)
{
    for (int i = 0; i < 6; i++) {
        bb_xfer(cmd[i]);
    }
    char line[96];
    int n = 0;
    for (int i = 0; i < 8 + resp_len; i++) {
        n += snprintf(line + n, sizeof(line) - n, "%02X ", bb_xfer(0xFF));
    }
    ESP_LOGW(TAG, "diag %s -> %s", name, line);
}

static void sd_wiring_diag(void)
{
    const int outs[] = {D_CS, D_MOSI, D_SCK};
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(outs[i]);
        gpio_set_direction(outs[i], GPIO_MODE_OUTPUT);
    }
    gpio_reset_pin(D_MISO);
    gpio_set_direction(D_MISO, GPIO_MODE_INPUT);
    gpio_set_level(D_CS, 1);
    gpio_set_level(D_MOSI, 1);
    gpio_set_level(D_SCK, 0);

    /* Is MISO driven, or floating? (card deselected) */
    gpio_set_pull_mode(D_MISO, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(200);
    int up = gpio_get_level(D_MISO);
    gpio_set_pull_mode(D_MISO, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(200);
    int down = gpio_get_level(D_MISO);
    gpio_set_pull_mode(D_MISO, GPIO_PULLUP_ONLY);
    ESP_LOGW(TAG, "diag MISO(GPIO%d) with pull-up=%d pull-down=%d -> %s", D_MISO, up, down,
             up != down ? "FLOATING (nothing drives it)" : (up ? "driven HIGH" : "driven LOW (!)"));

    /* >= 74 clocks with CS high, then CMD0 and CMD8 */
    for (int i = 0; i < 10; i++) {
        bb_xfer(0xFF);
    }
    gpio_set_level(D_CS, 0);
    static const uint8_t cmd0[6] = {0x40, 0, 0, 0, 0, 0x95};
    static const uint8_t cmd8[6] = {0x48, 0, 0, 0x01, 0xAA, 0x87};
    bb_cmd("CMD0 (expect 01)", cmd0, 0);
    bb_cmd("CMD8 (expect 01 00 00 01 AA)", cmd8, 4);
    gpio_set_level(D_CS, 1);
    bb_xfer(0xFF);

    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(outs[i]);
    }
    gpio_reset_pin(D_MISO);
}
#endif

static esp_err_t try_mount(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err;

#if CONFIG_CR10_SD_MODE_SDMMC
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_CR10_SD_FREQ_KHZ;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = CONFIG_CR10_SD_MMC_CLK;
    slot.cmd = CONFIG_CR10_SD_MMC_CMD;
    slot.d0 = CONFIG_CR10_SD_MMC_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    err = esp_vfs_fat_sdmmc_mount(STORAGE_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
#else
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = CONFIG_CR10_SD_FREQ_KHZ;

    if (!s_spi_bus_ready) {
        /* SD in SPI mode wants pull-ups on all lines; modules often lack them. */
        const int pins[] = {CONFIG_CR10_SD_SPI_CS, CONFIG_CR10_SD_SPI_MOSI, CONFIG_CR10_SD_SPI_SCK,
                            CONFIG_CR10_SD_SPI_MISO};
        for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
            gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
        }
        spi_bus_config_t bus = {
            .mosi_io_num = CONFIG_CR10_SD_SPI_MOSI,
            .miso_io_num = CONFIG_CR10_SD_SPI_MISO,
            .sclk_io_num = CONFIG_CR10_SD_SPI_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 16 * 1024,
        };
        err = spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
            return err;
        }
        s_spi_bus_ready = true;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = CONFIG_CR10_SD_SPI_CS;
    slot.host_id = SPI2_HOST;

    err = esp_vfs_fat_sdspi_mount(STORAGE_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
#endif

    if (err == ESP_OK) {
        s_mounted = true;
        ESP_LOGI(TAG, "SD mounted at %s", STORAGE_MOUNT_POINT);
        sdmmc_card_print_info(stdout, s_card);
    }
    return err;
}

static void mount_retry_task(void *arg)
{
    while (!s_mounted) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (try_mount() != ESP_OK) {
            ESP_LOGW(TAG, "SD card still not available, retrying");
        }
    }
    vTaskDelete(NULL);
}

esp_err_t storage_init(void)
{
#if CONFIG_CR10_SD_MODE_SPI
    sd_wiring_diag();
#endif
    esp_err_t err = try_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s); retrying in background", esp_err_to_name(err));
        xTaskCreate(mount_retry_task, "sd_retry", 4096, NULL, 2, NULL);
    }
    return err;
}

bool storage_mounted(void)
{
    return s_mounted;
}

void storage_card_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    *total_bytes = 0;
    *free_bytes = 0;
    if (!s_mounted) {
        return;
    }
    FATFS *fs;
    DWORD free_clusters;
    if (f_getfree("0:", &free_clusters, &fs) == FR_OK) {
        uint64_t cluster_bytes = (uint64_t)fs->csize * 512;
        *total_bytes = (uint64_t)(fs->n_fatent - 2) * cluster_bytes;
        *free_bytes = (uint64_t)free_clusters * cluster_bytes;
    }
}

bool storage_name_valid(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len == 0 || len > STORAGE_MAX_NAME || name[0] == '.' || name[0] == ' ') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == ' ' || c == '(' || c == ')' || c == '+';
        if (!ok) {
            return false;
        }
    }
    return strstr(name, "..") == NULL;
}

bool storage_path(const char *name, char *out, size_t out_len)
{
    if (!storage_name_valid(name)) {
        return false;
    }
    int n = snprintf(out, out_len, STORAGE_MOUNT_POINT "/%s", name);
    return n > 0 && (size_t)n < out_len;
}

esp_err_t storage_list(storage_list_cb_t cb, void *arg)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    DIR *dir = opendir(STORAGE_MOUNT_POINT);
    if (!dir) {
        return ESP_FAIL;
    }
    struct dirent *de;
    char path[STORAGE_PATH_MAX];
    while ((de = readdir(dir)) != NULL) {
        if (de->d_type == DT_DIR || de->d_name[0] == '.' || strlen(de->d_name) > STORAGE_MAX_NAME) {
            continue;
        }
        storage_file_info_t info = {0};
        strlcpy(info.name, de->d_name, sizeof(info.name));
        snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", info.name);
        struct stat st;
        if (stat(path, &st) == 0) {
            info.size = (uint32_t)st.st_size;
            info.mtime = st.st_mtime;
        }
        cb(&info, arg);
    }
    closedir(dir);
    return ESP_OK;
}

esp_err_t storage_delete(const char *name)
{
    char path[STORAGE_PATH_MAX];
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!storage_path(name, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }
    return unlink(path) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Simple key=value text file. Written to a temp file and renamed, so a power
 * cut in the middle of a write never leaves a truncated checkpoint behind. */
esp_err_t storage_checkpoint_save(const storage_checkpoint_t *cp)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *f = fopen(CHECKPOINT_TMP_PATH, "w");
    if (!f) {
        return ESP_FAIL;
    }
    fprintf(f, "file=%s\noffset=%" PRIu32 "\nline=%" PRIu32 "\nsize=%" PRIu32 "\nstate=%s\nreason=%s\n",
            cp->file, cp->offset, cp->line, cp->size, cp->state, cp->reason);
    fclose(f);
    unlink(CHECKPOINT_PATH);
    return rename(CHECKPOINT_TMP_PATH, CHECKPOINT_PATH) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_checkpoint_load(storage_checkpoint_t *cp)
{
    memset(cp, 0, sizeof(*cp));
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *f = fopen(CHECKPOINT_PATH, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "file")) {
            strlcpy(cp->file, v, sizeof(cp->file));
        } else if (!strcmp(k, "offset")) {
            cp->offset = strtoul(v, NULL, 10);
        } else if (!strcmp(k, "line")) {
            cp->line = strtoul(v, NULL, 10);
        } else if (!strcmp(k, "size")) {
            cp->size = strtoul(v, NULL, 10);
        } else if (!strcmp(k, "state")) {
            strlcpy(cp->state, v, sizeof(cp->state));
        } else if (!strcmp(k, "reason")) {
            strlcpy(cp->reason, v, sizeof(cp->reason));
        }
    }
    fclose(f);
    cp->valid = cp->file[0] != 0;
    return cp->valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
