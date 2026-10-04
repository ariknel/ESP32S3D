idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_sdspi esp_driver_spi esp_driver_gpio vfs
)
