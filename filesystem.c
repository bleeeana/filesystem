#define pr_fmt(fmt) "filesystem: " fmt  

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/blkdev.h>`
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/xxhash.h>

#include "filesystem.h"
#include "filesystem_ioctl.h"

#ifdef DEBUG
    #define dbg_print(fmt, ...) pr_info("[DBG] %s:%d: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
    #define dbg_print(fmt, ...) do {} while(0)
#endif


static char *disk_name = NULL;           
static uint sb1_offset = 0;              
static uint sb2_offset = 1024;           
static uint max_name_len = 64;           
static uint max_file_sectors = 8;        

module_param(disk_name, charp, 0);
MODULE_PARM_DESC(disk_name, "Block device name (e.g., /dev/loop0)");

module_param(sb1_offset, uint, 0);
MODULE_PARM_DESC(sb1_offset, "Offset of first superblock copy in sectors (default: 0)");

module_param(sb2_offset, uint, 0);
MODULE_PARM_DESC(sb2_offset, "Offset of second superblock copy in sectors (default: 1024)");

module_param(max_name_len, uint, 0);
MODULE_PARM_DESC(max_name_len, "Maximum filename length (default: 64)");

module_param(max_file_sectors, uint, 0);
MODULE_PARM_DESC(max_file_sectors, "Maximum file size in sectors (default: 8)");

static struct file_system_type fs_type; 
static int fs_major;  

static int __init filesystem_init(void)
{
    dbg_print("MODULE INIT START\n");
    dbg_print("Parameters: disk=%s, sb1=%u, sb2=%u, name_len=%u, max_sectors=%u\n",
              disk_name ?: "NULL", sb1_offset, sb2_offset, max_name_len, max_file_sectors);

    if (!disk_name) {
        pr_err("ERROR: disk_name parameter is required!\n");
        pr_err("Usage: insmod filesystem.ko disk_name=/dev/loop0 [sb1_offset=0] [sb2_offset=1024] ...\n");
        return -EINVAL; 
    }

    if (max_name_len == 0 || max_name_len > 255) {
        pr_err("ERROR: max_name_len must be 1..255 (got %u)\n", max_name_len);
        return -EINVAL;
    }
    if (max_file_sectors == 0 || max_file_sectors > 1024) {
        pr_err("ERROR: max_file_sectors must be 1..1024 (got %u)\n", max_file_sectors);
        return -EINVAL;
    }

    fs_type.name = "filesystem";
    fs_type.fs_flags = FS_REQUIRES_DEV;

    int ret = register_filesystem(&fs_type);
    if (ret) {
        pr_err("ERROR: Failed to register filesystem: %d\n", ret);
        return ret;
    }

    pr_info("Module loaded successfully. Use: mount -t filesystem %s /mnt -o sb1_offset=%u,sb2_offset=%u\n",
            disk_name, sb1_offset, sb2_offset);
    
    dbg_print("MODULE INIT DONE\n");
    return 0;
}

static void __exit filesystem_exit(void)
{
    dbg_print("MODULE EXIT START\n");

    unregister_filesystem(&fs_type);

    pr_info("Module unloaded.\n");
    dbg_print("=== MODULE EXIT DONE ===\n");
}

module_init(filesystem_init);
module_exit(filesystem_exit);

MODULE_LICENSE("GPL");              
MODULE_AUTHOR("1304 Bobkov Vladislav");       
MODULE_VERSION("0.1"); 