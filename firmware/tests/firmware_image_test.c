#include "pet_firmware_image.h"
#include "pet_flash_layout_esp.h"
#include "esp_image_format.h"
#include "esp_flash.h"
#include "esp_rom_crc.h"
#include "mbedtls/sha256.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define IMAGE_BYTES 131075u
static uint8_t running_data[1024],target_data[0x200000],payload[IMAGE_BYTES];
static esp_partition_t app[2];static unsigned boot_slot,erase_calls,write_calls,boot_calls,abort_calls;
static size_t wrote;static bool handle_active,secure,encrypted,bad_layout,bad_read,bad_write,bad_image,bad_version,bad_project,begin_error,end_error,boot_error;
static esp_ota_img_states_t states[2];static int target_state_status;static uint32_t image_length;
static pet_firmware_release_t candidate;static pet_firmware_receipt_store_t store;
static pet_firmware_image_t writer;static const pet_firmware_protection_t empty={.known=true},unknown={.known=false};
static pet_firmware_bootloader_t bootloader;static uint8_t bootloader_bytes[0x7000];
static bool bootloader_read_error;
static uint32_t sequences[2];static bool ota_read_error,mark_error,mark_noop,mark_read_error,bad_crc[2];static unsigned mark_calls;
static bool boot_noop,boot_destroys_fallback;
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(i*8));}
static void ota_records(uint8_t raw[2][32])
{
    memset(raw,0xff,64);
    for(unsigned i=0;i<2;++i)if(sequences[i]!=UINT32_MAX){
        put32(raw[i],sequences[i]);put32(raw[i]+24,states[i]);
        put32(raw[i]+28,esp_rom_crc32_le(UINT32_MAX,raw[i],4)^(bad_crc[i]?1u:0u));
    }
}
esp_flash_t *esp_flash_default_chip=(esp_flash_t *)1;
esp_err_t esp_flash_read(esp_flash_t *chip,void *out,uint32_t address,uint32_t bytes)
{assert(chip==esp_flash_default_chip);
 if(address==0x10000||address==0x11000){assert(bytes==32);if(ota_read_error)return ESP_FAIL;uint8_t raw[2][32];ota_records(raw);memcpy(out,raw[(address-0x10000)/4096],bytes);return ESP_OK;}
 assert(address+bytes<=sizeof(bootloader_bytes));if(bootloader_read_error)return ESP_FAIL;
 memcpy(out,bootloader_bytes+address,bytes);return ESP_OK;}
static void hash(const void *bytes,size_t length,char out[65])
{uint8_t d[32];assert(!mbedtls_sha256(bytes,length,d,0));const char *hex="0123456789abcdef";for(unsigned i=0;i<32;++i){out[2*i]=hex[d[i]>>4];out[2*i+1]=hex[d[i]&15];}out[64]=0;}
bool esp_flash_encryption_enabled(void){return encrypted;}
bool esp_secure_boot_enabled(void){return secure;}
const esp_partition_t *esp_ota_get_running_partition(void){return &app[0];}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p){assert(!p);return &app[1];}
const esp_partition_t *esp_ota_get_boot_partition(void){return &app[boot_slot];}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,esp_ota_img_states_t *s)
{assert(p==&app[0]||p==&app[1]);*s=states[p==&app[1]];return p==&app[1]?target_state_status:ESP_OK;}
esp_err_t pet_flash_layout_read(pet_flash_layout_t *layout,char sha[65])
{if(bad_layout)return ESP_FAIL;assert(pet_flash_layout_known(PET_LAYOUT_SINGLE_2M,layout));memset(sha,'c',64);sha[64]=0;return ESP_OK;}
/* Compatibility itself has real signature-backed tests in test_firmware_release.
 * This fixture keeps its boundary visible without replacing any flash checks. */
