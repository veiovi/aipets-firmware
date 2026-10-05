set(pet_vnext_sources)
if(CONFIG_PET_VNEXT_ENROLLMENT)
    set(pet_vnext_sources "pet_vnext.c" "pet_sync.c" "pet_install.c" "pet_journal.c"
        "pet_asset_store.c" "pet_control_wire.c" "pet_control_http.c" "pet_usb_wifi.c"
        "pet_pack_verify.c" "pet_release_v2.c" "pet_release_v2_pack.c" "pet_release_v2_protection.c" "pet_firmware_release.c" "pet_firmware_receipt.c" "pet_firmware_receipt_nvs.c" "pet_firmware_wire.c" "pet_firmware_image.c" "pet_ota_selection.c" "pet_firmware_control.c"
        "pet_replace.c" "pet_replace_journal.c" "pet_replace_writer.c" "pet_replace_wire.c" "pet_replace_receipt.c" "pet_replace_control.c"
        "pet_single_store.c")
    if(CONFIG_PET_POCKET_TERMINAL)
        list(APPEND pet_vnext_sources "pet_slot_inventory.c" "pet_slot_store.c" "pet_slot_protection.c")
    endif()
endif()

idf_build_get_property(AIPET_BSP_COMPONENT AIPET_BSP_COMPONENT)
idf_component_register(
    SRCS
        ${pet_vnext_sources}
        "app_main.c"
        "pet_audio.c"
        "pet_audio_board.c"
        "pet_battery.c"
        "pet_battery_format.c"
        "pet_config.c"
        "pet_enrollment.c"
        "pet_link.c"
        "pet_setup.c"
        "pet_deadline_transport.c"
        "pet_onboarding.c"
        "pet_menu_font.c"
        "pet_menu_icons.c"
        "pet_diagnostics.c"
        "pet_display.c"
        "pet_face.c"
        "pet_face_expression.c"
        "pet_face_ai.c"
        "pet_face_pack.c"
        "pet_face_profile.c"
        "pet_network.c"
        "pet_session_wire.c"
        "pet_ima_adpcm.c"
        "pet_ota.c"
        "pet_flash_layout.c"
        "pet_flash_layout_esp.c"
        "pet_pcm_gain.c"
        "pet_motion.c"
        "pet_protocol.c"
        "pet_shake_detector.c"
        "pet_sfx.c"
        "pet_sfx_plan.c"
        "pet_speech_mouth.c"
        "pet_tilt_detector.c"
        "pet_touch.c"
    INCLUDE_DIRS "."
    EMBED_FILES
        # The 18 interaction cues (sfx/README.md); other sources are not linked.
        "sfx/wake.pcm"
        "sfx/connect.pcm"
        "sfx/start.pcm"
        "sfx/stop.pcm"
        "sfx/complete.pcm"
        "sfx/cancel.pcm"
        "sfx/retry.pcm"
        "sfx/error.pcm"
        "sfx/no_voice.pcm"
        "sfx/settings_open.pcm"
        "sfx/settings_close.pcm"
        "sfx/face_swipe_0.pcm"
        "sfx/face_swipe_1.pcm"
        "sfx/face_swipe_2.pcm"
        "sfx/face_swipe_3.pcm"
        "sfx/face_swipe_4.pcm"
        "sfx/gesture_shake.pcm"
        "sfx/gesture_pop.pcm"
    REQUIRES
        esp_app_format
        esp_event
        esp-tls
        esp_http_client
        esp_partition
        spi_flash
        tcp_transport
        app_update
        bootloader_support
        mbedtls
        esp_netif
        esp_timer
        esp_hw_support
        esp_wifi
        esp_driver_usb_serial_jtag
        esp_driver_gpio
        esp_driver_i2c
        esp_adc
        espcoredump
        json
        nvs_flash
        face_core
        frame_player
        ${AIPET_BSP_COMPONENT}
        espressif__esp_lvgl_adapter
        waveshare__qmi8658
)
if(AIPET_BOARD STREQUAL "waveshare-esp32-s3-touch-amoled-2.06")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PET_BOARD_AMOLED_206=1)
elseif(AIPET_BOARD STREQUAL "waveshare-esp32-s3-touch-lcd-1.54")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PET_BOARD_LCD_154=1)
endif()
