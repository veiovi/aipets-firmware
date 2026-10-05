#include "pet_motion.h"

#include <math.h>
#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "face_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pet_shake_detector.h"
#include "pet_tilt_detector.h"
#include "qmi8658.h"

static const char *TAG = "pet_motion";
static qmi8658_dev_t s_imu;
static pet_motion_gesture_callback_t s_callback;
static pet_motion_enabled_callback_t s_enabled_callback;
static volatile uint8_t s_sensitivity;

static uint8_t gesture_for_tilt(pet_tilt_direction_t direction)
{
    switch (direction) {
        case PET_TILT_LEFT: return FC_GESTURE_SPIN_CCW;
        case PET_TILT_RIGHT: return FC_GESTURE_SPIN_CW;
        case PET_TILT_FORWARD: return FC_GESTURE_NOD;
        case PET_TILT_BACK: return FC_GESTURE_HEARTBEAT;
        default: return FC_GESTURE_NONE;
    }
}

static pet_motion_trigger_t trigger_for_tilt(pet_tilt_direction_t direction)
{
    switch (direction) {
        case PET_TILT_LEFT: return PET_MOTION_TRIGGER_TILT_LEFT;
        case PET_TILT_RIGHT: return PET_MOTION_TRIGGER_TILT_RIGHT;
        case PET_TILT_FORWARD: return PET_MOTION_TRIGGER_TILT_FORWARD;
        default: return PET_MOTION_TRIGGER_TILT_BACK;
    }
}

