#define pr_fmt(fmt) "filesystem: " fmt  

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h> 
#include <linux/fs_context.h>   
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/mnt_idmapping.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/crc32.h>
#include <linux/log2.h>        
#include <linux/uaccess.h>
#include <linux/dcache.h>      
#include <linux/timekeeping.h> 
#include <linux/dirent.h>      
#include <linux/string.h>      
#include <linux/sched/signal.h>  
#include "filesystem.h"
#include "filesystem_ioctl.h"

#ifdef DEBUG
    #define dbg_print(fmt, ...) pr_info("[DBG] %s:%d: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
    #define dbg_print(fmt, ...) do {} while(0)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    #define FS_SET_ATIME(i, ts) inode_set_atime_to_ts(i, ts)
    #define FS_SET_MTIME(i, ts) inode_set_mtime_to_ts(i, ts)
    #define FS_SET_CTIME(i, ts) inode_set_ctime_to_ts(i, ts)
#else
    #define FS_SET_ATIME(i, ts) (i)->i_atime = ts
    #define FS_SET_MTIME(i, ts) (i)->i_mtime = ts
    #define FS_SET_CTIME(i, ts) (i)->i_ctime = ts
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    #define FS_INIT_OWNER(inode, dir, mode) inode_init_owner(&nop_mnt_idmap, inode, dir, mode)
#else
    #define FS_INIT_OWNER(inode, dir, mode) inode_init_owner(&init_user_ns, inode, dir, mode)
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
static const struct file_operations fs_file_fops;
static const struct file_operations fs_root_fops;
static const struct inode_operations fs_file_iops;
static const struct inode_operations fs_root_iops;

static int fs_parse_file_index(const char *name, __u64 *index)
{
    if (strncmp(name, "file_", 5) != 0 || !name[5])
        return -EINVAL;
    return kstrtoull(name + 5, 10, index);
}

static struct fs_file_meta *fs_find_meta_by_index(struct superblock_info *info, __u64 index)
{
    struct fs_file_meta *meta;
    __u64 current_idx = 0;

    list_for_each_entry(meta, &info->files, list) {
        if (current_idx == index)
            return meta;
        current_idx++;
    }
    return NULL;
}

static struct fs_file_meta *fs_find_meta_by_name(struct superblock_info *info, const char *name)
{
    struct fs_file_meta *meta;

    list_for_each_entry(meta, &info->files, list) {
        if (strcmp(meta->name, name) == 0)
            return meta;
    }
    return NULL;
}

static int fs_zero_sector(struct super_block *sb, __u64 sector)
{
    struct buffer_head *bh;

    bh = sb_bread(sb, sector);
    if (!bh)
        return -EIO;

    memset(bh->b_data, 0, FS_SECTOR_SIZE);
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    return 0;
}

static int fs_hash_file(struct super_block *sb, const struct fs_file_meta *meta, __u32 *hash_out)
{
    __u32 hash = ~0U;
    __u64 i;

    for (i = 0; i < meta->size_sectors; i++) {
        struct buffer_head *bh;

        bh = sb_bread(sb, meta->disk_offset_sectors + i);
        if (!bh)
            return -EIO;
        hash = crc32(hash, bh->b_data, FS_SECTOR_SIZE);
        brelse(bh);
    }

    *hash_out = hash;
    return 0;
}

