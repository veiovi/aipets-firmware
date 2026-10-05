#pragma once
#include "pet_firmware_receipt.h"
/* Single control worker owns this store. Only pet_fw_v2/state_a and state_b;
 * never erase, reinitialize NVS, or touch Wi-Fi/enrollment/settings namespaces. */
bool pet_firmware_receipt_open_nvs(pet_firmware_receipt_store_t *store);
