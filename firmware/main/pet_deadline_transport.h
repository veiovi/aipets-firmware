#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_transport.h"
/* Caller owns the returned transport, destroys AFTER HTTP client cleanup.
 * Every underlying TLS read/write/poll consumes the same absolute budget. */
esp_transport_handle_t pet_deadline_transport_create(int64_t deadline_us);
/* Before each further request on a kept connection: a new absolute budget,
 * and the response byte count restarts. Only for transports created above. */
void pet_deadline_transport_arm(esp_transport_handle_t transport,int64_t deadline_us);
/* Response bytes read since creation or the last arm: 0 means none arrived. */
size_t pet_deadline_transport_received(esp_transport_handle_t transport);
