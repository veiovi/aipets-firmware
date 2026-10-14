#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pet_control_http.h"
#include "pet_session_wire.h"

/* The sound bank (sound-bank-v1): the story's stingers, game cues and loops,
 * kept in the Pocket's dedicated 2 MiB pet_sound partition. The gateway names it in sound.bank;
 * the control task downloads it there between turns, checks it and records it
 * in NVS, and each sound is copied out of it into RAM to play
 * (pet_atmosphere.h).
 *
 * SBK1, little-endian: a 16-byte header ("SBK1", u16 version 1, u16 count
 * 1-128, u32 dataOffset, u32 dataBytes), then `count` 36-byte entries sorted
 * by id (char id[24] NUL-padded, u32 offset from dataOffset, u32 bytes, u16
 * rate 12000 or 16000, u8 kind, u8 0), then each sound's whole 256-byte IMA
 * ADPCM blocks (pet_ima_adpcm.h). */
#define PET_SOUND_BANK_SOUNDS 128u
#define PET_SOUND_BANK_HEADER_BYTES 16u
#define PET_SOUND_BANK_ENTRY_BYTES 36u

typedef enum { PET_SOUND_ONE_SHOT, PET_SOUND_LOOP } pet_sound_kind_t;
typedef struct {
    char id[PET_SOUND_ID_BYTES + 1];
    uint32_t offset; /* From the bank's first byte. */
    uint32_t bytes;  /* Whole 256-byte blocks. */
    uint16_t rate;
    uint8_t kind;    /* pet_sound_kind_t */
} pet_sound_t;

/* The header and entries at `head` (`length` bytes of a bank of
 * `bank_bytes`), read into `sounds`: their count, or 0 when any of it is
 * malformed (magic, version, count, an id or the order of ids, a rate, a kind,
 * a sound's size or one outside the data). */
unsigned pet_sound_bank_parse(const uint8_t *head, size_t length, uint32_t bank_bytes,
                              pet_sound_t sounds[PET_SOUND_BANK_SOUNDS]);

/* Control task, once at start, after the v5 store opens. The old layout's
 * NVS bank record is cleared; the current record binds the bank address. */
void pet_sound_bank_open(bool usable);
/* Network task: the bank sound.bank names. */
void pet_sound_bank_want(const char *sha256, uint32_t bytes);
/* Control task, between turns, while no pet or story event is being downloaded: one 64 KiB range of the bank named, with
 * `buffer` (at least 64 KiB) as scratch, written in pet_sound and read
 * back. A new bank stops the old one playing and erases its NVS record before
 * its first byte changes. Once all of it is written, the SHA-256 of what was
 * read back must be the one named: it is then recorded and plays; otherwise it
 * is refused until the next boot. A failed range is tried again after the
 * server's delay, three times, and again whenever sound.bank names the bank.
 * True when it worked on a download. */
bool pet_sound_bank_step(const pet_control_http_t *http, uint8_t *buffer, size_t capacity, uint64_t now_ms);
/* Any task: index of sound id/kind or -1, and its 24-bit generation when requested.
 * The short table lock never covers flash I/O. */
int pet_sound_bank_find(const char *id, pet_sound_kind_t kind, uint32_t *generation);
/* Nonblocking revision for discarding a prepared copy after bank replacement. */
uint32_t pet_sound_bank_generation(void);
/* Media/control task only: sound `index`'s blocks copied into `buffer` through the flash
 * driver, never mapped. False when the bank no longer has it or it is larger
 * than `capacity`. */
bool pet_sound_bank_copy(int index, uint8_t *buffer, size_t capacity, size_t *bytes, uint32_t *rate);
