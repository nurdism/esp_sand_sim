idf_component_register(
    SRCS "main.c"
         "sand_board.c"
         "sand_sim.c"
         "sand_menu.c"
         "sand_render.c"
         "sand_scene.c"
         "sand_input.c"
         "sand_usb.c"
    INCLUDE_DIRS "."
    REQUIRES espressif__esp_lcd_co5300 waveshare__esp_lcd_touch_cst9217
             nvs_flash esp_partition
             esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c esp_timer
             espressif__esp_tinyusb espressif__libpng
             wear_levelling fatfs vfs
)