static long fs_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct superblock_info *info = FS_SB(sb);
    struct fs_file_meta *meta;
    int ret;

    if (!info)
        return -EINVAL;

    switch (cmd) {
    case FS_IOCTL_ZERO_ALL:
        pr_info("IOCTL: zeroing all files\n");
        list_for_each_entry(meta, &info->files, list) {
            __u64 i;

            if (fatal_signal_pending(current))
                return -ERESTARTSYS;

            for (i = 0; i < meta->size_sectors; i++) {
                ret = fs_zero_sector(sb, meta->disk_offset_sectors + i);
                if (ret)
                    return ret;
            }
            cond_resched();
        }
        return 0;

    case FS_IOCTL_ERASE_FS:
        pr_info("IOCTL: erasing filesystem superblocks and data\n");
        list_for_each_entry(meta, &info->files, list) {
            __u64 i;

            for (i = 0; i < meta->size_sectors; i++) {
                ret = fs_zero_sector(sb, meta->disk_offset_sectors + i);
                if (ret)
                    return ret;
            }
            cond_resched();
        }
        ret = fs_zero_sector(sb, info->sb1_offset);
        if (ret)
            return ret;
        return fs_zero_sector(sb, info->sb2_offset);

    case FS_IOCTL_GET_HASHES: {
        struct filesystem_hashes_request req;
        __u64 copied = 0;
        __u64 index = 0;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        req.total = info->file_count;
        req.copied = 0;

        if (req.capacity && req.entries) {
            list_for_each_entry(meta, &info->files, list) {
                struct filesystem_hash_info entry;
                struct filesystem_hash_info __user *entry_user;

                if (copied >= req.capacity)
                    break;

                memset(&entry, 0, sizeof(entry));
                entry.file_index = index;
                entry.first_sector = meta->disk_offset_sectors;
                entry.size_sectors = meta->size_sectors;
                ret = fs_hash_file(sb, meta, &entry.hash);
                if (ret)
                    return ret;
                strscpy(entry.name, meta->name, sizeof(entry.name));

                entry_user = (struct filesystem_hash_info __user *)(unsigned long)
                    (req.entries + copied * sizeof(entry));
                if (copy_to_user(entry_user, &entry, sizeof(entry)))
                    return -EFAULT;

                copied++;
                index++;
                cond_resched();
            }
        }

        req.copied = copied;
        if (copy_to_user((void __user *)arg, &req, sizeof(req)))
            return -EFAULT;
        return 0;
    }

    case FS_IOCTL_GET_MAPPING: {
        struct filesystem_mapping_info map;
        __u64 index;
        __u64 copied;

        if (copy_from_user(&map, (void __user *)arg, sizeof(map)))
            return -EFAULT;

        map.name[sizeof(map.name) - 1] = '\0';
        if (map.name[0]) {
            ret = fs_parse_file_index(map.name, &index);
            if (ret)
                return ret;
            meta = fs_find_meta_by_name(info, map.name);
        } else {
            index = map.file_index;
            meta = fs_find_meta_by_index(info, index);
        }
        if (!meta)
            return -ENOENT;

        map.file_index = index;
        map.total = meta->size_sectors;
        map.copied = 0;
        copied = 0;

        if (map.capacity && map.sectors) {
            for (copied = 0; copied < meta->size_sectors && copied < map.capacity; copied++) {
                __u64 sector = meta->disk_offset_sectors + copied;
                __u64 __user *sector_user;

                sector_user = (__u64 __user *)(unsigned long)
                    (map.sectors + copied * sizeof(sector));
                if (copy_to_user(sector_user, &sector, sizeof(sector)))
                    return -EFAULT;
            }
        }

        map.copied = copied;
        if (copy_to_user((void __user *)arg, &map, sizeof(map)))
            return -EFAULT;
        return 0;
    }

    default:
        return -ENOTTY;
    }
}

static int fs_iterate_shared(struct file *file, struct dir_context *ctx)
{
    struct inode *dir = file_inode(file);
    struct super_block *sb = dir->i_sb;
    struct superblock_info *info = FS_SB(sb);
    struct fs_file_meta *meta;
    loff_t pos = ctx->pos;
    unsigned int ino = 2;

    if (!info) return -EINVAL;

    dbg_print("iterate_shared: ctx->pos=%lld\n", pos);

    if (pos == 0) {
        if (!dir_emit(ctx, ".", 1, 1, DT_DIR)) return 0;
        ctx->pos = 1; pos = 1;
    }
    if (pos <= 1) {
        if (!dir_emit(ctx, "..", 2, 1, DT_DIR)) return 0;
        ctx->pos = 2; pos = 2;
    }
    if (pos < 2) pos = 2;

    list_for_each_entry(meta, &info->files, list) {
        if (ino < pos) {
            ino++;
            continue;
        }
        dbg_print("iterate_shared: emitting ino=%u, name='%s'\n", ino, meta->name);
        if (!dir_emit(ctx, meta->name, strlen(meta->name), ino, DT_REG))
            return 0;
        ctx->pos = ++ino;
    }
    dbg_print("iterate_shared: finished iteration, ctx->pos=%lld\n", ctx->pos);
    return 0;
}

