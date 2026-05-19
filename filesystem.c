#define pr_fmt(fmt) "filesystem: " fmt  

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h> 
#include <linux/fs_context.h>   
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/crc32.h>
#include <linux/log2.h>        
#include <linux/uaccess.h>
#include <linux/dcache.h>      
#include <linux/mnt_idmapping.h> 
#include <linux/timekeeping.h> 

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
MODULE_PARM_DESC(disk_name, "Block device name (e.g., /dev/loop0) - REQUIRED");
module_param(sb1_offset, uint, 0);
MODULE_PARM_DESC(sb1_offset, "Offset of first SB copy in sectors (default: 0)");
module_param(sb2_offset, uint, 0);
MODULE_PARM_DESC(sb2_offset, "Offset of second SB copy in sectors (default: 1024)");
module_param(max_name_len, uint, 0);
MODULE_PARM_DESC(max_name_len, "Maximum filename length (default: 64)");
module_param(max_file_sectors, uint, 0);
MODULE_PARM_DESC(max_file_sectors, "Maximum file size in sectors, M (default: 8)");

static struct file_system_type fs_type; 

static const struct inode_operations simplefs_root_iops = {};
static const struct file_operations simplefs_root_fops = {};

static int fs_read_sector(struct super_block *sb, __u64 sector, void *buf)
{
    struct buffer_head *bh = sb_bread(sb, sector);
    if (!bh) {
        dbg_print("Failed to read sector %llu\n", sector);
        return -EIO;
    }
    memcpy(buf, bh->b_data, SECTOR_SIZE);
    brelse(bh);
    return 0;
}

static int fs_validate_sb(struct filesystem_superblock *sb_disk)
{
    __le32 expected_checksum;

    if (le32_to_cpu(sb_disk->magic) != MAGIC_NUMBER) {
        dbg_print("Invalid magic: 0x%x (expected 0x%x)\n", 
                  le32_to_cpu(sb_disk->magic), MAGIC_NUMBER);
        return -EINVAL;
    }

    expected_checksum = cpu_to_le32(crc32(~0, (u8 *)sb_disk, 
                                          sizeof(*sb_disk) - sizeof(__le32)) ^ ~0);
    if (expected_checksum != sb_disk->checksum) {
        dbg_print("Checksum mismatch!\n");
        return -EUCLEAN;
    }
    return 0;
}

static int fs_format_disk(struct super_block *sb, struct superblock_info *info)
{
    struct filesystem_superblock sb_disk = {0};
    __u64 usable_sectors = bdev_nr_sectors(info->device);
    __u64 space_between = (info->sb2_offset > info->sb1_offset) ? 
                          (info->sb2_offset - info->sb1_offset - 1) : 0;
    __u64 space_after = usable_sectors - info->sb2_offset - 1;
    
    info->file_count = (space_between / info->max_file_sectors) + 
                       (space_after / info->max_file_sectors);

    sb_disk.magic = cpu_to_le32(MAGIC_NUMBER);
    sb_disk.version = cpu_to_le32(1);
    sb_disk.sectors_num = cpu_to_le64(usable_sectors);
    sb_disk.file_count = cpu_to_le64(info->file_count);
    sb_disk.max_file_size = cpu_to_le32(info->max_file_sectors);
    sb_disk.max_name_len = cpu_to_le32(info->max_name_len);
    sb_disk.sb2_offset = cpu_to_le64(info->sb2_offset);
    sb_disk.checksum = cpu_to_le32(crc32(~0, (u8 *)&sb_disk, 
                                         sizeof(sb_disk) - sizeof(__le32)) ^ ~0);

    struct buffer_head *bh = sb_bread(sb, info->sb1_offset);
    if (!bh) return -EIO;
    memcpy(bh->b_data, &sb_disk, SECTOR_SIZE);
    mark_buffer_dirty(bh); sync_dirty_buffer(bh); brelse(bh);

    bh = sb_bread(sb, info->sb2_offset);
    if (!bh) return -EIO;
    memcpy(bh->b_data, &sb_disk, SECTOR_SIZE);
    mark_buffer_dirty(bh); sync_dirty_buffer(bh); brelse(bh);

    pr_info("Formatted: %llu files, %llu total sectors\n", info->file_count, usable_sectors);
    return 0;
}

static int simple_statfs(struct dentry *dentry, struct kstatfs *buf) { return 0; }

static const struct super_operations fs_super_ops = {
    .statfs = simple_statfs,
};

