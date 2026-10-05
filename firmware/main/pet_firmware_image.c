#include "pet_firmware_image.h"
#include <string.h>
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_image_format.h"
#include "esp_secure_boot.h"
#include "mbedtls/sha256.h"
#include "pet_flash_layout_esp.h"

static bool security_supported(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    return !esp_flash_encryption_enabled()&&!esp_secure_boot_enabled();
#else
    return false; /* Never silently ship a dual-slot updater without rollback. */
#endif
}
static bool application(const esp_partition_t *p,uint32_t app_size)
{
    return p&&p->type==ESP_PARTITION_TYPE_APP&&!p->encrypted&&!p->readonly&&p->size==app_size&&
        ((p->address==0x30000&&p->subtype==ESP_PARTITION_SUBTYPE_APP_OTA_0)||
         (p->address==0x30000+app_size&&p->subtype==ESP_PARTITION_SUBTYPE_APP_OTA_1));
}
bool pet_firmware_image_bootloader_matches(const pet_firmware_bootloader_t *expected)
{
    if(!security_supported()||!expected||expected->bytes<4096||expected->bytes>0x8000||!memchr(expected->sha256,0,sizeof(expected->sha256))||
       strlen(expected->sha256)!=64||strspn(expected->sha256,"0123456789abcdef")!=64)return false;
    uint8_t buffer[4096],digest[32];mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);
    bool ok=mbedtls_sha256_starts(&sha,0)==0;
    for(uint32_t at=0;ok&&at<expected->bytes;){size_t n=expected->bytes-at;if(n>sizeof(buffer))n=sizeof(buffer);
        ok=esp_flash_read(esp_flash_default_chip,buffer,at,n)==ESP_OK&&mbedtls_sha256_update(&sha,buffer,n)==0;at+=(uint32_t)n;
    }
    if(ok)ok=mbedtls_sha256_finish(&sha,digest)==0;
    mbedtls_sha256_free(&sha);if(!ok)return false;
    const char *digits="0123456789abcdef";char hex[65];
    for(unsigned i=0;i<32;++i){hex[i*2]=digits[digest[i]>>4];hex[i*2+1]=digits[digest[i]&15];}hex[64]=0;
    return !strcmp(hex,expected->sha256);
}
static bool hash_image(const esp_partition_t *p,char hex[65],uint32_t *bytes,const char *version)
{
    if(!p||!hex||!bytes||p->encrypted||p->readonly||p->type!=ESP_PARTITION_TYPE_APP)return false;
    esp_image_metadata_t metadata={0};
    const esp_partition_pos_t position={.offset=p->address,.size=p->size};
    esp_app_desc_t description;
    if(esp_image_verify(ESP_IMAGE_VERIFY_SILENT,&position,&metadata)!=ESP_OK||!metadata.image.hash_appended||
       !metadata.image_len||metadata.image_len>p->size||
       esp_ota_get_partition_description(p,&description)!=ESP_OK||
       !memchr(description.project_name,0,sizeof(description.project_name))||strcmp(description.project_name,"aipet_firmware")||
       !memchr(description.version,0,sizeof(description.version))||(version&&strcmp(description.version,version)))return false;
    uint8_t buffer[4096],digest[32];mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);
    bool ok=mbedtls_sha256_starts(&sha,0)==0;
    for(uint32_t at=0;ok&&at<metadata.image_len;){size_t n=metadata.image_len-at;if(n>sizeof(buffer))n=sizeof(buffer);
        ok=esp_partition_read(p,at,buffer,n)==ESP_OK&&mbedtls_sha256_update(&sha,buffer,n)==0;at+=(uint32_t)n;
    }
    if(ok)ok=mbedtls_sha256_finish(&sha,digest)==0;
    mbedtls_sha256_free(&sha);if(!ok)return false;
    const char *digits="0123456789abcdef";for(unsigned i=0;i<32;++i){hex[i*2]=digits[digest[i]>>4];hex[i*2+1]=digits[digest[i]&15];}
    hex[64]=0;*bytes=metadata.image_len;return true;
}
/* The running application cannot change before the next boot: verifying and
 * hashing it (twice over its ~2 MB) happens once, not on every updater step. */