static struct dentry *fs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags __maybe_unused)
{
    struct super_block *sb = dir->i_sb;
    struct superblock_info *info = FS_SB(sb);
    struct fs_file_meta *meta;
    struct inode *inode;
    struct timespec64 ts;
    unsigned long ino = 2;
    bool found = false;

    if (!info) return ERR_PTR(-EINVAL);

    dbg_print("lookup: searching for '%s'\n", dentry->d_name.name);

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
        dbg_print("lookup: file not found\n");
        d_add(dentry, NULL);
        return NULL;
    }

    dbg_print("lookup: found file at ino=%lu, disk_offset=%llu, size=%u sectors\n", 
              ino, meta->disk_offset_sectors, meta->size_sectors);

    inode = new_inode(sb);
    if (!inode) return ERR_PTR(-ENOMEM);

    inode->i_ino = ino;
    inode->i_mode = S_IFREG | 0644;
    inode->i_size = meta->size_sectors * FS_SECTOR_SIZE;
    set_nlink(inode, 1);
    
    ts = current_time(inode);
    FS_SET_ATIME(inode, ts);
    FS_SET_MTIME(inode, ts);
    FS_SET_CTIME(inode, ts);
    
    inode->i_op = &fs_file_iops;
    inode->i_private = meta;          
    inode->i_fop = &fs_file_fops; 

    d_add(dentry, inode);
    return NULL;
}

static ssize_t fs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct fs_file_meta *meta = inode->i_private;
    loff_t pos = iocb->ki_pos;
    size_t count = iov_iter_count(to);
    size_t total_read = 0;

    dbg_print("read_iter: file offset=%lld, count=%zu, file_size=%lld\n", pos, count, inode->i_size);

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

    dbg_print("read_iter: read %zu bytes total\n", total_read);
    iocb->ki_pos = pos;
    return total_read;
}

static ssize_t fs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct superblock_info *info = FS_SB(sb);
    struct fs_file_meta *meta;
    struct timespec64 ts;
    loff_t pos = iocb->ki_pos;
    size_t count = iov_iter_count(from);
    size_t total_written = 0;

    if (!info) {
        pr_err("write_iter: info is NULL!\n");
        return -EINVAL;
    }

    spin_lock(&info->lock);
    meta = inode->i_private;
    if (!meta) {
        spin_unlock(&info->lock);
        pr_err("write_iter: meta is NULL!\n");
        return -EINVAL;
    }

    inode->i_size = meta->size_sectors * FS_SECTOR_SIZE;
    if (pos < 0) {
        spin_unlock(&info->lock);
        return -EINVAL;
    }
    if (pos >= inode->i_size) {
        spin_unlock(&info->lock);
        return -ENOSPC;
    }
    if (pos + count > inode->i_size) count = inode->i_size - pos;
    if (count == 0) {
        spin_unlock(&info->lock);
        return 0;
    }
    spin_unlock(&info->lock);

    dbg_print("write_iter: offset=%lld, count=%zu, file_size=%lld\n", 
              pos, count, inode->i_size);

    while (total_written < count) {
        loff_t sector_off = pos / FS_SECTOR_SIZE;
        size_t offset_in_sec = pos % FS_SECTOR_SIZE;
        size_t to_write = min_t(size_t, count - total_written, FS_SECTOR_SIZE - offset_in_sec);
        __u64 disk_sector = meta->disk_offset_sectors + sector_off;
        ssize_t copied;
        struct buffer_head *bh = sb_bread(sb, disk_sector);

        if (!bh) {
            pr_err("write_iter: sb_bread FAILED for sector %llu\n", disk_sector);
            return total_written > 0 ? total_written : -EIO;
        }

        copied = copy_from_iter(bh->b_data + offset_in_sec, to_write, from);
        if (copied != to_write) {
            pr_err("write_iter: copy_from_iter FAILED (got %ld, expected %zu)\n", copied, to_write);
            brelse(bh);
            return total_written > 0 ? total_written : -EFAULT;
        }

        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        brelse(bh);

        total_written += to_write;
        pos += to_write;
    }

    dbg_print("write_iter: wrote %zu bytes\n", total_written);

    ts = current_time(inode);
    FS_SET_MTIME(inode, ts);
    FS_SET_CTIME(inode, ts);
    mark_inode_dirty(inode);
    iocb->ki_pos = pos;
    
    return total_written;
}

