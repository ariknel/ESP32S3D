#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#define STORAGE_MOUNT_POINT  "/sdcard"
#define STORAGE_MAX_NAME     64
#define STORAGE_PATH_MAX     (sizeof(STORAGE_MOUNT_POINT) + STORAGE_MAX_NAME + 2)

typedef struct {
    char     name[STORAGE_MAX_NAME + 1];
    uint32_t size;
    time_t   mtime;
} storage_file_info_t;

/* Last known print position, written periodically while printing. */
typedef struct {
    bool     valid;
    char     file[STORAGE_MAX_NAME + 1];
    uint32_t offset;     /* bytes of the file acknowledged by the printer */
    uint32_t line;       /* file line number (1-based, counting every line) */
    uint32_t size;       /* file size */
    char     state[16];  /* printing / paused / finished / failed / cancelled */
    char     reason[64]; /* failure reason, if any */
} storage_checkpoint_t;

/* Mounts the card; if it is missing, a background task keeps retrying. */
esp_err_t storage_init(void);
bool      storage_mounted(void);
void      storage_card_info(uint64_t *total_bytes, uint64_t *free_bytes);

/* Validates a user supplied file name (no paths, safe characters, length). */
bool      storage_name_valid(const char *name);
/* Builds "/sdcard/<name>". Returns false if the name is invalid. */
bool      storage_path(const char *name, char *out, size_t out_len);

/* Calls cb for every regular, non-hidden file in the card root. */
typedef void (*storage_list_cb_t)(const storage_file_info_t *info, void *arg);
esp_err_t storage_list(storage_list_cb_t cb, void *arg);
esp_err_t storage_delete(const char *name);

esp_err_t storage_checkpoint_save(const storage_checkpoint_t *cp);
esp_err_t storage_checkpoint_load(storage_checkpoint_t *cp);
