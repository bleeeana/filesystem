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
#include <linux/dirent.h>      
#include <linux/string.h>      

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

static const struct file_operations simplefs_file_fops;
static const struct file_operations simplefs_root_fops;
static const struct inode_operations simplefs_root_iops;

/* ============================================================================
 * VFS OPERATIONS
 * ============================================================================ */

static int simplefs_iterate_shared(struct file *file, struct dir_context *ctx)
{
    struct inode *dir = file_inode(file);
    struct super_block *sb = dir->i_sb;
    struct superblock_info *info = FS_SB(sb);
    struct fs_file_meta *meta;
    unsigned long pos = ctx->pos;
    unsigned int ino = 2;

    if (!info) return -EINVAL;

    /* Отдаём "." и "..", если ещё не отдавали */
    if (pos == 0) {
        if (!dir_emit(ctx, ".", 1, 1, DT_DIR)) return 0;
        ctx->pos = 1; pos = 1;
    }
    if (pos == 1) {
        if (!dir_emit(ctx, "..", 2, 1, DT_DIR)) return 0;
        ctx->pos = 2; pos = 2;
    }

    /* Список файлов статичен после mount, спинлок не нужен.
       Убираем его, чтобы dir_emit не блокировал поток. */
    list_for_each_entry(meta, &info->files, list) {
        if (ino <= pos) {
            ino++;
            continue;
        }
        if (!dir_emit(ctx, meta->name, strlen(meta->name), ino, DT_REG))
            return 0; /* Буфер ls заполнен, вернёмся позже */
        
        ctx->pos = ino++;
    }
    return 0; /* Обход завершён */
}

static struct dentry *simplefs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct superblock_info *info = FS_SB(sb);
    
    if (!info) return ERR_PTR(-EINVAL);

    struct fs_file_meta *meta;
    struct inode *inode;
    unsigned long ino = 2;
    bool found = false;

    spin_lock(&info->lock);
    list_for_each_entry(meta, &info->files, list) {
        if (strcmp(meta->name, dentry->d_name.name) == 0) {
            found = true;
            break;
        }
        ino++;
    }
    spin_unlock(&info->lock);

    if (!found) {
        d_add(dentry, NULL);
        return NULL;
    }

    inode = new_inode(sb);
    if (!inode) return ERR_PTR(-ENOMEM);

    inode->i_ino = ino;
    inode->i_mode = S_IFREG | 0644;
    inode->i_size = meta->size_sectors * FS_SECTOR_SIZE;
    
    struct timespec64 ts = current_time(inode);
    inode_set_atime(inode, ts.tv_sec, ts.tv_nsec);
    inode_set_mtime(inode, ts.tv_sec, ts.tv_nsec);
    inode_set_ctime(inode, ts.tv_sec, ts.tv_nsec);
    
    inode->i_private = meta;          
    inode->i_fop = &simplefs_file_fops; 

    d_add(dentry, inode);
    return NULL;
}

static ssize_t simplefs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct fs_file_meta *meta = inode->i_private;
    loff_t pos = iocb->ki_pos;
    size_t count = iov_iter_count(to);
    size_t total_read = 0;

    if (pos >= inode->i_size) return 0;
    if (pos + count > inode->i_size) count = inode->i_size - pos;
    if (count == 0) return 0;

    while (total_read < count) {
        loff_t sector_off = pos / FS_SECTOR_SIZE;
        size_t offset_in_sec = pos % FS_SECTOR_SIZE;
        size_t to_read = min_t(size_t, count - total_read, FS_SECTOR_SIZE - offset_in_sec);
        __u64 disk_sector = meta->disk_offset_sectors + sector_off;

        struct buffer_head *bh = sb_bread(sb, disk_sector);
        if (!bh) return -EIO;

        if (copy_to_iter(bh->b_data + offset_in_sec, to_read, to) != to_read) {
            brelse(bh);
            return -EFAULT;
        }

        brelse(bh);
        total_read += to_read;
        pos += to_read;
    }

    iocb->ki_pos = pos;
    return total_read;
}

