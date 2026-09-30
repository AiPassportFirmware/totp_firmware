idf_component_register(
    SRCS "app_main.c"
         "app_totp.c"
         "app_lock.c"
         "app_kb.c"
         "app_lang.c"
         "app_store.c"
         "app_wifi.c"
         "app_server.c"
         "app_ui.c"
         "../assets/fonts/app_font_sc_16.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi esp_http_server nvs_flash
)
# 说明:demo_*.c / ui_pixel*.c / main.c 是基线 BSP 演示的参考实现,保留在仓库中
# 供宿主测试(tests/test_demo_*.c)与文档引用,但不再编入本应用固件。