static int fs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    struct superblock_info *info;
    struct filesystem_superblock sb_disk;
    struct fs_file_meta *file_meta;
    __u64 current_offset;
    int ret;

    dbg_print("FILL_SUPER START\n");

    info = kzalloc(sizeof(*info), GFP_KERNEL);
    if (!info) return -ENOMEM;
    sb->s_fs_info = info;

    INIT_LIST_HEAD(&info->files);
    spin_lock_init(&info->lock);
    info->sb1_offset = sb1_offset;
    info->sb2_offset = sb2_offset;
    info->max_name_len = max_name_len;
    info->max_file_sectors = max_file_sectors;

    ret = fs_read_sector(sb, info->sb1_offset, &sb_disk);
    if (ret == 0) ret = fs_validate_sb(&sb_disk);

    if (ret != 0) {
        dbg_print("Primary SB invalid, trying backup at %llu\n", info->sb2_offset);
        ret = fs_read_sector(sb, info->sb2_offset, &sb_disk);
        if (ret == 0) ret = fs_validate_sb(&sb_disk);
        
        if (ret != 0) {
            pr_info("No valid SB found. Formatting disk...\n");
            ret = fs_format_disk(sb, info);
            if (ret) { kfree(info); sb->s_fs_info = NULL; return ret; }
            ret = fs_read_sector(sb, info->sb1_offset, &sb_disk);
        }
    }

    info->device = sb->s_bdev;
    info->total_sectors = le64_to_cpu(sb_disk.sectors_num);
    info->file_count = le64_to_cpu(sb_disk.file_count);
    info->sb_checksum = le32_to_cpu(sb_disk.checksum);

    current_offset = info->sb1_offset + 1;
    for (__u64 i = 0; i < info->file_count; i++) {
        file_meta = kzalloc(sizeof(*file_meta), GFP_KERNEL);
        if (!file_meta) break;

        snprintf(file_meta->name, sizeof(file_meta->name), "file_%llu", i);
        file_meta->disk_offset_sectors = current_offset;
        file_meta->size_sectors = info->max_file_sectors;
        file_meta->content_hash = 0;

        spin_lock(&info->lock);
        list_add_tail(&file_meta->list, &info->files);
        spin_unlock(&info->lock);

        current_offset += info->max_file_sectors;
        if (current_offset == info->sb2_offset) current_offset++;
    }

    sb->s_magic = MAGIC_NUMBER;
    sb->s_blocksize = SECTOR_SIZE;
    sb->s_blocksize_bits = ilog2(SECTOR_SIZE);
    sb->s_op = &fs_super_ops;

    struct inode *root_inode = new_inode(sb);
    if (!root_inode) return -ENOMEM;

    root_inode->i_ino = 1;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_op = &simplefs_root_iops;
    root_inode->i_fop = &simplefs_root_fops;
    inode_init_owner(&nop_mnt_idmap, root_inode, NULL, S_IFDIR);
    simple_inode_init_ts(root_inode);

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) return -ENOMEM;

    pr_info("FS mounted. %llu files initialized.\n", info->file_count);
    return 0;
}

static void fs_kill_sb(struct super_block *sb)
{
    struct superblock_info *info = FS_SB(sb);
    struct fs_file_meta *meta, *tmp;

    dbg_print("KILL_SB START\n");
    if (info) {
        spin_lock(&info->lock);
        list_for_each_entry_safe(meta, tmp, &info->files, list) {
            list_del(&meta->list);
            kfree(meta);
        }
        spin_unlock(&info->lock);
        kfree(info);
    }
    kill_block_super(sb);
    dbg_print("KILL_SB DONE\n");
}

static int fs_get_tree(struct fs_context *fc)
{
    return get_tree_bdev(fc, fs_fill_super);
}

static const struct fs_context_operations fs_context_ops = {
    .get_tree = fs_get_tree,
};

static int fs_init_fs_context(struct fs_context *fc)
{
    fc->ops = &fs_context_ops;
    return 0;
}

static int __init filesystem_init(void)
{
    dbg_print("MODULE INIT START\n");
    if (!disk_name) {
        pr_err("ERROR: disk_name required!\n");
        return -EINVAL;
    }
    if (max_name_len == 0 || max_name_len > 255 || max_file_sectors == 0 || max_file_sectors > 1024) {
        pr_err("ERROR: Invalid parameters\n");
        return -EINVAL;
    }

    fs_type.name = "filesystem";
    fs_type.fs_flags = FS_REQUIRES_DEV;
    fs_type.init_fs_context = fs_init_fs_context;
    fs_type.kill_sb = fs_kill_sb;

    int ret = register_filesystem(&fs_type);
    if (ret) {
        pr_err("Failed to register: %d\n", ret);
        return ret;
    }

    pr_info("Module loaded. Use: mount -t filesystem %s /mnt\n", disk_name);
    return 0;
}

static void __exit filesystem_exit(void)
{
    dbg_print("MODULE EXIT START\n");
    unregister_filesystem(&fs_type);
    pr_info("Module unloaded.\n");
}

module_init(filesystem_init);
module_exit(filesystem_exit);

MODULE_LICENSE("GPL");              
MODULE_AUTHOR("1304 Bobkov Vladislav");       
MODULE_VERSION("0.1");