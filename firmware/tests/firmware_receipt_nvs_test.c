#include "pet_firmware_receipt_nvs.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t data[2][PET_FIRMWARE_RECEIPT_BYTES];static bool present[2];
static bool fail_commit,corrupt_read,short_read;static unsigned writes,commits,opens,closes;
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *handle)
{assert(!strcmp(name,"pet_fw_v2"));assert(mode==NVS_READONLY||mode==NVS_READWRITE);*handle=(unsigned)(mode+1);++opens;return ESP_OK;}
static unsigned slot(const char *name){assert(!strcmp(name,"state_a")||!strcmp(name,"state_b"));return !strcmp(name,"state_b");}
esp_err_t nvs_get_blob(nvs_handle_t handle,const char *name,void *out,size_t *bytes)
{assert(handle==1);unsigned n=slot(name);if(!present[n])return ESP_ERR_NVS_NOT_FOUND;assert(*bytes==PET_FIRMWARE_RECEIPT_BYTES);memcpy(out,data[n],*bytes);if(corrupt_read)((uint8_t *)out)[100]^=1;if(short_read)--*bytes;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t handle,const char *name,const void *value,size_t bytes)
{assert(handle==2&&bytes==PET_FIRMWARE_RECEIPT_BYTES);unsigned n=slot(name);memcpy(data[n],value,bytes);present[n]=true;++writes;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t handle){assert(handle==2);++commits;return fail_commit?ESP_FAIL:ESP_OK;}
void nvs_close(nvs_handle_t handle){assert(handle==1||handle==2);++closes;}
int main(void)
{
    pet_firmware_receipt_store_t store;
    assert(pet_firmware_receipt_open_nvs(&store));assert(writes==2&&commits==2&&opens==closes);
    unsigned before=writes;assert(pet_firmware_receipt_open_nvs(&store));assert(writes==before&&opens==closes);
    short_read=true;assert(!pet_firmware_receipt_open_nvs(&store));assert(writes==before);short_read=false;
    corrupt_read=true;assert(!pet_firmware_receipt_open_nvs(&store));assert(writes==before);corrupt_read=false;
    assert(pet_firmware_receipt_open_nvs(&store));fail_commit=true;
    assert(!pet_firmware_receipt_save(&store,&store.value));assert(!store.loaded&&opens==closes);
    fail_commit=false;assert(pet_firmware_receipt_open_nvs(&store));
    assert(store.generation==2); // Durable data despite an ambiguous commit error.
    puts("firmware receipt NVS: dedicated keys, exact lengths, commit/read-back failures and no reset of existing corruption passed");return 0;
}
