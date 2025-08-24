file(GLOB paginas ../spiffs/*.*)
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES wifi_provisioning esp_http_client json esp_http_server spiffs
                    EMBED_FILES ${paginas})