import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"


class DeviceSettingsWiringTests(unittest.TestCase):
    def test_online_credential_rotation_is_atomic_and_never_advertises_plaintext(self) -> None:
        config = self.read("pet_config.c")
        network = self.read("pet_network.c")
        app = self.read("app_main.c")
        self.assertIn("pet_config_stage_credential", config)
        self.assertIn("pet_config_commit_credential", config)
        self.assertIn('nvs_set_str(nvs, "token", pending_token)', config)
        self.assertIn('cJSON_CreateString("credential-rotation-v1")', network)
        self.assertIn('"device.credential.prepared"', network)
        self.assertIn('"device.credential.committed"', network)
        self.assertIn("APP_EVENT_CREDENTIAL_PREPARE", app)
        self.assertIn("APP_EVENT_CREDENTIAL_COMMIT", app)

    def test_wifi_update_preserves_and_restores_the_working_profile(self) -> None:
        config = self.read("pet_config.c")
        network = self.read("pet_network.c")
        self.assertIn("bool have_current", config)
        self.assertIn("pet_config_restore_wifi_fallback", config)
        self.assertIn("if (s_wifi_using_fallback)\n                pet_config_restore_wifi_fallback();", network)

    def test_inventory_reports_version_hash_size_format_and_source(self) -> None:
        catalog = self.read("pet_face_catalog.h")
        packs = self.read("pet_face_pack.c")
        network = self.read("pet_network.c")
        self.assertIn("char sha256[65]", catalog)
        self.assertIn("mbedtls_sha256", packs)
        for field in ("packVersion", "sha256", "sizeBytes", "formatVersion", "source", "compatible"):
            self.assertIn(f'"{field}"', network)

    def test_quick_settings_use_one_bottom_done_and_monochrome_theme(self):
        face = (MAIN / "pet_face.c").read_text(encoding="utf-8")
        self.assertNotIn("done_top", face)
        self.assertEqual(face.count('create_button(s_face.settings_panel, "Done"'), 1)
        self.assertIn("LV_ALIGN_BOTTOM_MID", face)
        self.assertIn("SETTINGS_BG = 0x17191c", face)
        self.assertIn("SETTINGS_SURFACE = 0x24272b", face)
        self.assertIn("style_settings_dropdown", face)
        self.assertIn("style_settings_slider", face)

    def read(self, filename: str) -> str:
        return (MAIN / filename).read_text(encoding="utf-8")

    def test_battery_adapter_is_read_only_and_settings_scoped(self) -> None:
        battery = self.read("pet_battery.c")
        self.assertIn("i2c_master_transmit_receive", battery)
        self.assertNotIn("bq27220_create", battery.lower())
        for forbidden in ("unseal", "reset", "write", "configure"):
            self.assertNotIn(f"bq27220_{forbidden}", battery.lower())
        app = self.read("app_main.c")
        opened = app[app.index("case APP_EVENT_SETTINGS_OPENED:"):
                     app.index("case APP_EVENT_SETTINGS_CLOSED:")]
        closed = app[app.index("case APP_EVENT_SETTINGS_CLOSED:"):
                     app.index("case APP_EVENT_WIFI_SCAN:")]
        self.assertIn("pet_battery_set_enabled(true)", opened)
        self.assertIn("pet_battery_set_enabled(false)", closed)

    def test_brightness_and_shake_defaults_are_validated_and_persisted(self) -> None:
        config = self.read("pet_config.c")
        self.assertIn("config->brightness = PET_BRIGHTNESS_DEFAULT", config)
        self.assertIn("config->shake_sensitivity = PET_SHAKE_SENSITIVITY_DEFAULT", config)
        self.assertIn('nvs_get_u8(nvs, "brightness"', config)
        self.assertIn('nvs_get_u8(nvs, "shake_sens"', config)
        self.assertIn('store_u8("brightness", brightness)', config)
        self.assertIn('store_u8("shake_sens", sensitivity)', config)

    def test_wifi_profiles_are_remembered_without_logging_passwords(self) -> None:
        config_h = self.read("pet_config.h")
        config = self.read("pet_config.c")
        network = self.read("pet_network.c")
        app = self.read("app_main.c")
        self.assertIn("wifi_fallback_ssid", config_h)
        self.assertIn("wifi_fallback_password", config_h)
        self.assertIn("PET_WIFI_PROFILE_MAX 8", config_h)
        self.assertIn('read_string(nvs, "ssid_alt"', config)
        self.assertIn('read_string(nvs, "password_alt"', config)
        self.assertIn("pet_config_store_wifi_fallback", config)
        self.assertIn('nvs_set_u8(nvs, "wifi_count", count)', config)
        self.assertIn('"wifi_ssid_"', config)
        self.assertIn('"wifi_pass_"', config)
        self.assertIn("pet_config_recall_wifi", config)
        self.assertIn("pet_config_wifi_saved", config)
        self.assertIn("connect_wifi_profile(!s_wifi_using_fallback)", network)
        self.assertIn("WIFI_RETRIES_PER_PROFILE", network)
        self.assertIn("pet_config_wifi_saved(networks[network_count].ssid)",
                      "".join(network.split()))
        wifi_join = app[app.index("case APP_EVENT_WIFI_JOIN:"):]
        self.assertIn("pet_config_recall_wifi(event.ssid", wifi_join)
        self.assertNotIn('ESP_LOGI(TAG, "connecting with %s Wi-Fi password', network)
        self.assertNotIn("password=%s", config)

    def test_settings_layout_has_battery_then_volume_brightness_and_shake(self) -> None:
        face = self.read("pet_face.c")
        battery = face.index("s_face.battery_card = lv_obj_create(content)")
        volume = face.index('lv_label_set_text(volume_label, "Volume")')
        brightness = face.index('lv_label_set_text(brightness_label, "Brightness")')
        shake = face.index('lv_label_set_text(shake_label, "Shake sensitivity")')
        self.assertLess(battery, volume)
        self.assertLess(volume, brightness)
        self.assertLess(brightness, shake)
        self.assertIn("LV_EVENT_VALUE_CHANGED", face[brightness:shake])
        self.assertIn("LV_EVENT_RELEASED", face[brightness:shake])

    def test_wifi_enter_and_ok_submit_once_through_the_password_field(self) -> None:
        face = self.read("pet_face.c")
        wiring = face[face.index("s_face.keyboard = lv_keyboard_create"):
                      face.index("lv_obj_add_flag(s_face.wifi_password_panel", face.index("s_face.keyboard = lv_keyboard_create"))]
        self.assertIn(
            "lv_obj_add_event_cb(s_face.wifi_password, wifi_password_event, LV_EVENT_READY, NULL);",
            wiring,
        )
        self.assertIn(
            "lv_obj_add_event_cb(s_face.keyboard, wifi_password_event, LV_EVENT_CANCEL, NULL);",
            wiring,
        )
        self.assertNotIn(
            "lv_obj_add_event_cb(s_face.keyboard, wifi_password_event, LV_EVENT_READY",
            wiring,
        )

        handler = face[face.index("static void close_wifi_password_panel"):
                       face.index("static void animation_perf_record")]
        self.assertIn("lv_keyboard_set_textarea(s_face.keyboard, NULL);", handler)
        self.assertIn("lv_obj_add_flag(s_face.wifi_password_panel, LV_OBJ_FLAG_HIDDEN);", handler)
        self.assertIn("lv_obj_remove_flag(s_face.settings_panel, LV_OBJ_FLAG_HIDDEN);", handler)
        self.assertIn('lv_label_set_text(s_face.wifi_status, "saving; restarting...");', handler)
        self.assertEqual(handler.count("wifi_join_requested(s_face.wifi_names[selected]"), 1)

    def test_wifi_password_screen_has_visible_back_button(self) -> None:
        face = self.read("pet_face.c")
        handler = face[face.index("static void close_wifi_password_panel"):
                       face.index("static void wifi_password_event")]
        self.assertIn("static void wifi_password_back_event", handler)
        self.assertIn("close_wifi_password_panel();", handler)

        panel = face[face.index("s_face.wifi_password_panel = lv_obj_create"):
                     face.index("lv_obj_add_flag(s_face.wifi_password_panel", face.index("s_face.wifi_password_panel = lv_obj_create"))]
        self.assertIn('s_face.wifi_password_panel, "Back", 96, wifi_password_back_event', panel)
        self.assertIn("lv_obj_align(wifi_password_back, LV_ALIGN_TOP_MID, 0, 8)", panel)
        self.assertIn("lv_obj_set_size(s_face.keyboard, 270, 170)", panel)
        self.assertIn("lv_obj_align(s_face.keyboard, LV_ALIGN_TOP_MID, 0, 128)", panel)

    def test_dropdown_options_are_centered_scrollable_and_round_screen_safe(self) -> None:
        face = self.read("pet_face.c")
        helper = face[face.index("static void center_open_dropdown"):
                      face.index("static void style_settings_slider")]
        self.assertIn("lv_obj_set_width(list, 284)", helper)
        self.assertIn("lv_obj_get_height(list) > 224", helper)
        self.assertIn("lv_obj_set_scroll_dir(list, LV_DIR_VER)", helper)
        self.assertIn("lv_obj_center(list)", helper)
        self.assertIn("LV_TEXT_ALIGN_CENTER", helper)
        self.assertIn("lv_async_call(center_open_dropdown", helper)

        settings = face[face.index("static void create_settings_ui"):
                        face.index("esp_err_t pet_face_init")]
        self.assertIn("lv_obj_set_size(content, 300, 258)", settings)
        self.assertIn("lv_obj_set_style_pad_bottom(content, 86, 0)", settings)

    def test_saved_wifi_reconnects_without_opening_password_screen(self) -> None:
        face = self.read("pet_face.c")
        handler = face[face.index("static void show_wifi_password_event"):
                       face.index("static void close_wifi_password_panel")]
        self.assertIn('"already connected"', handler)
        self.assertIn("s_face.wifi_saved[selected]", handler)
        self.assertIn('wifi_join_requested(s_face.wifi_names[selected], "")',
                      handler)
        self.assertLess(handler.index("s_face.wifi_saved[selected]"),
                        handler.index("lv_textarea_set_text"))

        listing = face[face.index("void pet_face_set_wifi_networks"):
                       face.index("void pet_face_set_wifi_status")]
        self.assertIn('" (connected)"', listing)
        self.assertIn('" (saved)"', listing)
        self.assertIn("s_face.wifi_saved[i] = networks[i].saved || connected", listing)

    def test_repeated_wifi_scan_request_is_coalesced(self) -> None:
        network = self.read("pet_network.c")
        scan = network[network.index("esp_err_t pet_network_scan("):
                       network.index("esp_err_t pet_network_input_start")]
        self.assertIn("if (s_scan_running) return ESP_OK;", scan)
        app = self.read("app_main.c")
        wifi_event = app[app.index("case APP_EVENT_WIFI_SCAN:"):
                         app.index("case APP_EVENT_VOLUME_CHANGED:")]
        self.assertNotIn("use USB setup", wifi_event)
        self.assertIn("tap scan to retry", wifi_event)

    def test_wifi_scan_can_interrupt_a_stuck_connection_and_resume_it(self) -> None:
        network = self.read("pet_network.c")
        scan = network[network.index("esp_err_t pet_network_scan("):
                       network.index("esp_err_t pet_network_input_start")]
        self.assertIn("if (err == ESP_ERR_WIFI_STATE)", scan)
        self.assertIn("esp_wifi_disconnect()", scan)
        self.assertIn("WIFI_SCAN_START_RETRIES", scan)
        self.assertIn("WIFI_SCAN_RETRY_DELAY_MS", scan)
        self.assertIn("esp_wifi_connect();", scan)

        event = network[network.index("static void wifi_event("):
                        network.index("static void tx_task(")]
        self.assertIn("if (s_scan_suspended_reconnect) return;", event)
        finish = network[network.index("static void finish_wifi_scan"):
                         network.index("static void wifi_scan_task")]
        self.assertIn("if (s_scan_suspended_reconnect)", finish)
        self.assertIn("esp_wifi_connect();", finish)

    def test_horizontal_swipes_cycle_faces_without_replacing_vertical_gestures(self) -> None:
        face = self.read("pet_face.c")
        touch = face[face.index("static void touch_event"):face.index("static void close_settings_event")]
        self.assertIn("request_adjacent_face(dx < 0 ? 1 : -1)", touch)
        self.assertIn("dy >= 48", touch)
        self.assertIn("touch_started_at.y >= 288", touch)
        app = self.read("app_main.c")
        switching = app[app.index("case APP_EVENT_FACE_ID_CHANGED:"):
                        app.index("case APP_EVENT_RECORDING_TIMEOUT_CHANGED:")]
        self.assertIn("select_active_profile(event.face_id)", switching)
        self.assertIn("pet_config_store_face_id(event.face_id)", switching)
        self.assertIn("conversation_is_active()", switching)
        self.assertIn("pet_network_cancel", switching)

    def test_face_picker_is_catalog_driven_and_stable_id_persisted(self) -> None:
        face = self.read("pet_face.c")
        self.assertIn("pet_face_pack_list(catalog", face)
        self.assertIn("catalog[index].name", face)
        self.assertIn("catalog[index].id", face)
        self.assertNotIn("Classic\\nAI Pet", face)
        config = self.read("pet_config.c")
        self.assertIn('nvs_get_str(nvs, "face_id"', config)
        self.assertIn('nvs_set_str(nvs, "face_id"', config)

    def test_settings_sync_waits_for_durable_ack_and_retries_rejection(self) -> None:
        network = self.read("pet_network.c")
        synced = network[network.index('} else if (!strcmp(type->valuestring, "pet.settings"))'):
                         network.index('} else if (!strcmp(type->valuestring, "pet.state"))')]
        self.assertIn("bool unchanged = synced.version == s_config.settings_version", synced)
        self.assertIn("s_pending_synced_settings = *settings", network)
        self.assertIn("s_queued_synced_settings = synced", synced)
        self.assertIn("dispatch_synced_settings(&synced)", synced)
        self.assertNotIn("s_config.settings_version = synced.version", synced)
        self.assertIn("installed_face_id(face_id->valuestring)", synced)
        ack = network[network.index("esp_err_t pet_network_synced_settings_result"):
                      network.index("esp_err_t pet_network_set_ai_preferences")]
        self.assertIn("if (applied)", ack)
        self.assertIn("s_config.settings_version = settings->version", ack)
        self.assertIn("dispatch_synced_settings(&queued)", ack)
        self.assertIn("refresh_durable_settings()", ack)
        app = self.read("app_main.c")
        apply = app[app.index("static esp_err_t apply_synced_settings_transaction"):
                    app.index("static void app_event_task")]
        self.assertLess(apply.index("apply_active_profile()"),
                        apply.index("pet_config_store_synced_settings"))
        self.assertIn("rollback_synced_settings", apply)
        event = app[app.index("case APP_EVENT_SYNCED_SETTINGS:"):
                    app.index("case APP_EVENT_WIFI_JOIN:")]
        compact_event = "".join(event.split())
        self.assertIn("pet_network_synced_settings_result(synced,true)", compact_event)
        self.assertIn("pet_network_synced_settings_result(synced,false)", compact_event)
        self.assertIn("publish_local_settings()", event)
        profile = network[network.index("static esp_err_t set_profile_preferences"):
                          network.index("esp_err_t pet_network_set_face_context")]
        self.assertIn("bool changed =", profile)
        self.assertIn("if (announce && changed && s_websocket_ready) send_hello();", profile)

    def test_local_face_change_publishes_before_profile_hello(self) -> None:
        app = self.read("app_main.c")
        switching = app[app.index("case APP_EVENT_FACE_ID_CHANGED:"):
                        app.index("case APP_EVENT_RECORDING_TIMEOUT_CHANGED:")]
        self.assertIn("apply_active_profile_internal(false)", switching)
        self.assertLess(switching.index("publish_local_settings();"),
                        switching.index("pet_network_announce_profile_preferences();"))
        network = self.read("pet_network.c")
        staging = network[network.index("esp_err_t pet_network_stage_profile_preferences"):
                          network.index("esp_err_t pet_network_set_face_context")]
        self.assertIn("set_profile_preferences(profile, false)", staging)
        self.assertIn("if (s_websocket_ready) send_hello();", staging)

    def test_ai_identity_is_persisted_and_never_derived_from_visual_face(self) -> None:
        config_h = self.read("pet_config.h")
        config = self.read("pet_config.c")
        self.assertIn("char ai_pet_id[PET_AI_PET_ID_MAX]", config_h)
        self.assertIn('nvs_get_str(nvs, "ai_pet_id"', config)
        self.assertIn('nvs_set_str(nvs, "ai_pet_id"', config)
        app = self.read("app_main.c")
        listening = app[app.index("static esp_err_t start_listening"):
                        app.index("static bool conversation_is_active")]
        self.assertIn("pet_network_get_ai_pet_id", listening)
        self.assertNotIn("pet_face_get_agent_id", listening)
        switching = app[app.index("case APP_EVENT_FACE_ID_CHANGED:"):
                        app.index("case APP_EVENT_RECORDING_TIMEOUT_CHANGED:")]
        self.assertNotIn("pet_network_set_ai_pet_id", switching)
        network = self.read("pet_network.c")
        audio_start = network[network.index("static esp_err_t send_turn_start"):
                              network.index("esp_err_t pet_network_send_microphone")]
        self.assertIn('cJSON_AddStringToObject(root, "aiPetId"', audio_start)
        self.assertNotIn('"faceId"', audio_start)

    def test_face_context_is_ready_before_network_handshake(self) -> None:
        app = self.read("app_main.c")
        startup = app[app.rindex("pet_face_set_state(PET_FACE_CONNECTING)"):]
        self.assertLess(startup.index("refresh_network_face_context();"),
                        startup.index("pet_network_start(&config, &callbacks)"))

    def test_runtime_and_cloud_report_the_built_firmware_version(self) -> None:
        app = self.read("app_main.c")
        network = self.read("pet_network.c")
        component_cmake = self.read("CMakeLists.txt")
        project_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('esp_app_get_description()->version', app)
        self.assertIn('cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version)', network)
        self.assertIn('esp_app_format', component_cmake)
        self.assertIn('set(PROJECT_VER "aipet-6.0.2-240")', project_cmake)
        self.assertNotIn('AI Pet firmware 0.1.0 booting', app)
        self.assertNotIn('cJSON_AddStringToObject(root, "firmware", "0.1.0")', network)

    def test_speech_profiles_preserve_legacy_selection_and_offer_fish(self) -> None:
        config_h = self.read("pet_config.h")
        config = self.read("pet_config.c")
        network = self.read("pet_network.c")
        face = self.read("pet_face.c")
        self.assertIn("PET_SPEECH_PROFILE_FISH_DIRECT", config_h)
        self.assertIn('profile_err = nvs_get_u8(nvs, "stt_mode"', config)
        self.assertIn('nvs_set_u8(nvs, "speech_profile"', config)
        self.assertIn('nvs_set_u8(nvs, "stt_mode"', config)
        self.assertIn('"device.speech.profile.changed"', network)
        self.assertIn('"pet.speech.profile"', network)
        self.assertIn('"pet.stt.preference"', network)
        self.assertIn('"speechProfile"', network)
        self.assertIn("Fish Audio - S2 Pro", face)
        self.assertIn("Cloud speech pipeline", face)

    def test_config_v2_round_trips_local_controls_and_keeps_secrets_out_of_reports(self) -> None:
        config = self.read("pet_config.c")
        network = self.read("pet_network.c")
        app = self.read("app_main.c")
        self.assertIn('"config-v2"', network)
        self.assertIn('"wifi-admin-v1"', network)
        self.assertIn('"pet-inventory-v1"', network)
        self.assertIn('"device.config.proposed"', network)
        self.assertIn('"pet.config.desired"', network)
        self.assertIn('"device.config.applied"', network)
        self.assertIn('"device.config.rejected"', network)
        self.assertIn("pet_animation_profile_parse_wire(animation->valuestring", network)
        self.assertIn("pet_animation_profile_wire_value(config->animation_profile)", network)
        self.assertNotIn("static bool parse_animation_profile", network)
        self.assertNotIn("pet_animation_profile_name(config->animation_profile)", network)
        self.assertIn('"device.wifi.result"', network)
        self.assertNotIn('cJSON_AddStringToObject(root, "password"', network)
        self.assertIn('nvs_set_str(nvs, "config_fp"', config)
        self.assertIn("pet_config_forget_wifi", config)
        self.assertIn("pet_config_set_wifi_priority", config)
        self.assertIn("pet_config_list_wifi_networks", network)
        self.assertIn("saved_network_count", network)
        self.assertIn("publish_local_settings();", app[app.index("case APP_EVENT_BRIGHTNESS_CHANGED:"):app.index("case APP_EVENT_FACE_ID_CHANGED:")])
        self.assertIn("publish_local_settings();", app[app.index("case APP_EVENT_ANIMATION_PROFILE_CHANGED:"):app.index("case APP_EVENT_AI_MODE_CHANGED:")])
        self.assertIn('"Choose AI Pets in web admin"', app)

    def test_battery_health_reaches_cloud_telemetry(self) -> None:
        app = self.read("app_main.c")
        network = self.read("pet_network.c")
        self.assertIn("pet_network_set_battery_snapshot(snapshot)", app)
        self.assertIn('"batteryPercent"', network)
        self.assertIn('"batteryState"', network)
        self.assertIn('"batteryEtaMinutes"', network)


if __name__ == "__main__":
    unittest.main()
