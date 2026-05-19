#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <linux/types.h>
#include <linux/magic.h>
#include <linux/crc32.h> 

#define MAGIC_NUMBER 0x12345678
#define SECTOR_SIZE 512
#define MAX_FILES 1024
#define MAX_NAME_LENGTH 64

struct filesystem_superblock {
    __le32 magic;
    __le32 version;
    __le64 sectors_num;
    __le32 max_file_size;
    __le64 sb2_offset;
    __le32 checksum;
    __u8 reserve[468];
} __attribute__((packed));

struct fs_file_meta {
    char name[256];          
    __u64 disk_offset_sectors;        
    __u32 size_sectors;      
    __u32 content_hash;       
    struct list_head list;     
};

struct superblock_info {
    struct block_device *device;
    __u64 sb1_offset;          
    __u64 sb2_offset;          
    __u32 max_file_sectors;    
    __u32 max_name_len;        
    struct list_head files;    
    spinlock_t lock;           
    __u32 sb_checksum;
};

static inline struct fs_sb_info *FS_SB(struct super_block *sb)
{
    return (struct fs_sb_info *)sb->s_fs_info;
}

#endif