static const struct inode_operations fs_file_iops = {};

static const struct file_operations fs_file_fops = {
    .read_iter       = fs_read_iter,
    .write_iter      = fs_write_iter,
    .unlocked_ioctl  = fs_unlocked_ioctl, 
    .llseek          = generic_file_llseek,
};

static const struct inode_operations fs_root_iops = {
    .lookup = fs_lookup,
};

static const struct file_operations fs_root_fops = {
    .llseek          = generic_file_llseek,
    .iterate_shared  = fs_iterate_shared,
    .unlocked_ioctl  = fs_unlocked_ioctl,  
};

static int fs_read_sector(struct super_block *sb, __u64 sector, void *buf)
{
    struct buffer_head *bh;

    bh = sb_bread(sb, sector);
    if (!bh) {
        dbg_print("Failed to read sector %llu\n", sector);
        return -EIO;
    }
    memcpy(buf, bh->b_data, FS_SECTOR_SIZE);
    brelse(bh);
    return 0;
}

static int fs_write_sector(struct super_block *sb, __u64 sector, const void *buf, size_t len)
{
    struct buffer_head *bh;

    bh = sb_bread(sb, sector);
    if (!bh)
        return -EIO;

    memset(bh->b_data, 0, FS_SECTOR_SIZE);
    memcpy(bh->b_data, buf, len);
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    return 0;
}

static __u32 fs_super_checksum(const struct filesystem_superblock *sb_disk)
{
    struct filesystem_superblock tmp = *sb_disk;

    tmp.checksum = 0;
    return crc32(~0U, (u8 *)&tmp, sizeof(tmp));
}

static int fs_validate_sb(const struct filesystem_superblock *sb_disk,
                          __u64 copy_sector, __u64 device_sectors)
{
    __u32 expected_checksum;
    __u64 stored_sb1;
    __u64 stored_sb2;

    if (le32_to_cpu(sb_disk->magic) != MAGIC_NUMBER) {
        dbg_print("Invalid magic: 0x%x (expected 0x%x)\n", 
                  le32_to_cpu(sb_disk->magic), MAGIC_NUMBER);
        return -EINVAL;
    }
    if (le32_to_cpu(sb_disk->version) != FS_VERSION)
        return -EINVAL;

    expected_checksum = fs_super_checksum(sb_disk);
    if (expected_checksum != le32_to_cpu(sb_disk->checksum)) {
        dbg_print("Checksum mismatch!\n");
        return -EUCLEAN;
    }

    stored_sb1 = le64_to_cpu(sb_disk->sb1_offset);
    stored_sb2 = le64_to_cpu(sb_disk->sb2_offset);
    if (le64_to_cpu(sb_disk->sectors_num) != device_sectors ||
        stored_sb1 >= device_sectors ||
        stored_sb2 >= device_sectors ||
        stored_sb1 == stored_sb2)
        return -EINVAL;
    if (copy_sector != stored_sb1 && copy_sector != stored_sb2)
        return -EINVAL;
    if (le32_to_cpu(sb_disk->max_file_size) == 0 ||
        le32_to_cpu(sb_disk->max_name_len) == 0 ||
        le32_to_cpu(sb_disk->max_name_len) > MAX_NAME_LENGTH)
        return -EINVAL;

    return 0;
}

