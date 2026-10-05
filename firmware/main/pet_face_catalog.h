#pragma once

#include <stdint.h>
#include "pet_face_limits.h"

enum {
    PET_FACE_CATALOG_CAPACITY = 8,
};

typedef enum {
    PET_FACE_CATALOG_RESIDENT = 0,
} pet_face_catalog_state_t;

typedef struct {
    char id[PET_FACE_ID_MAX];
    char name[PET_FACE_NAME_MAX];
    char version[24];
    char sha256[65];
    uint32_t pack_bytes;
    uint16_t format_version;
    uint32_t payload_crc32;
    uint8_t physical_slot;
    uint8_t gender;
    uint8_t tier;
    pet_face_catalog_state_t state;
} pet_face_catalog_item_t;
