idf_component_register(SRCS "main.c"
                            "ePaper_Config.c"
                            "EPD_7in3f.c"
                            "ImageData2.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_driver_gpio esp_driver_spi
                    INCLUDE_DIRS "." "GUI" "Fonts")