static __u64 fs_count_range_files(__u64 start, __u64 end, __u32 max_file_sectors)
{
    __u64 sectors;

    if (end <= start)
        return 0;
    sectors = end - start;
    return (sectors + max_file_sectors - 1) / max_file_sectors;
}

static __u64 fs_count_files(const struct superblock_info *info)
{
    __u64 first_sb = min(info->sb1_offset, info->sb2_offset);
    __u64 second_sb = max(info->sb1_offset, info->sb2_offset);

    return fs_count_range_files(0, first_sb, info->max_file_sectors) +
           fs_count_range_files(first_sb + 1, second_sb, info->max_file_sectors) +
           fs_count_range_files(second_sb + 1, info->total_sectors, info->max_file_sectors);
}

static int fs_check_generated_names(const struct superblock_info *info)
{
    char name[FS_IOCTL_NAME_LEN];

    if (!info->file_count)
        return 0;
    snprintf(name, sizeof(name), "file_%llu", info->file_count - 1);
    if (strlen(name) > info->max_name_len) {
        pr_err("max_name_len=%u is too small for generated name '%s'\n",
               info->max_name_len, name);
        return -ENAMETOOLONG;
    }
    return 0;
}

static void fs_build_superblock(struct filesystem_superblock *sb_disk,
                                const struct superblock_info *info)
{
    memset(sb_disk, 0, sizeof(*sb_disk));
    sb_disk->magic = cpu_to_le32(MAGIC_NUMBER);
    sb_disk->version = cpu_to_le32(FS_VERSION);
    sb_disk->sectors_num = cpu_to_le64(info->total_sectors);
    sb_disk->sb1_offset = cpu_to_le64(info->sb1_offset);
    sb_disk->sb2_offset = cpu_to_le64(info->sb2_offset);
    sb_disk->file_count = cpu_to_le64(info->file_count);
    sb_disk->max_file_size = cpu_to_le32(info->max_file_sectors);
    sb_disk->max_name_len = cpu_to_le32(info->max_name_len);
    sb_disk->checksum = cpu_to_le32(fs_super_checksum(sb_disk));
}

static int fs_write_superblocks(struct super_block *sb, const struct superblock_info *info)
{
    struct filesystem_superblock *sb_disk;
    int ret;

    sb_disk = kzalloc(sizeof(*sb_disk), GFP_KERNEL);
    if (!sb_disk)
        return -ENOMEM;

    fs_build_superblock(sb_disk, info);
    ret = fs_write_sector(sb, info->sb1_offset, sb_disk, sizeof(*sb_disk));
    if (ret)
        goto out;
    ret = fs_write_sector(sb, info->sb2_offset, sb_disk, sizeof(*sb_disk));

out:
    kfree(sb_disk);
    return ret;
}

static int fs_format_disk(struct super_block *sb, struct superblock_info *info)
{
    __u64 sector;
    int ret;

    if (info->total_sectors <= 2) {
        pr_err("Device too small: %llu sectors\n", info->total_sectors);
        return -ENOSPC;
    }

    info->file_count = fs_count_files(info);
    ret = fs_check_generated_names(info);
    if (ret)
        return ret;

    pr_info("Formatting: %llu files, %llu sectors\n", info->file_count, info->total_sectors);

    for (sector = 0; sector < info->total_sectors; sector++) {
        if (sector == info->sb1_offset || sector == info->sb2_offset)
            continue;
        ret = fs_zero_sector(sb, sector);
        if (ret)
            return ret;
        cond_resched();
    }

    return fs_write_superblocks(sb, info);
}

static int fs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct superblock_info *info = FS_SB(dentry->d_sb);

    buf->f_type = MAGIC_NUMBER;
    buf->f_bsize = FS_SECTOR_SIZE;
    buf->f_blocks = info ? info->total_sectors : 0;
    buf->f_bfree = 0;
    buf->f_bavail = 0;
    buf->f_files = info ? info->file_count : 0;
    buf->f_ffree = 0;
    buf->f_namelen = info ? info->max_name_len : MAX_NAME_LENGTH;
    return 0;
}

static const struct super_operations fs_super_ops = {
    .statfs = fs_statfs,
    .drop_inode = generic_delete_inode,
};

