#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "pet_ai.h"
#include "pet_speech_mouth.h"

typedef esp_err_t (*pet_audio_capture_chunk_cb_t)(uint32_t stream_id, uint32_t sequence,
                                                   const int16_t *pcm, size_t samples);
typedef void (*pet_audio_capture_done_cb_t)(uint32_t stream_id, uint32_t final_sequence,
                                            uint32_t duration_ms, esp_err_t result);
typedef void (*pet_audio_playback_done_cb_t)(uint32_t stream_id);

esp_err_t pet_audio_init(pet_audio_capture_chunk_cb_t chunk_callback,
                         pet_audio_capture_done_cb_t done_callback,
                         pet_audio_playback_done_cb_t playback_done_callback);
esp_err_t pet_audio_capture_start(uint32_t stream_id);
void pet_audio_capture_stop(void);
bool pet_audio_is_capturing(void);
esp_err_t pet_audio_set_capture_timeout(uint16_t seconds);
uint16_t pet_audio_get_capture_timeout(void);

esp_err_t pet_audio_playback_start(uint32_t stream_id, uint32_t sample_rate,
                                   pet_realtime_boost_t boost,
                                   pet_speech_mouth_mode_t mouth_mode);
/* Queues one frame without waiting: ESP_ERR_NO_MEM when there is no room. The
 * mouth follows only what the speaker plays, so a refused frame never moves
 * it and later frames keep their timing. */
esp_err_t pet_audio_playback_enqueue(uint32_t stream_id, uint32_t sequence,
                                     const uint8_t *pcm, size_t length);
/* Ends the reply after its queued frames. The playback done callback runs
 * once the last audio has left the DMA and the codec (at least 300 ms after
 * the final frame is written); the speaker stays open on silence for
 * CONFIG_PET_PLAYBACK_TAIL_MS so nothing is cut. The microphone, a cue, a new
 * reply or a cancel ends that tail at once. */
void pet_audio_playback_finish(uint32_t stream_id);
/* Stops the reply and closes the speaker now; no done callback follows. */
void pet_audio_playback_cancel(void);
/* True from playback start until the finished reply has drained. */
bool pet_audio_is_playing(void);
/* False only when streamed playback is close to exhausting its bounded queue
 * capacity; renderers may shed decorative work but must preserve speech. */
bool pet_audio_playback_queue_healthy(void);
/* Play an embedded cue in 20 ms chunks at the effects volume. The cue stops
 * with ESP_ERR_NOT_FINISHED before the next chunk once `keep_playing` (may be
 * NULL) returns false, the microphone is requested or speech is starting. */
esp_err_t pet_audio_play_local(const uint8_t *pcm, size_t length,
                               bool (*keep_playing)(void *context), void *context);
void pet_audio_set_volume(uint8_t volume);
uint8_t pet_audio_get_volume(void);
/* Mouth timing (pet_speech_mouth.h), clamped to -300..300 ms. The mouth
 * already waits for its slice to be heard; this moves it later (positive) or
 * earlier. Values computed from then on use it. */
void pet_audio_set_speech_mouth_offset(int16_t offset_ms);
int16_t pet_audio_get_speech_mouth_offset(void);