bool pet_firmware_image_running(char sha[65],uint32_t *bytes)
{
    static char hex[65];static uint32_t length;
    if(!sha||!bytes)return false;
    if(!hex[0]&&(!security_supported()||!hash_image(esp_ota_get_running_partition(),hex,&length,NULL))){hex[0]=0;return false;}
    memcpy(sha,hex,sizeof(hex));*bytes=length;return true;
}
bool pet_firmware_image_boot_state(pet_firmware_boot_state_t *out)
{
    if(!out)return false;
    memset(out,0,sizeof(*out));out->selection.active=-1;
    pet_firmware_boot_state_t s={0};uint8_t raw[2][PET_OTA_RECORD_BYTES];
    if(!security_supported()||pet_flash_layout_read(&s.layout,s.partition_sha256)!=ESP_OK||s.layout.id<PET_LAYOUT_SINGLE_2M)return false;
    const esp_partition_t *running=esp_ota_get_running_partition();
    if(!application(running,s.layout.app_slot_bytes))return false;
    /* Both sectors are fixed system regions in every recognized v2 layout. */
    if(esp_flash_read(esp_flash_default_chip,raw[0],0x10000,sizeof(raw[0]))!=ESP_OK||
       esp_flash_read(esp_flash_default_chip,raw[1],0x11000,sizeof(raw[1]))!=ESP_OK||
       !pet_ota_selection_parse(raw,&s.selection))return false;
    s.running_offset=running->address;s.running_slot=running->address==0x30000?0u:1u;
    *out=s;return true;
}
bool pet_firmware_image_confirm(uint32_t expected_offset,const char *expected_sha,const pet_firmware_bootloader_t *bootloader)
{
    if(!expected_sha||strlen(expected_sha)!=64||strspn(expected_sha,"0123456789abcdef")!=64||
       !pet_firmware_image_bootloader_matches(bootloader))return false;
    pet_firmware_boot_state_t before,after;char sha[65];uint32_t bytes;
    if(!pet_firmware_image_boot_state(&before)||before.running_offset!=expected_offset||
       !hash_image(esp_ota_get_running_partition(),sha,&bytes,NULL)||strcmp(sha,expected_sha))return false;
    if(pet_ota_selection_is(&before.selection,before.running_slot,PET_OTA_STATE_VALID))return true;
    if(!pet_ota_selection_is(&before.selection,before.running_slot,PET_OTA_STATE_PENDING))return false;
    uint32_t sequence=before.selection.records[before.selection.active].sequence;
    if(esp_ota_mark_app_valid_cancel_rollback()!=ESP_OK||!pet_firmware_image_boot_state(&after))return false;
    return after.running_offset==expected_offset&&!strcmp(before.partition_sha256,after.partition_sha256)&&
        pet_ota_selection_is(&after.selection,after.running_slot,PET_OTA_STATE_VALID)&&
        after.selection.records[after.selection.active].sequence==sequence;
}
static bool release_matches(const pet_firmware_receipt_t *r,const pet_firmware_release_t *v)
{
    return v&&pet_firmware_receipt_valid(r)&&r->phase!=PET_FW_EMPTY&&
        !strcmp(r->release_id,v->release_id)&&!strcmp(r->target_sha256,v->sha256)&&!strcmp(r->firmware_version,v->version)&&
        r->image_bytes==v->bytes&&r->firmware_epoch==v->requirements.firmware_epoch&&r->layout_id==v->requirements.layout.id&&
        !strcmp(r->partition_sha256,v->requirements.partition_sha256);
}
static const esp_partition_t *target(pet_firmware_receipt_store_t *store,const pet_firmware_release_t *release,
                                     const pet_firmware_bootloader_t *bootloader,const pet_firmware_protection_t *protection,
                                     pet_firmware_boot_state_t *boot_state)
{
    if(!security_supported()||!store||!store->loaded||!release_matches(&store->value,release)||!pet_firmware_image_bootloader_matches(bootloader))return NULL;
    if(!pet_firmware_image_boot_state(boot_state)||
       !pet_firmware_release_compatible(release,&boot_state->layout,boot_state->partition_sha256,bootloader,protection))return NULL;
    const esp_partition_t *running=esp_ota_get_running_partition(),*next=esp_ota_get_next_update_partition(NULL);
    if(!application(running,boot_state->layout.app_slot_bytes)||!application(next,boot_state->layout.app_slot_bytes)||running->address==next->address||
       next->address!=store->value.target_offset)return NULL;
    char running_sha[65];uint32_t bytes;
    if(!hash_image(running,running_sha,&bytes,NULL)||strcmp(running_sha,store->value.previous_sha256))return NULL;
    return next;
}
static bool bound(const pet_firmware_image_t *image)
{
    if(!image||!image->receipt||!image->receipt->loaded)return false;
    const pet_firmware_receipt_t *r=&image->receipt->value;
    return pet_firmware_receipt_valid(r)&&r->phase==PET_FW_DOWNLOADING&&r->acknowledged_sequence&&
        !strcmp(image->operation_id,r->operation_id)&&!strcmp(image->sha256,r->target_sha256)&&
        image->image_bytes==r->image_bytes&&image->partition&&image->partition->address==r->target_offset;
}
bool pet_firmware_image_abort(pet_firmware_image_t *image)
{
    if(!image)return false;
    if(!image->active)return true;
    esp_err_t result=esp_ota_abort(image->handle);
    if(result!=ESP_OK&&result!=ESP_ERR_NOT_FOUND)return false;
    image->active=false;image->handle=0;return true;
}
bool pet_firmware_image_begin(pet_firmware_image_t *image,pet_firmware_receipt_store_t *store,
                              const pet_firmware_release_t *release,const pet_firmware_bootloader_t *bootloader,
                              const pet_firmware_protection_t *protection)
{
    if(!image||image->active||!store||!store->loaded||store->value.phase!=PET_FW_DOWNLOADING||!store->value.acknowledged_sequence)return false;
    pet_firmware_boot_state_t boot;
    const esp_partition_t *next=target(store,release,bootloader,protection,&boot);
    if(!next||!pet_ota_selection_is(&boot.selection,boot.running_slot,PET_OTA_STATE_VALID)||
       boot.selection.records[boot.selection.active].sequence>=PET_OTA_WRITE_SEQUENCE_MAX)return false;
    memset(image,0,sizeof(*image));image->partition=next;image->receipt=store;image->image_bytes=release->bytes;
    strcpy(image->operation_id,store->value.operation_id);strcpy(image->sha256,release->sha256);strcpy(image->version,release->version);
    esp_err_t error=esp_ota_begin(next,image->image_bytes,&image->handle);
    /* IDF can allocate its handle before an erase failure. Do not leak that
     * writer or declare the operation quiescent until abort is confirmed. */
    image->active=image->handle!=0;
    if(error!=ESP_OK){pet_firmware_image_abort(image);return false;}
    return image->active;
}
bool pet_firmware_image_write(pet_firmware_image_t *image,uint32_t offset,const void *data,size_t bytes)
{
    if(!image||!image->active||!bound(image)||!data||!bytes||bytes>0x10000||offset!=image->written||
       offset>=image->image_bytes||bytes>image->image_bytes-offset)return false;
    bool ok=esp_ota_write(image->handle,data,bytes)==ESP_OK;uint8_t actual[4096];
    for(size_t at=0;ok&&at<bytes;){size_t n=bytes-at;if(n>sizeof(actual))n=sizeof(actual);
        ok=esp_partition_read(image->partition,offset+at,actual,n)==ESP_OK&&!memcmp(actual,(const uint8_t *)data+at,n);at+=n;
    }
    if(!ok){pet_firmware_image_abort(image);return false;}
    image->written+=(uint32_t)bytes;return true;
}
bool pet_firmware_image_finish(pet_firmware_image_t *image)
{
    if(!image||!image->active||!bound(image)||image->written!=image->image_bytes||
       image->receipt->value.downloaded_bytes!=image->image_bytes)return false;
    char sha[65];uint32_t bytes;
    if(!hash_image(image->partition,sha,&bytes,image->version)||bytes!=image->image_bytes||strcmp(sha,image->sha256)){
        pet_firmware_image_abort(image);return false;
    }
    esp_err_t result=esp_ota_end(image->handle);image->active=false;image->handle=0;return result==ESP_OK;
}
static bool selected_with_fallback(const pet_firmware_boot_state_t *boot,uint32_t previous_sequence)
{
    if(!pet_ota_selection_is(&boot->selection,1-boot->running_slot,PET_OTA_STATE_NEW)||
       !previous_sequence||previous_sequence>=PET_OTA_WRITE_SEQUENCE_MAX)return false;
    int prior=pet_ota_selection_newest(&boot->selection,boot->running_slot);
    return prior>=0&&boot->selection.records[prior].state==PET_OTA_STATE_VALID&&
        boot->selection.records[prior].sequence==previous_sequence&&
        boot->selection.records[boot->selection.active].sequence==previous_sequence+1;
}
bool pet_firmware_image_select(pet_firmware_receipt_store_t *store,const pet_firmware_release_t *release,const char *current_boot,
                               const pet_firmware_bootloader_t *bootloader,const pet_firmware_protection_t *protection)
{
    if(!store||!store->loaded||store->value.phase!=PET_FW_REBOOTING||
       store->value.acknowledged_sequence!=store->value.sequence||!current_boot||strcmp(current_boot,store->value.report_boot_id))return false;
    pet_firmware_boot_state_t boot;
    const esp_partition_t *next=target(store,release,bootloader,protection,&boot);if(!next)return false;
    char sha[65];uint32_t bytes;
    if(!hash_image(next,sha,&bytes,release->version)||bytes!=release->bytes||strcmp(sha,release->sha256))return false;
    unsigned next_slot=1-boot.running_slot;
    int prior=pet_ota_selection_newest(&boot.selection,boot.running_slot);
    if(pet_ota_selection_is(&boot.selection,next_slot,PET_OTA_STATE_NEW))
        return prior>=0&&selected_with_fallback(&boot,boot.selection.records[prior].sequence);
    if(!pet_ota_selection_is(&boot.selection,boot.running_slot,PET_OTA_STATE_VALID))return false;
    uint32_t previous_sequence=boot.selection.records[boot.selection.active].sequence;
    if(previous_sequence>=PET_OTA_WRITE_SEQUENCE_MAX)return false;
    int newest=pet_ota_selection_newest(&boot.selection,next_slot);
    if(newest>=0&&(boot.selection.records[newest].state==PET_OTA_STATE_INVALID||
       boot.selection.records[newest].state==PET_OTA_STATE_ABORTED||boot.selection.records[newest].state==PET_OTA_STATE_PENDING))return false;
    if(esp_ota_set_boot_partition(next)!=ESP_OK)return false;
    pet_firmware_boot_state_t after;
    return pet_firmware_image_boot_state(&after)&&after.running_offset==boot.running_offset&&
        !strcmp(after.partition_sha256,boot.partition_sha256)&&selected_with_fallback(&after,previous_sequence);
}
