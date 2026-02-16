idf_component_register(
    SRCS "keybridge.cpp" "esp_hid_gap.c"
    INCLUDE_DIRS "."
    REQUIRES bt esp_hid usb nvs_flash
    PRIV_REQUIRES framework-arduinoespressif32
)
