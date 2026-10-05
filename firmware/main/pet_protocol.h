#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define PET_PROTOCOL_VERSION 1
#define PET_AUDIO_HEADER_BYTES 12
#define PET_AUDIO_KIND_MICROPHONE 1
#define PET_AUDIO_KIND_SPEAKER 2
#define PET_PCM_CHUNK_BYTES 640

typedef struct {
    uint8_t version;
    uint8_t kind;
    uint16_t flags;
    uint32_t stream_id;
    uint32_t sequence;
    const uint8_t *pcm;
    size_t pcm_length;
} pet_audio_frame_t;

size_t pet_protocol_encode_audio(uint8_t *target, size_t capacity, uint8_t kind,
                                 uint32_t stream_id, uint32_t sequence,
                                 const int16_t *pcm, size_t samples);
esp_err_t pet_protocol_decode_audio(const uint8_t *data, size_t length, pet_audio_frame_t *frame);
uint16_t pet_protocol_read_u16_le(const uint8_t *data);
uint32_t pet_protocol_read_u32_le(const uint8_t *data);
void pet_protocol_write_u16_le(uint8_t *data, uint16_t value);
void pet_protocol_write_u32_le(uint8_t *data, uint32_t value);
