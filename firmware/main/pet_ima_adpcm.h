#pragma once
#include <stddef.h>
#include <stdint.h>

/* IMA ADPCM speech (cloud device-protocol SPEECH_IMA_ADPCM_CAPABILITY): a
 * quarter of the bytes of 16 kHz PCM, for a Pocket on a slow link. A frame
 * starts with the coder state it begins from (predictor int16 LE, step index
 * 0-88, a zero byte), then holds two samples a byte, low nibble first, so
 * each frame decodes on its own. */
#define PET_IMA_ADPCM_CAPABILITY "speech-ima-adpcm-v1"
#define PET_IMA_ADPCM_HEADER_BYTES 4

/* Decodes one frame into `pcm`. Returns the number of samples, or 0 for a
 * malformed frame or one holding more than `capacity` samples. */
size_t pet_ima_adpcm_decode(const uint8_t *frame, size_t length, int16_t *pcm, size_t capacity);
