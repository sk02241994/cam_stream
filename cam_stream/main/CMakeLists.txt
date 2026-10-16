idf_component_register(SRCS "cam_stream.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp32-camera esp_http_server esp_wifi nvs_flash)