bool pet_firmware_release_compatible(const pet_firmware_release_t *r,const pet_flash_layout_t *physical,
                                     const char *sha,const pet_firmware_bootloader_t *profile,const pet_firmware_protection_t *p)
{return profile&&r->requirements.bootloader.bytes==profile->bytes&&!strcmp(r->requirements.bootloader.sha256,profile->sha256)&&
 p&&p->known&&!p->active.requirements.present&&!p->interrupted.requirements.present&&!p->installed_count&&
 r->requirements.layout.id==physical->id&&!strcmp(r->requirements.partition_sha256,sha);}
esp_err_t esp_image_verify(int mode,const esp_partition_pos_t *p,esp_image_metadata_t *m)
{assert(mode==ESP_IMAGE_VERIFY_SILENT&&p->size==0x200000);if(bad_image&&p->offset==app[1].address)return ESP_FAIL;
 m->image.hash_appended=1;m->image_len=p->offset==app[0].address?sizeof(running_data):image_length;return ESP_OK;}
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p,esp_app_desc_t *d)
{memset(d,0,sizeof(*d));strcpy(d->project_name,bad_project?"another_project":"aipet_firmware");
 strcpy(d->version,p==&app[0]?"previous":bad_version?"wrong":"fixture-v2");return ESP_OK;}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *out,size_t bytes)
{assert(offset+bytes<=p->size);if(p==&app[0]){assert(offset+bytes<=sizeof(running_data));memcpy(out,running_data+offset,bytes);}
 else {assert(p==&app[1]);memcpy(out,target_data+offset,bytes);if(bad_read&&bytes)((uint8_t *)out)[0]^=1;}return ESP_OK;}
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t bytes,esp_ota_handle_t *out)
{assert(p==&app[1]&&!handle_active&&bytes==IMAGE_BYTES);++erase_calls;*out=7;handle_active=true;
 memset(target_data,0xff,sizeof(target_data));wrote=0;
 if(!begin_error){target_state_status=ESP_ERR_NOT_FOUND;sequences[1]=UINT32_MAX;}
 return begin_error?ESP_FAIL:ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t handle,const void *data,size_t bytes)
{assert(handle==7&&handle_active&&wrote+bytes<=IMAGE_BYTES);++write_calls;if(bad_write)return ESP_FAIL;memcpy(target_data+wrote,data,bytes);wrote+=bytes;return ESP_OK;}
esp_err_t esp_ota_end(esp_ota_handle_t handle)
{assert(handle==7&&handle_active);handle_active=false;return end_error?ESP_FAIL:ESP_OK;}
esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{assert(handle==7&&handle_active);++abort_calls;handle_active=false;return ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{assert(p==&app[1]&&!handle_active);++boot_calls;
 if(!boot_noop){boot_slot=1;sequences[1]=sequences[0]+1;states[1]=ESP_OTA_IMG_NEW;target_state_status=ESP_OK;}
 if(boot_destroys_fallback)sequences[0]=UINT32_MAX;return boot_error?ESP_FAIL:ESP_OK;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{++mark_calls;uint8_t raw[2][32];ota_records(raw);pet_ota_selection_t s;assert(pet_ota_selection_parse(raw,&s)&&s.active>=0);
 if(!mark_noop)states[s.active]=ESP_OTA_IMG_VALID;if(mark_read_error)ota_read_error=true;return mark_error?ESP_FAIL:ESP_OK;}
static void reset(void)
{
    assert(!handle_active);memset(&writer,0,sizeof(writer));memset(&candidate,0,sizeof(candidate));memset(&store,0,sizeof(store));
    secure=encrypted=bad_layout=bad_read=bad_write=bad_image=bad_version=bad_project=begin_error=end_error=boot_error=false;
    bootloader_read_error=false;memset(bootloader_bytes,0x5a,sizeof(bootloader_bytes));
    bootloader.bytes=sizeof(bootloader_bytes);hash(bootloader_bytes,sizeof(bootloader_bytes),bootloader.sha256);
    boot_slot=erase_calls=write_calls=boot_calls=abort_calls=0;wrote=0;states[0]=states[1]=ESP_OTA_IMG_VALID;target_state_status=ESP_OK;image_length=IMAGE_BYTES;
    sequences[0]=3;sequences[1]=2;ota_read_error=mark_error=mark_noop=mark_read_error=bad_crc[0]=bad_crc[1]=false;mark_calls=0;
    boot_noop=boot_destroys_fallback=false;
    app[0]=(esp_partition_t){.type=ESP_PARTITION_TYPE_APP,.subtype=ESP_PARTITION_SUBTYPE_APP_OTA_0,.address=0x30000,.size=0x200000};
    app[1]=(esp_partition_t){.type=ESP_PARTITION_TYPE_APP,.subtype=ESP_PARTITION_SUBTYPE_APP_OTA_1,.address=0x230000,.size=0x200000};
    for(unsigned i=0;i<IMAGE_BYTES;++i)payload[i]=(uint8_t)(i*13+17);
    for(unsigned i=0;i<sizeof(running_data);++i)running_data[i]=(uint8_t)(i*19+3);
    candidate.bytes=IMAGE_BYTES;strcpy(candidate.release_id,"00000000-0000-4000-8000-000000000002");strcpy(candidate.version,"fixture-v2");
    candidate.requirements.bootloader=bootloader;
    hash(payload,sizeof(payload),candidate.sha256);memset(candidate.requirements.partition_sha256,'c',64);candidate.requirements.partition_sha256[64]=0;
    assert(pet_flash_layout_known(PET_LAYOUT_SINGLE_2M,&candidate.requirements.layout));candidate.requirements.firmware_epoch=3;
    char previous[65];hash(running_data,sizeof(running_data),previous);
    assert(pet_firmware_receipt_begin(&store.value,&candidate,"00000000-0000-4000-8000-000000000005",previous,
        "00000000-0000-4000-8000-000000000003",app[1].address));store.loaded=true;
}
static bool begin(void){return pet_firmware_image_begin(&writer,&store,&candidate,&bootloader,&empty);}
static void transfer(void)
{
    assert(pet_firmware_receipt_ack(&store.value,store.value.sequence));assert(begin());
    for(uint32_t at=0;at<IMAGE_BYTES;){size_t n=IMAGE_BYTES-at;if(n>0x10000)n=0x10000;
        assert(pet_firmware_image_write(&writer,at,payload+at,n));at+=(uint32_t)n;
        assert(pet_firmware_receipt_progress(&store.value,at,store.value.report_boot_id));
    }
}
static bool select_image(void){return pet_firmware_image_select(&store,&candidate,store.value.report_boot_id,&bootloader,&empty);}
int main(void)
{
    reset();assert(!begin()&&!erase_calls);assert(pet_firmware_receipt_ack(&store.value,1));
    assert(!pet_firmware_image_begin(&writer,&store,&candidate,NULL,&empty)&&!erase_calls);
    bootloader_bytes[0]^=1;assert(!begin()&&!erase_calls);bootloader_bytes[0]^=1;
    bootloader_read_error=true;assert(!begin()&&!erase_calls);bootloader_read_error=false;
    secure=true;assert(!begin()&&!erase_calls);secure=false;encrypted=true;assert(!begin()&&!erase_calls);encrypted=false;
    bad_layout=true;assert(!begin()&&!erase_calls);bad_layout=false;
    states[0]=ESP_OTA_IMG_PENDING_VERIFY;assert(!begin()&&!erase_calls);states[0]=ESP_OTA_IMG_VALID;
    app[1].address=app[0].address;assert(!begin()&&!erase_calls);app[1].address=0x230000;
    app[1].encrypted=true;assert(!begin()&&!erase_calls);app[1].encrypted=false;
    sequences[1]=4;states[1]=ESP_OTA_IMG_NEW;assert(!begin()&&!erase_calls);sequences[1]=2;states[1]=ESP_OTA_IMG_VALID;
    // get_state_partition would return the old VALID copy first. Raw selection
    // must notice the newer PENDING copy for that SAME running slot.
    sequences[0]=1;sequences[1]=3;states[1]=ESP_OTA_IMG_PENDING_VERIFY;assert(!begin()&&!erase_calls);
    sequences[0]=3;sequences[1]=2;states[1]=ESP_OTA_IMG_VALID;
    sequences[0]=0xfffffffd;assert(!begin()&&!erase_calls);sequences[0]=3;
    assert(!pet_firmware_image_begin(&writer,&store,&candidate,&bootloader,&unknown)&&!erase_calls);
    begin_error=true;assert(!begin()&&erase_calls==1&&abort_calls==1&&!writer.active);begin_error=false;
    assert(begin());assert(!pet_firmware_image_write(&writer,1,payload,10));assert(!pet_firmware_image_write(&writer,0,payload,65537));
    assert(!pet_firmware_image_finish(&writer)&&!boot_calls);
    bad_read=true;assert(!pet_firmware_image_write(&writer,0,payload,65536)&&!writer.active&&!handle_active);
    reset();transfer();assert(erase_calls==1&&write_calls==3&&!boot_calls);assert(pet_firmware_image_finish(&writer)&&!writer.active);
    assert(!select_image());assert(pet_firmware_receipt_stage(&store.value,candidate.sha256));assert(pet_firmware_receipt_ack(&store.value,store.value.sequence));
    assert(pet_firmware_receipt_reboot(&store.value));assert(!select_image()&&!boot_calls);
    assert(pet_firmware_receipt_ack(&store.value,store.value.sequence));target_data[10]^=1;assert(!select_image()&&!boot_calls);target_data[10]^=1;
    assert(!pet_firmware_image_select(&store,&candidate,"00000000-0000-4000-8000-000000000099",&bootloader,&empty)&&!boot_calls);
    sequences[0]=0xfffffffd;assert(!select_image()&&!boot_calls);sequences[0]=3;
    boot_noop=true;assert(!select_image()&&boot_calls==1);boot_noop=false;boot_calls=0;
    boot_error=true;assert(!select_image()&&boot_calls==1&&boot_slot==1&&states[1]==ESP_OTA_IMG_NEW);
    boot_error=false;assert(select_image()&&boot_calls==1); // Lost selection return never re-arms NEW.
    states[1]=ESP_OTA_IMG_ABORTED;assert(!select_image()&&boot_calls==1);
    states[1]=ESP_OTA_IMG_PENDING_VERIFY;assert(!select_image()&&boot_calls==1);
    reset();transfer();assert(pet_firmware_image_finish(&writer));
    assert(pet_firmware_receipt_stage(&store.value,candidate.sha256)&&pet_firmware_receipt_ack(&store.value,store.value.sequence));
    assert(pet_firmware_receipt_reboot(&store.value)&&pet_firmware_receipt_ack(&store.value,store.value.sequence));
    boot_destroys_fallback=true;assert(!select_image()&&boot_calls==1);
    assert(!select_image()&&boot_calls==1); // no silent re-arm/restore without a valid fallback
    reset();app[0].address=0x230000;app[0].subtype=ESP_PARTITION_SUBTYPE_APP_OTA_1;
    app[1].address=0x30000;app[1].subtype=ESP_PARTITION_SUBTYPE_APP_OTA_0;store.value.target_offset=0x30000;
    sequences[0]=2;sequences[1]=1;transfer();assert(pet_firmware_image_finish(&writer));
    assert(pet_firmware_receipt_stage(&store.value,candidate.sha256)&&pet_firmware_receipt_ack(&store.value,store.value.sequence));
    assert(pet_firmware_receipt_reboot(&store.value)&&pet_firmware_receipt_ack(&store.value,store.value.sequence));
    sequences[0]=0xfffffffe;assert(!select_image()&&!boot_calls); // never allocate reserved UINT32_MAX
    sequences[0]=PET_OTA_WRITE_SEQUENCE_MAX;assert(!select_image()&&!boot_calls);
    sequences[0]=2;assert(select_image()&&boot_calls==1&&sequences[1]==3);
    for(unsigned fault=0;fault<5;++fault){reset();transfer();
        if(fault==0)bad_image=true;else if(fault==1)bad_version=true;else if(fault==2)image_length--;else if(fault==3)end_error=true;else target_data[100]^=1;
        assert(!pet_firmware_image_finish(&writer)&&!writer.active&&!handle_active&&!boot_calls);
    }
    reset();assert(pet_firmware_receipt_ack(&store.value,1));assert(begin());bad_write=true;
    assert(!pet_firmware_image_write(&writer,0,payload,65536)&&!writer.active&&!handle_active);
    reset();char sha[65];uint32_t bytes;
    bad_project=true;assert(!pet_firmware_image_running(sha,&bytes)); /* A failure is not remembered... */
    bad_project=false;assert(pet_firmware_image_running(sha,&bytes));
    assert(bytes==sizeof(running_data)&&!strcmp(sha,store.value.previous_sha256));
    /* ...the hash is: the running app cannot change before the next boot. */
    running_data[0]^=1;bad_project=true;assert(pet_firmware_image_running(sha,&bytes)&&!strcmp(sha,store.value.previous_sha256));
    reset();assert(pet_firmware_image_running(sha,&bytes));
    assert(pet_firmware_image_confirm(app[0].address,sha,&bootloader)&&!mark_calls);
    assert(!pet_firmware_image_confirm(app[1].address,sha,&bootloader)&&!mark_calls);
    states[0]=ESP_OTA_IMG_PENDING_VERIFY;
    assert(pet_firmware_image_confirm(app[0].address,sha,&bootloader)&&mark_calls==1&&states[0]==ESP_OTA_IMG_VALID);
    // A fallback-running app must never validate the other slot's active entry.
    sequences[1]=4;states[1]=ESP_OTA_IMG_PENDING_VERIFY;
    assert(!pet_firmware_image_confirm(app[0].address,sha,&bootloader)&&mark_calls==1);
    // Duplicate same-slot record: confirm the newest active copy, not the old
    // VALID one returned by IDF's first-match API.
    sequences[0]=1;sequences[1]=3;
    assert(pet_firmware_image_confirm(app[0].address,sha,&bootloader)&&mark_calls==2&&states[1]==ESP_OTA_IMG_VALID);
    states[1]=ESP_OTA_IMG_PENDING_VERIFY;mark_noop=true;
    assert(!pet_firmware_image_confirm(app[0].address,sha,&bootloader)&&mark_calls==3);mark_noop=false;
    mark_error=true;assert(!pet_firmware_image_confirm(app[0].address,sha,&bootloader)&&mark_calls==4);
    mark_error=false;assert(pet_firmware_image_confirm(app[0].address,sha,&bootloader)&&mark_calls==4); // durable VALID, lost return
    states[1]=ESP_OTA_IMG_PENDING_VERIFY;mark_read_error=true;
    assert(!pet_firmware_image_confirm(app[0].address,sha,&bootloader)&&mark_calls==5);
    ota_read_error=mark_read_error=false;assert(pet_firmware_image_confirm(app[0].address,sha,&bootloader)&&mark_calls==5);
    states[1]=ESP_OTA_IMG_UNDEFINED;assert(!pet_firmware_image_confirm(app[0].address,sha,&bootloader)&&mark_calls==5);
    sequences[0]=sequences[1]=UINT32_MAX;assert(!pet_firmware_image_confirm(app[0].address,sha,&bootloader)&&mark_calls==5);
    puts("firmware image: inactive-only writes, receipt ACK, security/layout, exact read-back/hash/identity, abort cleanup and boot-selection gates passed");return 0;
}
