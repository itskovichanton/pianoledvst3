# led_protocol.c лежит в shared/ и компилируется также Mac-стороной:
# обе стороны провода используют буквально один и тот же кодек.
idf_component_register(
    SRCS "main.c" "led_guard.c" "../../shared/led_protocol.c"
    INCLUDE_DIRS "." "../../shared"
    REQUIRES driver esp_timer esp_driver_usb_serial_jtag
)
