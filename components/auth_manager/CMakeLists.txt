idf_component_register(SRCS "auth_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES 
                            storage_manager
                            esp_http_server
                            esp_timer)