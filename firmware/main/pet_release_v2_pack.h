#pragma once
#include "pet_release_v2.h"
#include "frame_player.h"

/* After pet_release_v2_verify, with an immutable/detached full-file mapping.
 * Checks SHA-256 plus every resource/clip/facial patch with the shared renderer,
 * then identity (exact, or an imported release's derived face ID,
 * pet_face_id.h), resolution and codec declarations. A move pack is never
 * a pet: it only plays beside one. Workspace is the
 * bounded off-stack buffer required by fp_validate_with_workspace. Success
 * still does not replace the cloud activation acknowledgement. */
bool pet_release_v2_validate_pack(const pet_release_v2_t *release,const void *pack,size_t bytes,
                                 void *workspace,uint32_t workspace_bytes,fp_pack_info_t *info);
/* Same signed identity and both published/reviewed hashes, after a detached
 * fp_stream_open(prove=1) proved every resource and the resident budget.
 * Control task only; scratch is the unbound stream's unused RAM tail. */
bool pet_release_v2_validate_source(const pet_release_v2_t *release, const fp_pack_source_t *source,
    const fp_pack_info_t *proven, void *scratch, uint32_t scratch_bytes);
