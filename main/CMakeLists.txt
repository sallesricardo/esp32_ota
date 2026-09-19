idf_component_register(SRCS "blink_example_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES
                           driver
                           esp_driver_gpio
                           nvs_flash
                           esp_wifi
                           esp_https_ota
                           esp_http_client
                           esp_http_server
                           app_update
                           esp_app_format
                           cjson
                           esp_timer
)
