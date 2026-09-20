idf_component_register(
    SRCS "ota.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        esp_https_ota
        esp_http_client
        esp_http_server
        app_update
        esp_app_format
        cjson
        esp_timer
)
