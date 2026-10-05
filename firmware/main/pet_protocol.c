#include "pet_protocol.h"
#include <string.h>

uint16_t pet_protocol_read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

uint32_t pet_protocol_read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

void pet_protocol_write_u16_le(uint8_t *data, uint16_t value)
{
    data[0] = value & 0xff;
    data[1] = (value >> 8) & 0xff;
}

void pet_protocol_write_u32_le(uint8_t *data, uint32_t value)
{
    data[0] = value & 0xff;
    data[1] = (value >> 8) & 0xff;
    data[2] = (value >> 16) & 0xff;
    data[3] = (value >> 24) & 0xff;
}

size_t pet_protocol_encode_audio(uint8_t *target, size_t capacity, uint8_t kind,
                                 uint32_t stream_id, uint32_t sequence,
                                 const int16_t *pcm, size_t samples)
{
    const size_t pcm_bytes = samples * sizeof(int16_t);
    const size_t total = PET_AUDIO_HEADER_BYTES + pcm_bytes;
    if (!target || !pcm || !samples || capacity < total) return 0;
    target[0] = PET_PROTOCOL_VERSION;
    target[1] = kind;
    pet_protocol_write_u16_le(target + 2, 0);
    pet_protocol_write_u32_le(target + 4, stream_id);
    pet_protocol_write_u32_le(target + 8, sequence);
    memcpy(target + PET_AUDIO_HEADER_BYTES, pcm, pcm_bytes);
    return total;
}

esp_err_t pet_protocol_decode_audio(const uint8_t *data, size_t length, pet_audio_frame_t *frame)
{
    if (!data || !frame || length <= PET_AUDIO_HEADER_BYTES ||
        ((length - PET_AUDIO_HEADER_BYTES) & 1)) return ESP_ERR_INVALID_SIZE;
    if (data[0] != PET_PROTOCOL_VERSION) return ESP_ERR_NOT_SUPPORTED;
    if (data[1] != PET_AUDIO_KIND_MICROPHONE && data[1] != PET_AUDIO_KIND_SPEAKER) return ESP_ERR_INVALID_ARG;
    frame->version = data[0];
    frame->kind = data[1];
    frame->flags = pet_protocol_read_u16_le(data + 2);
    frame->stream_id = pet_protocol_read_u32_le(data + 4);
    frame->sequence = pet_protocol_read_u32_le(data + 8);
    frame->pcm = data + PET_AUDIO_HEADER_BYTES;
    frame->pcm_length = length - PET_AUDIO_HEADER_BYTES;
    return ESP_OK;
}
