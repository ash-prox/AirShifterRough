idf_component_register(SRCS "parser.c" "parser-oldbitch1.c" "security.c" "mqtt.c" "biz_logix.c" 
                            
                            "wifi_manager.c" 
                            "main.c" 
                            "gatt_svr.c"
                    PRIV_REQUIRES bt nvs_flash mqtt esp_system esp_wifi
                    INCLUDE_DIRS "include/."
                    #EMBED_FILES "hivemq_ca.pem")
)

target_add_binary_data(${COMPONENT_LIB} "certs/cert.pem" TEXT)

