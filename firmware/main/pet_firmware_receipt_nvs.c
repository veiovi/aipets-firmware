#include "pet_firmware_receipt_nvs.h"
#include "nvs.h"

static const char *key(unsigned slot){return slot==0?"state_a":"state_b";}
static int read_record(void *unused,unsigned slot,uint8_t out[PET_FIRMWARE_RECEIPT_BYTES])
{
    (void)unused;if(slot>1)return -1;
    nvs_handle_t handle;esp_err_t error=nvs_open("pet_fw_v2",NVS_READONLY,&handle);
    if(error==ESP_ERR_NVS_NOT_FOUND)return 0;
    if(error!=ESP_OK)return -1;
    size_t bytes=PET_FIRMWARE_RECEIPT_BYTES;
    error=nvs_get_blob(handle,key(slot),out,&bytes);nvs_close(handle);
    if(error==ESP_ERR_NVS_NOT_FOUND)return 0;
    return error==ESP_OK&&bytes==PET_FIRMWARE_RECEIPT_BYTES?1:-1;
}
static bool write_record(void *unused,unsigned slot,const uint8_t data[PET_FIRMWARE_RECEIPT_BYTES])
{
    (void)unused;if(slot>1)return false;
    nvs_handle_t handle;if(nvs_open("pet_fw_v2",NVS_READWRITE,&handle)!=ESP_OK)return false;
    esp_err_t error=nvs_set_blob(handle,key(slot),data,PET_FIRMWARE_RECEIPT_BYTES);
    if(error==ESP_OK)error=nvs_commit(handle);
    nvs_close(handle);return error==ESP_OK;
}
bool pet_firmware_receipt_open_nvs(pet_firmware_receipt_store_t *store)
{
    const pet_firmware_receipt_io_t io={read_record,write_record,NULL};
    return pet_firmware_receipt_open(store,&io);
}
