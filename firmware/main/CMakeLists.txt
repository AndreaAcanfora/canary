idf_component_register(
    SRCS main.c net.c improv_serial.c web.c onewire.c led.c store.c
    INCLUDE_DIRS .
    PRIV_REQUIRES core nvs_flash esp_wifi esp_netif esp_event esp_eth esp_http_client
                  esp_http_server esp_driver_gpio esp_driver_uart esp_driver_usb_serial_jtag
                  esp_timer esp_app_format mbedtls)

file(READ "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" CANARY_VERSION)
string(STRIP "${CANARY_VERSION}" CANARY_VERSION)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CANARY_VERSION="${CANARY_VERSION}")
