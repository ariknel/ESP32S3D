if(CONFIG_CR10_PRINTER_DRY_RUN)
    set(srcs "usb_printer_sim.c")
else()
    set(srcs "usb_printer_ch34x.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_common
    PRIV_REQUIRES usb log esp_timer
)
