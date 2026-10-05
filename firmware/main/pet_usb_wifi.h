#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PET_USB_WIFI_PREFIX "AIPETS_WIFI "
#define PET_USB_WIFI_FRAME_BYTES 1024
typedef enum { PET_USB_WIFI_INFO, PET_USB_WIFI_SAVE, PET_USB_WIFI_SAVED } pet_usb_wifi_action_t;
typedef struct {
    pet_usb_wifi_action_t action;
    char request_id[37], mac[18], ssid[33], password[65];
} pet_usb_wifi_request_t;
typedef struct {
    char frame[PET_USB_WIFI_FRAME_BYTES];
    size_t length;
    bool overflow;
} pet_usb_wifi_t;
typedef void (*pet_usb_wifi_handler_t)(const pet_usb_wifi_request_t *request, void *context);

/* Bounded application-mode serial framing; never echoes input. The callback
 * borrows the request only until it returns, then all credential bytes clear. */
void pet_usb_wifi_feed(pet_usb_wifi_t *state, const uint8_t *bytes, size_t length,
                       pet_usb_wifi_handler_t handler, void *context);
