#pragma once
/* Records each line so tests can assert what the firmware logs. */
void pet_test_log(char level, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
#define ESP_LOGI(tag, ...) pet_test_log('I', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) pet_test_log('W', tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) pet_test_log('E', tag, __VA_ARGS__)
