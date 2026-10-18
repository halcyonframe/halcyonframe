idf_component_register(
  SRCS "main.c" "epd.c" "storage.c" "wifi.c" "web.c" "display.c" "pmic.cpp"
  INCLUDE_DIRS "."
  PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_driver_i2c esp_driver_sdmmc fatfs
                esp_wifi esp_netif esp_event esp_http_server nvs_flash esp_timer XPowersLib
  EMBED_TXTFILES "www/index.html")
