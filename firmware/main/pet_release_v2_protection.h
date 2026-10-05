#pragma once
#include "pet_release_v2.h"
#include "pet_replace_journal.h"
#include "pet_firmware_wire.h"

/* Read-only derivation of firmware rollback requirements. physical/hash come
 * from the validated physical partition reader, account from authenticated
 * pet-control context. false means UNKNOWN, never an empty pet partition.
 * A sole metadata record cannot prove both old and new releases during
 * REQUESTED/FENCED, so those phases intentionally stay unknown and fenced.
 * Success here protects compatibility only; rendering/activation still needs
 * full pack-byte validation and the exact cloud acknowledgement. */
bool pet_release_v2_protection(const pet_replace_journal_t *journal,
                               const pet_flash_layout_t *physical,const char *partition_sha256,
                               const void *record,size_t record_bytes,const char *expected_account,
                               const pet_pack_trust_key_t *keys,size_t key_count,
                               pet_firmware_pack_identity_t *active,pet_firmware_pack_identity_t *interrupted);
