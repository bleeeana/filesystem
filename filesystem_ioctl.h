#ifndef FILESYSTEM_IOCTL_H
#define FILESYSTEM_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define FILESYSTEM_IOC_MAGIC 'n'

struct filesystem_hash_info {
    __u32 file_index;          
    char name[256];            
    __u32 hash;                
    __u32 size_sectors;  
};

struct filesystem_mapping_info {
    __u32 file_index;         
    __u32 sector_count;        
    __u64 sectors[128];       
};

// 1. Обнулить содержимое файлов
#define FS_IOCTL_ZERO_ALL _IO(FILESYSTEM_IOC_MAGIC, 1)

// 2. Полностью стереть ФС (записать нули в оба суперблока + все файлы)
#define FS_IOCTL_ERASE_FS _IO(FILESYSTEM_IOC_MAGIC, 2)

// 3. Получить массив хешей для файлов
#define FS_IOCTL_GET_HASHES _IOR(FILESYSTEM_IOC_MAGIC, 3, struct filesystem_hash_info *)

// 4. Получить маппинг секторов для конкретного файла
#define FS_IOCTL_GET_MAPPING _IOWR(FILESYSTEM_IOC_MAGIC, 4, struct filesystem_mapping_info *)

#define FS_IOC_MAXNR 4

#endif