static void fs_free_file_list(struct superblock_info *info)
{
    struct fs_file_meta *meta;
    struct fs_file_meta *tmp;

    list_for_each_entry_safe(meta, tmp, &info->files, list) {
        list_del(&meta->list);
        kfree(meta);
    }
}

static int fs_add_files_for_range(struct superblock_info *info,
                                  __u64 start, __u64 end, __u64 *index)
{
    __u64 sector = start;

    while (sector < end) {
        struct fs_file_meta *file_meta;
        __u64 remaining = end - sector;
        __u32 file_sectors = min_t(__u64, remaining, info->max_file_sectors);

        file_meta = kzalloc(sizeof(*file_meta), GFP_KERNEL);
        if (!file_meta)
            return -ENOMEM;

        snprintf(file_meta->name, sizeof(file_meta->name), "file_%llu", *index);
        file_meta->disk_offset_sectors = sector;
        file_meta->size_sectors = file_sectors;
        file_meta->content_hash = 0;

        list_add_tail(&file_meta->list, &info->files);
        sector += file_sectors;
        (*index)++;
    }

    return 0;
}

static int fs_rebuild_file_list(struct superblock_info *info)
{
    __u64 first_sb = min(info->sb1_offset, info->sb2_offset);
    __u64 second_sb = max(info->sb1_offset, info->sb2_offset);
    __u64 index = 0;
    int ret;

    fs_free_file_list(info);

    ret = fs_add_files_for_range(info, 0, first_sb, &index);
    if (ret)
        return ret;
    ret = fs_add_files_for_range(info, first_sb + 1, second_sb, &index);
    if (ret)
        return ret;
    ret = fs_add_files_for_range(info, second_sb + 1, info->total_sectors, &index);
    if (ret)
        return ret;

    if (index != info->file_count) {
        pr_err("File metadata mismatch: superblock=%llu generated=%llu\n",
               info->file_count, index);
        return -EUCLEAN;
    }

    return 0;
}

static void fs_info_from_disk(struct superblock_info *info,
                              const struct filesystem_superblock *sb_disk)
{
    info->total_sectors = le64_to_cpu(sb_disk->sectors_num);
    info->sb1_offset = le64_to_cpu(sb_disk->sb1_offset);
    info->sb2_offset = le64_to_cpu(sb_disk->sb2_offset);
    info->file_count = le64_to_cpu(sb_disk->file_count);
    info->max_file_sectors = le32_to_cpu(sb_disk->max_file_size);
    info->max_name_len = le32_to_cpu(sb_disk->max_name_len);
    info->sb_checksum = le32_to_cpu(sb_disk->checksum);
}

static int fs_load_or_format(struct super_block *sb, struct superblock_info *info)
{
    struct filesystem_superblock *primary;
    struct filesystem_superblock *backup;
    int primary_ret;
    int backup_ret;
    int ret;

    info->total_sectors = bdev_nr_sectors(info->device);

    if (info->sb1_offset >= info->total_sectors ||
        info->sb2_offset >= info->total_sectors ||
        info->sb1_offset == info->sb2_offset) {
        pr_err("Invalid superblock offsets: sb1=%llu sb2=%llu total=%llu\n",
               info->sb1_offset, info->sb2_offset, info->total_sectors);
        return -EINVAL;
    }

    primary = kmalloc(sizeof(*primary), GFP_KERNEL);
    backup = kmalloc(sizeof(*backup), GFP_KERNEL);
    if (!primary || !backup) {
        kfree(primary);
        kfree(backup);
        return -ENOMEM;
    }

    primary_ret = fs_read_sector(sb, info->sb1_offset, primary);
    if (!primary_ret)
        primary_ret = fs_validate_sb(primary, info->sb1_offset, info->total_sectors);

    backup_ret = fs_read_sector(sb, info->sb2_offset, backup);
    if (!backup_ret)
        backup_ret = fs_validate_sb(backup, info->sb2_offset, info->total_sectors);

    if (!primary_ret || !backup_ret) {
        const struct filesystem_superblock *source = primary_ret ? backup : primary;

        fs_info_from_disk(info, source);
        if (info->file_count != fs_count_files(info)) {
            pr_err("Superblock file_count mismatch: stored=%llu expected=%llu\n",
                   info->file_count, fs_count_files(info));
            ret = -EUCLEAN;
            goto out;
        }
        ret = fs_check_generated_names(info);
        if (ret)
            goto out;

        if (primary_ret || backup_ret) {
            pr_info("Repairing missing/corrupted superblock copy\n");
            ret = fs_write_superblocks(sb, info);
            if (ret)
                goto out;
        }
        ret = 0;
        goto out;
    }

    pr_info("No valid superblock found. Formatting disk...\n");
    ret = fs_format_disk(sb, info);

out:
    kfree(primary);
    kfree(backup);
    return ret;
}

