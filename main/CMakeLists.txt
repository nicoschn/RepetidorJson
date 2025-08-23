idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES wifi_provisioning esp_http_client json)