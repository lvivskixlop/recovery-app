idf_component_register(SRCS "server_manager.c"
                        INCLUDE_DIRS "include"
                        REQUIRES 
                            esp_http_server
                            app_update
                            storage_manager
                            auth_manager)
