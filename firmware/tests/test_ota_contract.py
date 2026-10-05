import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"


class OtaContractTests(unittest.TestCase):
    def test_ota_is_inactive_slot_hash_verified_and_rollback_gated(self) -> None:
        ota = (MAIN / "pet_ota.c").read_text(encoding="utf-8")
        header = (MAIN / "pet_ota.h").read_text(encoding="utf-8")
        self.assertIn("esp_ota_get_next_update_partition(NULL)", ota)
        self.assertIn("mbedtls_sha256_finish", ota)
        self.assertIn("OTA_HASH_MISMATCH", ota)
        self.assertIn("esp_ota_set_boot_partition", ota)
        self.assertIn("ESP_OTA_IMG_PENDING_VERIFY", ota)
        self.assertIn("esp_ota_mark_app_valid_cancel_rollback", ota)
        self.assertIn("PET_OTA_HEALTH_PRE_ENROLLMENT", header)
        self.assertIn("PET_OTA_HEALTH_ENROLLED_NO_PET", header)
        self.assertIn("PET_OTA_HEALTH_INSTALLED_PET", header)
        gate = ota[ota.index("esp_err_t pet_ota_confirm_health_stage("):]
        self.assertNotIn("wifi", gate.lower())
        self.assertIn("authenticated_control", gate)
        self.assertIn("pet_runtime_ready", gate)
        self.assertIn("OTA_HEALTH_DEADLINE_MS", ota)
        self.assertIn("esp_restart();", ota)

    def test_ota_never_logs_token(self) -> None:
        ota = (MAIN / "pet_ota.c").read_text(encoding="utf-8")
        network = (MAIN / "pet_network.c").read_text(encoding="utf-8")
        self.assertIn('"Authorization"', ota)
        self.assertNotIn('ESP_LOGI(TAG, "token', ota)
        self.assertIn('cJSON_CreateString("ota-v1")', network)
        self.assertIn('"device.ota.status"', network)

    def test_ota_does_not_touch_nvs_or_assets(self) -> None:
        ota = (MAIN / "pet_ota.c").read_text(encoding="utf-8")
        self.assertNotIn("nvs_", ota)
        self.assertNotIn("assets", ota.lower())
        self.assertNotIn("esp_partition_erase_range", ota)

    def test_migrated_layout_rejects_legacy_commands_before_acceptance(self) -> None:
        ota = (MAIN / "pet_ota.c").read_text(encoding="utf-8")
        start = ota[ota.index("esp_err_t pet_ota_start("):ota.index("esp_err_t pet_ota_confirm_healthy(")]
        self.assertLess(start.index("pet_flash_layout_read"), start.index("malloc"))
        self.assertLess(start.index("pet_flash_layout_allows_legacy_ota"), start.index('"accepted"'))
        self.assertIn("return ESP_ERR_NOT_SUPPORTED", start)

    def test_legacy_guard_dependencies_are_unconditional(self) -> None:
        cmake = (MAIN / "CMakeLists.txt").read_text(encoding="utf-8")
        unconditional = cmake[cmake.index("idf_component_register("):]
        for source in ("pet_ota.c", "pet_flash_layout.c", "pet_flash_layout_esp.c"):
            self.assertIn('"' + source + '"', unconditional)


if __name__ == "__main__":
    unittest.main()