static int fs_fill_super(struct super_block *sb, struct fs_context *fc __maybe_unused)
{
    struct superblock_info *info;
    struct inode *root_inode;
    struct timespec64 ts;
    int ret;

    dbg_print("FILL_SUPER START\n");

    if (!sb_set_blocksize(sb, FS_SECTOR_SIZE))
        return -EINVAL;

    info = kzalloc(sizeof(*info), GFP_KERNEL);
    if (!info) return -ENOMEM;
    sb->s_fs_info = info;

    info->device = sb->s_bdev;
    INIT_LIST_HEAD(&info->files);
    spin_lock_init(&info->lock);
    info->sb1_offset = sb1_offset;
    info->sb2_offset = sb2_offset;
    info->max_name_len = max_name_len;
    info->max_file_sectors = max_file_sectors;

    ret = fs_load_or_format(sb, info);
    if (ret)
        goto fail;

    if (info->file_count > FS_MAX_FILES) {
        pr_err("File count too large: %llu, aborting\n", info->file_count);
        ret = -EINVAL;
        goto fail;
    }

    ret = fs_rebuild_file_list(info);
    if (ret)
        goto fail;

    sb->s_magic = MAGIC_NUMBER;
    sb->s_op = &fs_super_ops;

    root_inode = new_inode(sb);
    if (!root_inode) {
        ret = -ENOMEM;
        goto fail;
    }

    root_inode->i_ino = FS_ROOT_INO;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_op = &fs_root_iops;
    root_inode->i_fop = &fs_root_fops;
    FS_INIT_OWNER(root_inode, NULL, S_IFDIR | 0755);
    set_nlink(root_inode, 2);
    
    ts = current_time(root_inode);
    FS_SET_ATIME(root_inode, ts);
    FS_SET_MTIME(root_inode, ts);
    FS_SET_CTIME(root_inode, ts);

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto fail;
    }

    pr_info("FS mounted. %llu files initialized.\n", info->file_count);
    return 0;

fail:
    fs_free_file_list(info);
    kfree(info);
    sb->s_fs_info = NULL;
    return ret;
}

static void fs_kill_sb(struct super_block *sb)
{
    struct superblock_info *info = FS_SB(sb);

    dbg_print("KILL_SB START\n");
    kill_block_super(sb);
    if (info) {
        fs_free_file_list(info);
        kfree(info);
        sb->s_fs_info = NULL;
    }
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
    int ret;

    dbg_print("MODULE INIT START\n");
    if (!disk_name) {
        pr_err("ERROR: disk_name required!\n");
        return -EINVAL;
    }
    if (max_name_len == 0 || max_name_len > MAX_NAME_LENGTH ||
        max_file_sectors == 0 || max_file_sectors > 1024 ||
        sb1_offset == sb2_offset) {
        pr_err("ERROR: Invalid parameters\n");
        return -EINVAL;
    }

    fs_type.owner = THIS_MODULE;
    fs_type.name = "filesystem";
    fs_type.fs_flags = FS_REQUIRES_DEV;
    fs_type.init_fs_context = fs_init_fs_context;
    fs_type.kill_sb = fs_kill_sb;

    ret = register_filesystem(&fs_type);
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
MODULE_DESCRIPTION("Task");
