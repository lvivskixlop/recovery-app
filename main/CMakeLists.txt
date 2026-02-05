idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "include"
                        REQUIRES 
                            storage_manager
                            wifi_manager
                            server_manager
                            auth_manager)
