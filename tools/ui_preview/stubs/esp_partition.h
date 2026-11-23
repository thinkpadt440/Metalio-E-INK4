#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int esp_err_t;
typedef int esp_partition_mmap_handle_t;
typedef struct { size_t size; } esp_partition_t;
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 0xff
#define ESP_PARTITION_MMAP_DATA 0
#define ESP_OK 0
const esp_partition_t* esp_partition_find_first(int,int,const char*);
esp_err_t esp_partition_mmap(const esp_partition_t*,size_t,size_t,int,const void**,esp_partition_mmap_handle_t*);
void esp_partition_munmap(esp_partition_mmap_handle_t);
const char* esp_err_to_name(int);
#ifdef __cplusplus
}
#endif