static ssize_t simplefs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct fs_file_meta *meta = inode->i_private;
    loff_t pos = iocb->ki_pos;
    size_t count = iov_iter_count(from);
    size_t total_written = 0;

    if (pos + count > inode->i_size) count = inode->i_size - pos;
    if (count == 0) return 0;

    while (total_written < count) {
        loff_t sector_off = pos / FS_SECTOR_SIZE;
        size_t offset_in_sec = pos % FS_SECTOR_SIZE;
        size_t to_write = min_t(size_t, count - total_written, FS_SECTOR_SIZE - offset_in_sec);
        __u64 disk_sector = meta->disk_offset_sectors + sector_off;

        struct buffer_head *bh = sb_bread(sb, disk_sector);
        if (!bh) return -EIO;

        if (copy_from_iter(bh->b_data + offset_in_sec, to_write, from) != to_write) {
            brelse(bh);
            return -EFAULT;
        }

        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        brelse(bh);

        total_written += to_write;
        pos += to_write;
    }

    struct timespec64 ts = current_time(inode);
    inode_set_mtime(inode, ts.tv_sec, ts.tv_nsec);
    inode_set_ctime(inode, ts.tv_sec, ts.tv_nsec);
    mark_inode_dirty(inode);
    iocb->ki_pos = pos;
    return total_written;
}

static const struct inode_operations simplefs_file_iops = {};
static const struct file_operations simplefs_file_fops = {
    .read_iter = simplefs_read_iter,
    .write_iter = simplefs_write_iter,
    .llseek    = generic_file_llseek,
};

static const struct inode_operations simplefs_root_iops = {
    .lookup = simplefs_lookup,
};

static const struct file_operations simplefs_root_fops = {
    .llseek         = generic_file_llseek,
    .iterate_shared = simplefs_iterate_shared,
};

/* ============================================================================
 * CORE FS FUNCTIONS
 * ============================================================================ */

static int fs_read_sector(struct super_block *sb, __u64 sector, void *buf)
{
    struct buffer_head *bh = sb_bread(sb, sector);
    if (!bh) {
        dbg_print("Failed to read sector %llu\n", sector);
        return -EIO;
    }
    memcpy(buf, bh->b_data, FS_SECTOR_SIZE);
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
    
    /* ЗАЩИТА ОТ ПЕРЕПОЛНЕНИЯ: проверяем размер устройства */
    if (usable_sectors < 1025) {
        pr_err("Device too small: %llu sectors\n", usable_sectors);
        return -ENOSPC;
    }

    __u64 space_between = (info->sb2_offset > info->sb1_offset) ? 
                          (info->sb2_offset - info->sb1_offset - 1) : 0;
    __u64 space_after = usable_sectors - info->sb2_offset - 1;
    
    info->file_count = (space_between / info->max_file_sectors) + 
                       (space_after / info->max_file_sectors);

    pr_info("Formatting: %llu files, %llu sectors\n", info->file_count, usable_sectors);

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
    memcpy(bh->b_data, &sb_disk, FS_SECTOR_SIZE);
    mark_buffer_dirty(bh); sync_dirty_buffer(bh); brelse(bh);

    bh = sb_bread(sb, info->sb2_offset);
    if (!bh) return -EIO;
    memcpy(bh->b_data, &sb_disk, FS_SECTOR_SIZE);
    mark_buffer_dirty(bh); sync_dirty_buffer(bh); brelse(bh);

    return 0;
}

static int fs_statfs(struct dentry *dentry, struct kstatfs *buf) { return 0; }

static const struct super_operations fs_super_ops = {
    .statfs = fs_statfs,
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

    /* ✅ ИСПРАВЛЕНО: Присваиваем device СРАЗУ, до любых операций с диском */
    info->device = sb->s_bdev;
    
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

    /* Убрали повторное info->device = sb->s_bdev; отсюда */
    info->total_sectors = le64_to_cpu(sb_disk.sectors_num);
    info->file_count = le64_to_cpu(sb_disk.file_count);
    info->sb_checksum = le32_to_cpu(sb_disk.checksum);

    if (info->file_count > 100000) {
        pr_err("File count too large: %llu, aborting\n", info->file_count);
        kfree(info); sb->s_fs_info = NULL;
        return -EINVAL;
    }

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
    sb->s_blocksize = FS_SECTOR_SIZE;
    sb->s_blocksize_bits = ilog2(FS_SECTOR_SIZE);
    sb->s_op = &fs_super_ops;

    struct inode *root_inode = new_inode(sb);
    if (!root_inode) return -ENOMEM;

    root_inode->i_ino = 1;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_op = &simplefs_root_iops;
    root_inode->i_fop = &simplefs_root_fops;
    inode_init_owner(&nop_mnt_idmap, root_inode, NULL, S_IFDIR);
    
    struct timespec64 ts = current_time(root_inode);
    inode_set_atime(root_inode, ts.tv_sec, ts.tv_nsec);
    inode_set_mtime(root_inode, ts.tv_sec, ts.tv_nsec);
    inode_set_ctime(root_inode, ts.tv_sec, ts.tv_nsec);

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
MODULE_DESCRIPTION("Simple educational block-based filesystem");
MODULE_VERSION("0.1");