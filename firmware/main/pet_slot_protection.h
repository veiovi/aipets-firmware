#pragma once
#include "pet_release_v2.h"
#include "pet_slot_store.h"

/* Firmware rollback protection of the Pocket Terminal's three-pet store: the
 * single-pet rule (pet_release_v2_protection.h) for every slot. Each occupied
 * slot proves its pack with the signed release record its cloud installation
 * wrote, for the authenticated account and the slot's exact pack (the
 * installation's target while it is written):
 * - active: the saved selection, or the bound pet when none is saved;
 * - interrupted: the slot an installation is writing;
 * - installed: every other ready slot.
 * A slot without a verifiable record, such as a pet placed over USB, leaves
 * the whole protection unknown, so no firmware update can strand it. From an
 * installation's request until the installer writes the new pet's record, at
 * the start of its download, it stays unknown too. The partition table is
 * read back and must be the one the store opened. `record` is the caller's
 * 8 KiB scratch. Returns out->known; unknown leaves `out` empty. */
bool pet_slot_protection(pet_slot_store_t *store,void *record,const char *expected_account,
                         const pet_pack_trust_key_t *keys,size_t key_count,pet_firmware_protection_t *out);