static void motion_task(void *arg)
{
    (void)arg;
    pet_shake_detector_t detector = {0};
    pet_tilt_detector_t tilt_detector = {0};
    pet_shake_detector_set_sensitivity(&detector, s_sensitivity);
    unsigned consecutive_errors = 0;
    unsigned consecutive_not_ready = 0;
    bool first_sample = true;
    bool detector_enabled = false;
    uint8_t applied_sensitivity = s_sensitivity;
    float peak_linear = 0.0f;
    float peak_delta = 0.0f;
    float min_x = 0.0f;
    float min_y = 0.0f;
    float min_z = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    float max_z = 0.0f;
    bool stats_initialized = false;
    uint32_t activity_log_ms = 0;

    for (;;) {
        /* Shake sensitivity 0 disables shake only. Deliberate tilt remains an
         * independent idle interaction. */
        bool enabled = s_enabled_callback && s_enabled_callback();
        if (!enabled || applied_sensitivity != s_sensitivity) {
            if (detector_enabled || applied_sensitivity != s_sensitivity) {
                applied_sensitivity = s_sensitivity;
                pet_shake_detector_set_sensitivity(&detector, applied_sensitivity);
                pet_tilt_detector_reset(&tilt_detector);
                peak_linear = 0.0f;
                peak_delta = 0.0f;
                stats_initialized = false;
                activity_log_ms = 0;
            }
            detector_enabled = false;
            vTaskDelay(pdMS_TO_TICKS(PET_SHAKE_SAMPLE_MS));
            continue;
        }
        if (!detector_enabled) {
            pet_shake_detector_set_sensitivity(&detector, applied_sensitivity);
            pet_tilt_detector_reset(&tilt_detector);
            detector_enabled = true;
        }

        bool ready = false;
        esp_err_t result = qmi8658_is_data_ready(&s_imu, &ready);
        if (result == ESP_OK && !ready) {
            if (++consecutive_not_ready == 1 || consecutive_not_ready % 200 == 0) {
                ESP_LOGW(TAG, "accelerometer data not ready (consecutive=%u)",
                         consecutive_not_ready);
            }
            vTaskDelay(pdMS_TO_TICKS(PET_SHAKE_SAMPLE_MS));
            continue;
        }

        float x;
        float y;
        float z;
        if (result == ESP_OK) result = qmi8658_read_accel(&s_imu, &x, &y, &z);
        if (result == ESP_OK) {
            consecutive_errors = 0;
            consecutive_not_ready = 0;
            if (first_sample) {
                first_sample = false;
                ESP_LOGI(TAG, "accelerometer online x=%.2f y=%.2f z=%.2f m/s^2",
                         (double)x, (double)y, (double)z);
            }
            if (!stats_initialized) {
                min_x = max_x = x;
                min_y = max_y = y;
                min_z = max_z = z;
                stats_initialized = true;
            } else {
                if (x < min_x) min_x = x;
                if (y < min_y) min_y = y;
                if (z < min_z) min_z = z;
                if (x > max_x) max_x = x;
                if (y > max_y) max_y = y;
                if (z > max_z) max_z = z;
            }

            bool shake = pet_shake_detector_update(&detector, x, y, z,
                                                    PET_SHAKE_SAMPLE_MS);
            if (shake) {
                uint8_t gesture = pet_shake_pick_gesture(esp_random());
                ESP_LOGI(TAG, "physical shake detected gesture=%u linear=%.2f threshold=%.2f noise=%.2f",
                         gesture, (double)detector.last_linear_accel,
                         (double)detector.effective_threshold,
                         (double)detector.noise_mean);
                pet_tilt_detector_reset(&tilt_detector);
                if (s_callback) s_callback(gesture, PET_MOTION_TRIGGER_SHAKE);
            }

            /* The Waveshare QMI8658 component can report a biased absolute
             * magnitude on this board (one axis may sit near its full-scale
             * sentinel), while relative samples and jerk remain stable. Do not
             * require the vector magnitude to equal 1 g: that made every held
             * tilt ineligible. A stable low-jerk window is the correct gate for
             * a relative-orientation interaction. */
            bool motion_quiet = detector.last_jerk < 1.0f;
            pet_tilt_direction_t tilt = shake ? PET_TILT_NONE :
                pet_tilt_detector_update(&tilt_detector, x, y, z,
                                         PET_SHAKE_SAMPLE_MS, motion_quiet);
            if (tilt != PET_TILT_NONE) {
                uint8_t gesture = gesture_for_tilt(tilt);
                ESP_LOGI(TAG, "deliberate tilt detected direction=%u amount=%.2f gesture=%u",
                         tilt, (double)tilt_detector.last_tilt_amount, gesture);
                if (s_callback) s_callback(gesture, trigger_for_tilt(tilt));
            }

            if (detector.last_linear_accel > peak_linear) {
                peak_linear = detector.last_linear_accel;
            }
            if (detector.last_jerk > peak_delta) peak_delta = detector.last_jerk;
            activity_log_ms += PET_SHAKE_SAMPLE_MS;
            if (activity_log_ms >= 2000) {
                /* Always expose a bounded two-second summary while motion input
                 * is eligible. This avoids flash wear and lets a receive-only
                 * serial capture diagnose sensor scale, axes, and thresholds. */
                ESP_LOGI(TAG, "motion telemetry sensitivity=%u threshold=%.2f noise=%.2f peakLinear=%.2f peakDelta=%.2f impulses=%u rejection=%u cooldownMs=%lu tilt=%.2f span=[%.2f,%.2f,%.2f]",
                         applied_sensitivity, (double)detector.effective_threshold,
                         (double)detector.noise_mean, (double)peak_linear,
                         (double)peak_delta, detector.impulse_count,
                         detector.last_rejection,
                         (unsigned long)detector.cooldown_ms,
                         (double)tilt_detector.last_tilt_amount,
                         (double)(max_x - min_x), (double)(max_y - min_y),
                         (double)(max_z - min_z));
                activity_log_ms = 0;
                peak_linear = 0.0f;
                peak_delta = 0.0f;
                stats_initialized = false;
            }
        } else if (++consecutive_errors == 1 || consecutive_errors % 200 == 0) {
            ESP_LOGW(TAG, "accelerometer read failed: %s (consecutive=%u)",
                     esp_err_to_name(result), consecutive_errors);
        }
        vTaskDelay(pdMS_TO_TICKS(PET_SHAKE_SAMPLE_MS));
    }
}

