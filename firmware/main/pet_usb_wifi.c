#include "pet_usb_wifi.h"
#include "pet_control_wire.h"
#include "pet_enrollment.h"
#include "cJSON.h"
#include <string.h>

static const char *string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool text_valid(const char *value, size_t maximum, bool empty)
{
    if (!value || (!empty && !*value) || strlen(value) > maximum) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
    {
        if (*p < 32 || *p == 127) return false;
    }
    return true;
}

static bool hex_pattern(const char *value, size_t length, char separator)
{
    if (!value || strlen(value) != length) return false;
    for (size_t i = 0; i < length; ++i)
    {
        bool gap = separator == ':' ? i % 3 == 2 : i == 8 || i == 13 || i == 18 || i == 23;
        if (gap ? value[i] != separator : !strchr("0123456789abcdef", value[i])) return false;
    }
    return true;
}

static bool parse(const cJSON *root, pet_usb_wifi_request_t *out)
{
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    const char *id = string(root, "requestId"), *action = string(root, "action");
    if (!cJSON_IsObject(root) || !cJSON_IsNumber(version) || version->valuedouble != 1 ||
        !hex_pattern(id, 36, '-') || !action) return false;
    strcpy(out->request_id, id);
    if (!strcmp(action, "info"))
    {
        out->action = PET_USB_WIFI_INFO;
        return cJSON_GetArraySize(root) == 3;
    }
    const char *mac = string(root, "mac"), *ssid = string(root, "ssid");
    if (!hex_pattern(mac, 17, ':') || !text_valid(ssid, 32, false)) return false;
    strcpy(out->mac, mac);
    strcpy(out->ssid, ssid);
    if (!strcmp(action, "saved"))
    {
        out->action = PET_USB_WIFI_SAVED;
        return cJSON_GetArraySize(root) == 5;
    }
    const char *password = string(root, "password");
    if (strcmp(action, "save") || cJSON_GetArraySize(root) != 6 || !text_valid(password, 64, true)) return false;
    size_t length = strlen(password);
    if (length && length < 8) return false;
    if (length == 64 && strspn(password, "0123456789abcdefABCDEF") != 64) return false;
    out->action = PET_USB_WIFI_SAVE;
    strcpy(out->password, password);
    return true;
}

static void clear_strings(cJSON *node)
{
    for (cJSON *child = node ? node->child : NULL; child; child = child->next) clear_strings(child);
    if (node && node->valuestring) pet_enrollment_clear(node->valuestring, strlen(node->valuestring));
}

void pet_usb_wifi_feed(pet_usb_wifi_t *state, const uint8_t *bytes, size_t length,
                       pet_usb_wifi_handler_t handler, void *context)
{
    if (!state || !bytes || !handler) return;
    for (size_t i = 0; i < length; ++i)
    {
        uint8_t byte = bytes[i];
        if (byte == '\n' || byte == '\r')
        {
            const size_t prefix = sizeof(PET_USB_WIFI_PREFIX) - 1;
            if (!state->overflow && state->length > prefix &&
                !memcmp(state->frame, PET_USB_WIFI_PREFIX, prefix))
            {
                cJSON *root = pet_control_json(state->frame + prefix, state->length - prefix,
                                               PET_USB_WIFI_FRAME_BYTES);
                pet_usb_wifi_request_t request = {0};
                if (parse(root, &request)) handler(&request, context);
                pet_enrollment_clear(&request, sizeof(request));
                clear_strings(root);
                cJSON_Delete(root);
            }
            pet_enrollment_clear(state, sizeof(*state));
        }
        else if (!state->overflow)
        {
            if (state->length == sizeof(state->frame))
            {
                pet_enrollment_clear(state->frame, sizeof(state->frame));
                state->length = 0;
                state->overflow = true;
            }
            else state->frame[state->length++] = (char)byte;
        }
    }
}
