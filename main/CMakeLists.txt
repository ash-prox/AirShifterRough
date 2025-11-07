idf_component_register(SRCS "security.c" "mqtt.c" "biz_logix.c" 
                            "parser.c" 
                            "wifi_manager.c" 
                            "main.c" 
                            "gatt_svr.c"
                    PRIV_REQUIRES bt nvs_flash mqtt esp_system
                    INCLUDE_DIRS "."
                    #EMBED_FILES "hivemq_ca.pem")
)

target_add_binary_data(${COMPONENT_LIB} "hivemq_ca.pem" TEXT)