esp_err_t pet_motion_start(pet_motion_gesture_callback_t callback,
                           pet_motion_enabled_callback_t enabled_callback,
                           uint8_t sensitivity)
{
    ESP_RETURN_ON_FALSE(callback, ESP_ERR_INVALID_ARG, TAG, "gesture callback required");
    ESP_RETURN_ON_FALSE(enabled_callback, ESP_ERR_INVALID_ARG, TAG,
                        "enabled callback required");
    ESP_RETURN_ON_FALSE(sensitivity <= 100, ESP_ERR_INVALID_ARG, TAG,
                        "invalid sensitivity");
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG, "I2C bus unavailable");

    ESP_RETURN_ON_ERROR(qmi8658_init(&s_imu, bus, QMI8658_ADDRESS_HIGH),
                        TAG, "initialize QMI8658");
    /* Reset the sensor itself on every ESP boot. An ESP USB reset does not
     * power-cycle the QMI8658, so stale modes and a locked sample otherwise
     * survive firmware restarts. The vendor helper's reset routine writes the
     * wrong register; 0x60/0xB0 is the reset sequence from the QMI8658C data
     * sheet. */
    ESP_RETURN_ON_ERROR(qmi8658_write_register(&s_imu, 0x60, 0xB0),
                        TAG, "reset QMI8658");
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t reset_status = 0;
    ESP_RETURN_ON_ERROR(qmi8658_read_register(&s_imu, 0x4D, &reset_status, 1),
                        TAG, "read QMI8658 reset status");
    ESP_RETURN_ON_FALSE(reset_status == 0x80, ESP_ERR_INVALID_RESPONSE, TAG,
                        "QMI8658 reset did not complete (status=0x%02x)",
                        reset_status);

    /* Address auto-increment is required for the six-byte acceleration burst.
     * Select little-endian output because qmi8658_read_accel decodes LSB then
     * MSB. The component default (0x60) selects big-endian data. */
    ESP_RETURN_ON_ERROR(qmi8658_write_register(&s_imu, QMI8658_CTRL1, 0x40),
                        TAG, "configure QMI8658 serial format");

    /* Reapply the proven Waveshare sensor sequence after the reset. */
    ESP_RETURN_ON_ERROR(qmi8658_enable_sensors(&s_imu, QMI8658_DISABLE_ALL),
                        TAG, "pause QMI8658 configuration");
    ESP_RETURN_ON_ERROR(qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_8G),
                        TAG, "set accelerometer range");
    ESP_RETURN_ON_ERROR(qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_500HZ),
                        TAG, "set accelerometer rate");
    ESP_RETURN_ON_ERROR(qmi8658_write_register(&s_imu, QMI8658_CTRL5, 0x03),
                        TAG, "configure QMI8658 filters");
    ESP_RETURN_ON_ERROR(qmi8658_enable_sensors(&s_imu, QMI8658_ENABLE_ACCEL),
                        TAG, "enable accelerometer");
    qmi8658_set_accel_unit_mps2(&s_imu, true);
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t ctrl1 = 0;
    uint8_t ctrl2 = 0;
    uint8_t ctrl5 = 0;
    uint8_t ctrl7 = 0;
    uint8_t status0 = 0;
    ESP_RETURN_ON_ERROR(qmi8658_read_register(&s_imu, QMI8658_CTRL1, &ctrl1, 1),
                        TAG, "verify QMI8658 CTRL1");
    ESP_RETURN_ON_ERROR(qmi8658_read_register(&s_imu, QMI8658_CTRL2, &ctrl2, 1),
                        TAG, "verify QMI8658 CTRL2");
    ESP_RETURN_ON_ERROR(qmi8658_read_register(&s_imu, QMI8658_CTRL5, &ctrl5, 1),
                        TAG, "verify QMI8658 CTRL5");
    ESP_RETURN_ON_ERROR(qmi8658_read_register(&s_imu, QMI8658_CTRL7, &ctrl7, 1),
                        TAG, "verify QMI8658 CTRL7");
    ESP_RETURN_ON_ERROR(qmi8658_read_register(&s_imu, QMI8658_STATUS0, &status0, 1),
                        TAG, "verify QMI8658 STATUS0");
    ESP_LOGI(TAG, "accelerometer configured reset=0x%02x ctrl1=0x%02x ctrl2=0x%02x ctrl5=0x%02x ctrl7=0x%02x status0=0x%02x",
             reset_status, ctrl1, ctrl2, ctrl5, ctrl7, status0);
    s_callback = callback;
    s_enabled_callback = enabled_callback;
    s_sensitivity = sensitivity;

    BaseType_t created = xTaskCreate(motion_task, "pet_motion", 3584, NULL, 3, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "create motion task");
    ESP_LOGI(TAG, "shake and tilt gestures enabled at 40 Hz shakeSensitivity=%u",
             sensitivity);
    return ESP_OK;
}

void pet_motion_set_sensitivity(uint8_t sensitivity)
{
    if (sensitivity <= 100) s_sensitivity = sensitivity;
}
