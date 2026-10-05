#pragma once
#include "pet_replace_wire.h"
#include "pet_replace_journal.h"

/* Inputs are a loaded journal and an authenticated, wire-validated operation.
 * These helpers do not write flash, verify pack bytes, or grant activation.
 * An ACTIVE journal has no operation IDs: reconnect it only through the exact
 * immutable activation result, never through the pack hash alone. */
bool pet_replace_receipt_make(const pet_replace_journal_t *journal,
                              const pet_replace_operation_t *operation,pet_replace_report_t *out);
/* True only for this current cloud state and this exact durable receipt.
 * A RECOVERY receipt retained by a new FENCED retry is not its current ACK. */
bool pet_replace_receipt_acknowledged(const pet_replace_operation_t *operation,
                                      const pet_replace_report_t *receipt);
typedef enum {PET_RECEIPT_REJECT,PET_RECEIPT_DUPLICATE,PET_RECEIPT_ADVANCE} pet_replace_receipt_action_t;
/* Mirrors the pinned cloud transition rules after schema validation. Receipt
 * must come from make() or the wire validator, not an arbitrary C struct.
 * DUPLICATE may reconcile a lost report response, but cannot authorize
 * repeating old side effects. Encoding revalidates the complete wire schema. */
pet_replace_receipt_action_t pet_replace_receipt_classify(const pet_replace_operation_t *operation,
                                                         const pet_replace_report_t *receipt);
