#include "pet_replace_receipt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *request="00000000-0000-4000-8000-000000000001";
static const char *operation="00000000-0000-4000-8000-000000000002";
static const char *fence="00000000-0000-4000-8000-000000000003";
static const char *relationship="00000000-0000-4000-8000-000000000004";
static const char *sha="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *prefix="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static pet_replace_journal_t journal;
static pet_replace_operation_t cloud;
static pet_replace_report_t receipt;
static void persist(const pet_replace_t *next)
{
    uint8_t record[PET_REPLACE_RECORD_BYTES];assert(pet_replace_encode(next,record,sizeof(record)));
    memset(&journal.state,0xaa,sizeof(journal.state));assert(pet_replace_decode(record,sizeof(record),&journal.state));
    ++journal.journal.generation;
}
static void ack(void)
{
    assert(pet_replace_receipt_make(&journal,&cloud,&receipt));
    assert(pet_replace_receipt_classify(&cloud,&receipt)==PET_RECEIPT_ADVANCE);
    assert(!pet_replace_receipt_acknowledged(&cloud,&receipt));
    cloud.phase=receipt.phase;cloud.report=receipt;cloud.has_report=true;
    cloud.flash_reserved=cloud.phase!=PET_CLOUD_RECOVERY&&cloud.phase!=PET_CLOUD_INSTALLED;
    assert(pet_replace_receipt_acknowledged(&cloud,&receipt));
    assert(pet_replace_receipt_classify(&cloud,&receipt)==PET_RECEIPT_DUPLICATE);
    pet_replace_report_t wrong=receipt;wrong.prefix_sha256[0]=wrong.prefix_sha256[0]=='a'?'b':'a';
    assert(!pet_replace_receipt_acknowledged(&cloud,&wrong));
    assert(pet_replace_receipt_classify(&cloud,&wrong)==PET_RECEIPT_REJECT);
    wrong=receipt;--wrong.generation;assert(pet_replace_receipt_classify(&cloud,&wrong)==PET_RECEIPT_REJECT);
    wrong=receipt;wrong.operation_id[35]='9';assert(pet_replace_receipt_classify(&cloud,&wrong)==PET_RECEIPT_REJECT);
    wrong=receipt;wrong.fence_id[35]='9';assert(pet_replace_receipt_classify(&cloud,&wrong)==PET_RECEIPT_REJECT);
    wrong=receipt;wrong.pack.build_id[35]='9';assert(pet_replace_receipt_classify(&cloud,&wrong)==PET_RECEIPT_REJECT);
    wrong=receipt;++wrong.pack.bytes;assert(pet_replace_receipt_classify(&cloud,&wrong)==PET_RECEIPT_REJECT);
}
static void no_receipt(void)
{
    memset(&receipt,0xaa,sizeof(receipt));assert(!pet_replace_receipt_make(&journal,&cloud,&receipt));
    for(size_t i=0;i<sizeof(receipt);++i)assert(!((const unsigned char *)&receipt)[i]);
}
int main(void)
{
    cloud.phase=PET_CLOUD_QUEUED;cloud.flash_reserved=true;
    strcpy(cloud.id,operation);strcpy(cloud.request_id,request);strcpy(cloud.fence_id,fence);strcpy(cloud.expected_revision,"r0");
    cloud.release.pack.bytes=0x10003;strcpy(cloud.release.pack.build_id,request);strcpy(cloud.release.pack.sha256,sha);
    journal.ready=journal.journal.loaded=true;journal.journal.generation=9007199254740993ULL;
    assert(pet_replace_empty(&journal.state,PET_REPLACE_MAX_BYTES));no_receipt();
    pet_replace_t next=journal.state;
    assert(pet_replace_request(&next,request,&cloud.release.pack,"r0"));persist(&next);no_receipt();
    assert(pet_replace_fenced(&next,operation,fence,&cloud.release.pack));persist(&next);no_receipt();
    cloud.phase=PET_CLOUD_FENCED;cloud.fence_confirmed=true;
    assert(pet_replace_invalidate(&next));persist(&next);ack();
    assert(pet_replace_begin_download(&next));persist(&next);ack();
    assert(pet_replace_progress(&next,0x10000,prefix));persist(&next);ack();
    pet_replace_report_t wrong=receipt;++wrong.generation;wrong.downloaded_bytes=0;wrong.prefix_sha256[0]=0;
    assert(pet_replace_receipt_classify(&cloud,&wrong)==PET_RECEIPT_REJECT);
    wrong.phase=PET_CLOUD_RECOVERY;assert(pet_replace_receipt_classify(&cloud,&wrong)==PET_RECEIPT_REJECT);
    wrong.phase=PET_CLOUD_INVALIDATED;assert(pet_replace_receipt_classify(&cloud,&wrong)==PET_RECEIPT_ADVANCE);

    /* A lost response after a durable same-target reset still admits reset
     * during cancellation, but never another downloading report. */
    cloud.cancel_requested=true;assert(pet_replace_restart(&next,&next.target));persist(&next);ack();
    wrong=receipt;++wrong.generation;wrong.phase=PET_CLOUD_DOWNLOADING;
    assert(pet_replace_receipt_classify(&cloud,&wrong)==PET_RECEIPT_REJECT);
    assert(pet_replace_cancel(&next));persist(&next);ack();
    pet_replace_report_t recovery=receipt;
    cloud.phase=PET_CLOUD_FENCED;cloud.flash_reserved=true;cloud.cancel_requested=false;
    assert(pet_replace_receipt_classify(&cloud,&recovery)==PET_RECEIPT_DUPLICATE);
    assert(!pet_replace_receipt_acknowledged(&cloud,&recovery));
    assert(pet_replace_retry(&next,operation,fence,&cloud.release.pack));persist(&next);ack();
    assert(pet_replace_begin_download(&next));persist(&next);ack();
    assert(pet_replace_progress(&next,0x10000,prefix));persist(&next);ack();
    assert(pet_replace_progress(&next,next.target.bytes,sha));persist(&next);ack();
    assert(pet_replace_verified(&next,sha));persist(&next);ack();
    assert(pet_replace_activate(&next));persist(&next);ack();
    cloud.has_result=true;cloud.binding.assigned=true;strcpy(cloud.binding.revision,"r1");
    strcpy(cloud.binding.relationship_id,relationship);strcpy(cloud.binding.build_id,cloud.release.pack.build_id);
    strcpy(cloud.binding.sha256,sha);strcpy(cloud.config.version,"17");
    assert(pet_replace_commit(&next,operation,fence,&cloud.release.pack,"r1",relationship,"17"));persist(&next);
    assert(!journal.state.operation_id[0]&&!journal.state.fence_id[0]&&!journal.state.target.bytes);
    assert(pet_replace_receipt_make(&journal,&cloud,&receipt)&&receipt.phase==PET_CLOUD_INSTALLED);
    assert(pet_replace_receipt_classify(&cloud,&receipt)==PET_RECEIPT_ADVANCE);

    /* Power loss after ACTIVE and before installed ACK is correlated by every
     * immutable activation field; same pack alone is insufficient. */
    pet_replace_operation_t original=cloud;
    cloud.binding.revision[1]='2';no_receipt();cloud=original;
    cloud.binding.relationship_id[35]='9';no_receipt();cloud=original;
    cloud.config.version[0]='2';no_receipt();cloud=original;
    cloud.binding.sha256[0]='b';no_receipt();cloud=original;
    cloud.release.pack.sha256[0]='b';no_receipt();cloud=original;
    ++cloud.release.pack.bytes;no_receipt();cloud=original;
    cloud.phase=PET_CLOUD_VERIFIED;no_receipt();cloud=original;
    cloud.has_result=false;no_receipt();cloud=original;
    cloud.supersedes=true;cloud.previous_generation=journal.journal.generation;no_receipt();cloud=original;
    journal.ready=false;no_receipt();journal.ready=true;
    journal.journal.loaded=false;no_receipt();journal.journal.loaded=true;
    uint64_t saved_generation=journal.journal.generation;journal.journal.generation=0;no_receipt();journal.journal.generation=saved_generation;
    ack();
    journal.journal.generation=UINT64_MAX;
    assert(pet_replace_receipt_make(&journal,&cloud,&receipt)&&receipt.generation==UINT64_MAX);
    /* Terminal cloud operation cannot accept a fabricated newer ACTIVE report. */
    assert(pet_replace_receipt_classify(&cloud,&receipt)==PET_RECEIPT_REJECT);
    puts("pet replacement receipts: durable uint64 identity, recovery retries, cancellation, exact ACK and lost activation ACK passed");
}
