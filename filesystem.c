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
#include <linux/mnt_idmapping.h>
#include <linux/crc32.h>
#include <linux/uaccess.h>
#include <linux/timekeeping.h>
#include <linux/dirent.h>
#include <linux/sched/signal.h>
#include "filesystem.h"
#include "filesystem_ioctl.h"

static char *disk_name;
static uint sb1_offset;
static uint sb2_offset = 1024;
static uint max_name_len = 64;
static uint max_file_sectors = 8;

module_param(disk_name, charp, 0);
module_param(sb1_offset, uint, 0);
module_param(sb2_offset, uint, 0);
module_param(max_name_len, uint, 0);
module_param(max_file_sectors, uint, 0);

static struct file_system_type fs_type;
static const struct inode_operations fs_root_iops;
static const struct file_operations fs_root_fops;
static const struct file_operations fs_file_fops;

struct fs_file_layout {
    __u64 first_sector;
    __u32 sectors;
};

static void fs_name(__u64 index, char *buf, size_t size)
{
    snprintf(buf, size, "file_%llu", index);
}

static int fs_parse_name(const char *name, __u64 *index)
{
    if (strncmp(name, "file_", 5) != 0 || !name[5])
        return -EINVAL;
    return kstrtoull(name + 5, 10, index);
}

static __u64 fs_range_file_count(__u64 start, __u64 end, __u32 file_sectors)
{
    __u64 sectors;

    if (end <= start)
        return 0;
    sectors = end - start;
    return DIV_ROUND_UP_ULL(sectors, file_sectors);
}

static __u64 fs_file_count_for(const struct superblock_info *info)
{
    __u64 first_sb = min(info->sb1_offset, info->sb2_offset);
    __u64 second_sb = max(info->sb1_offset, info->sb2_offset);

    return fs_range_file_count(0, first_sb, info->max_file_sectors) +
           fs_range_file_count(first_sb + 1, second_sb, info->max_file_sectors) +
           fs_range_file_count(second_sb + 1, info->total_sectors, info->max_file_sectors);
}

static bool fs_layout_in_range(__u64 start, __u64 end, __u32 max_sectors,
                               __u64 *index, struct fs_file_layout *layout)
{
    __u64 count = fs_range_file_count(start, end, max_sectors);

    if (*index >= count) {
        *index -= count;
        return false;
    }

    layout->first_sector = start + *index * max_sectors;
    layout->sectors = min_t(__u64, max_sectors, end - layout->first_sector);
    return true;
}

static int fs_file_layout(const struct superblock_info *info, __u64 index,
                          struct fs_file_layout *layout)
{
    __u64 first_sb = min(info->sb1_offset, info->sb2_offset);
    __u64 second_sb = max(info->sb1_offset, info->sb2_offset);
    __u64 idx = index;

    if (fs_layout_in_range(0, first_sb, info->max_file_sectors, &idx, layout) ||
        fs_layout_in_range(first_sb + 1, second_sb, info->max_file_sectors, &idx, layout) ||
        fs_layout_in_range(second_sb + 1, info->total_sectors, info->max_file_sectors, &idx, layout))
        return 0;
    return -ENOENT;
}

static int fs_check_names(const struct superblock_info *info)
{
    char name[FS_IOCTL_NAME_LEN];

    if (!info->file_count)
        return -ENOSPC;
    fs_name(info->file_count - 1, name, sizeof(name));
    return strlen(name) <= info->max_name_len ? 0 : -ENAMETOOLONG;
}

static int fs_rw_sector(struct super_block *sb, __u64 sector, void *buf,
                        size_t len, bool write)
{
    struct buffer_head *bh = sb_bread(sb, sector);

    if (!bh)
        return -EIO;

    if (write) {
        memset(bh->b_data, 0, FS_SB(sb)->sector_size);
        memcpy(bh->b_data, buf, len);
        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
    } else {
        memcpy(buf, bh->b_data, len);
    }

    brelse(bh);
    return 0;
}

static int fs_zero_sector(struct super_block *sb, __u64 sector)
{
    int ret;
    char *zero;

    zero = kzalloc(FS_SB(sb)->sector_size, GFP_KERNEL);
    if (!zero)
        return -ENOMEM;

    ret = fs_rw_sector(sb, sector, zero, FS_SB(sb)->sector_size, true);

    kfree(zero);
    return ret;
}

static int fs_zero_all_files(struct super_block *sb, struct superblock_info *info)
{
    struct fs_file_layout layout;
    __u64 i;
    __u32 s;
    int ret;
    if (info->erased) return -EIO;
    for (i = 0; i < info->file_count; i++) {
        if (fatal_signal_pending(current))
            return -EINTR;
        ret = fs_file_layout(info, i, &layout);
        if (ret)
            return ret;
        for (s = 0; s < layout.sectors; s++) {
            ret = fs_zero_sector(sb, layout.first_sector + s);
            if (ret)
                return ret;
        }
        cond_resched();
    }
    return 0;
}

static int fs_hash_file(struct super_block *sb, const struct fs_file_layout *layout,
                        __u32 *hash_out)
{
    void *data;
    __u32 hash = ~0U;
    __u32 i;
    int ret;

    data = kmalloc(FS_SB(sb)->sector_size, GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    for (i = 0; i < layout->sectors; i++) {
        ret = fs_rw_sector(sb, layout->first_sector + i, data, FS_SB(sb)->sector_size, false);
        if (ret)
            goto out;
        hash = crc32(hash, data, FS_SB(sb)->sector_size);
    }

    *hash_out = hash;
    ret = 0;
    out:
        kfree(data);
        return ret;
}

static long fs_ioctl_hashes(struct super_block *sb, struct superblock_info *info,
                            unsigned long arg)
{
    struct filesystem_hashes_request req;
    __u64 i;
    if (info->erased) return -EIO;
    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    req.total = info->file_count;
    req.copied = 0;
    for (i = 0; req.entries && i < info->file_count && req.copied < req.capacity; i++) {
        struct filesystem_hash_info entry;
        struct filesystem_hash_info __user *dst;
        struct fs_file_layout layout;
        int ret = fs_file_layout(info, i, &layout);

        if (ret)
            return ret;

        memset(&entry, 0, sizeof(entry));
        entry.file_index = i;
        entry.first_sector = layout.first_sector;
        entry.size_sectors = layout.sectors;
        fs_name(i, entry.name, sizeof(entry.name));
        ret = fs_hash_file(sb, &layout, &entry.hash);
        if (ret)
            return ret;

        dst = (void __user *)(unsigned long)(req.entries + req.copied * sizeof(entry));
        if (copy_to_user(dst, &entry, sizeof(entry)))
            return -EFAULT;
        req.copied++;
        cond_resched();
    }

    return copy_to_user((void __user *)arg, &req, sizeof(req)) ? -EFAULT : 0;
}

static long fs_ioctl_mapping(struct superblock_info *info, unsigned long arg)
{
    struct filesystem_mapping_info map;
    struct fs_file_layout layout;
    __u64 i;
    int ret;
    if (info->erased) return -EIO;

    if (copy_from_user(&map, (void __user *)arg, sizeof(map)))
        return -EFAULT;

    map.name[sizeof(map.name) - 1] = '\0';
    if (map.name[0]) {
        ret = fs_parse_name(map.name, &map.file_index);
        if (ret)
            return ret;
    }

    ret = fs_file_layout(info, map.file_index, &layout);
    if (ret)
        return ret;

    map.total = layout.sectors;
    map.copied = 0;
    for (i = 0; map.sectors && i < layout.sectors && i < map.capacity; i++) {
        __u64 sector = layout.first_sector + i;
        __u64 __user *dst = (void __user *)(unsigned long)(map.sectors + i * sizeof(sector));

        if (copy_to_user(dst, &sector, sizeof(sector)))
            return -EFAULT;
        map.copied++;
    }

    return copy_to_user((void __user *)arg, &map, sizeof(map)) ? -EFAULT : 0;
}

static long fs_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct super_block *sb = file_inode(file)->i_sb;
    struct superblock_info *info = FS_SB(sb);
    int ret;

    if (!info)
        return -EINVAL;

    switch (cmd) {
    case FS_IOCTL_ZERO_ALL:
        mutex_lock(&info->lock);
        ret = fs_zero_all_files(sb, info);
        mutex_unlock(&info->lock);
        return ret;
    case FS_IOCTL_ERASE_FS:
        mutex_lock(&info->lock);

        ret = fs_zero_all_files(sb, info);
        if (!ret) ret = fs_rw_sector(sb, info->sb1_offset, NULL, 0, true);
        if (!ret) ret = fs_rw_sector(sb, info->sb2_offset, NULL, 0, true);
        if (!ret) info->erased = true;
        mutex_unlock(&info->lock);
        return ret;
    case FS_IOCTL_GET_HASHES:
        mutex_lock(&info->lock);
        ret = fs_ioctl_hashes(sb, info, arg);
        mutex_unlock(&info->lock);
        return ret;
    case FS_IOCTL_GET_MAPPING:
        mutex_lock(&info->lock);
        ret = fs_ioctl_mapping(info, arg);
        mutex_unlock(&info->lock);
        return ret;
    default:
        return -ENOTTY;
    }
}

static struct inode *fs_make_inode(struct super_block *sb, umode_t mode,
                                   __u64 ino, loff_t size)
{
    struct inode *inode = new_inode(sb);
    struct timespec64 ts;

    if (!inode)
        return NULL;

    inode->i_ino = ino;
    inode->i_mode = mode;
    inode->i_size = size;
    inode_init_owner(&nop_mnt_idmap, inode, NULL, mode);
    set_nlink(inode, S_ISDIR(mode) ? 2 : 1);

    ts = current_time(inode);
    inode_set_atime_to_ts(inode, ts);
    inode_set_mtime_to_ts(inode, ts);
    inode_set_ctime_to_ts(inode, ts);

    if (S_ISDIR(mode)) {
        inode->i_op = &fs_root_iops;
        inode->i_fop = &fs_root_fops;
    } else {
        inode->i_fop = &fs_file_fops;
    }
    return inode;
}

static int fs_iterate_shared(struct file *file, struct dir_context *ctx)
{
    struct superblock_info *info = FS_SB(file_inode(file)->i_sb);
    char name[FS_IOCTL_NAME_LEN];
    __u64 i;
    if (info->erased) return 0;

    if (!dir_emit_dots(file, ctx))
        return 0;

    for (i = ctx->pos - 2; i < info->file_count; i++) {
        fs_name(i, name, sizeof(name));
        if (!dir_emit(ctx, name, strlen(name), FS_FIRST_FILE_INO + i, DT_REG))
            return 0;
        ctx->pos = i + 3;
    }
    return 0;
}

static struct dentry *fs_lookup(struct inode *dir, struct dentry *dentry,
                                unsigned int flags __maybe_unused)
{
    struct superblock_info *info = FS_SB(dir->i_sb);
    struct fs_file_layout layout;
    struct inode *inode = NULL;
    __u64 index;

    if (dentry->d_name.len <= info->max_name_len &&
        fs_parse_name(dentry->d_name.name, &index) == 0 &&
        index < info->file_count &&
        fs_file_layout(info, index, &layout) == 0 &&
        !info->erased) {
        inode = fs_make_inode(dir->i_sb, S_IFREG | 0644,
                              FS_FIRST_FILE_INO + index,
                              (loff_t)layout.sectors * info->sector_size);
        if (!inode)
            return ERR_PTR(-ENOMEM);
    }

    d_add(dentry, inode);
    return NULL;
}

static ssize_t fs_transfer(struct kiocb *iocb, struct iov_iter *iter, bool write)
{
    struct inode *inode = file_inode(iocb->ki_filp);
    struct superblock_info *info = FS_SB(inode->i_sb);
    struct fs_file_layout layout;
    loff_t pos = iocb->ki_pos;
    loff_t size;
    size_t done = 0;
    size_t count = iov_iter_count(iter);
    int ret;
    mutex_lock(&info->lock);
    if (info->erased) {
        mutex_unlock(&info->lock);
        return -EIO;
    }
    ret = fs_file_layout(info, inode->i_ino - FS_FIRST_FILE_INO, &layout);
    if (ret){
        mutex_unlock(&info->lock);
        return ret;
    }
    size = (loff_t)layout.sectors * info->sector_size;
    if (pos < 0){
        mutex_unlock(&info->lock);
        return -EINVAL;
    }
    if (pos >= size) {
        mutex_unlock(&info->lock);
        return write ? -ENOSPC : 0;
    }
       
    if (count > size - pos)
        count = size - pos;

    while (done < count) {
        size_t off = pos % info->sector_size;;
        size_t chunk = min_t(size_t,
                     min_t(size_t, count - done, info->sector_size),
                     info->sector_size - off);
        struct buffer_head *bh = sb_bread(inode->i_sb,
                                          layout.first_sector + pos / info->sector_size);
        size_t copied;

        if (!bh) {
            mutex_unlock(&info->lock);
            return done ? done : -EIO;
        }
           

        copied = write ? copy_from_iter(bh->b_data + off, chunk, iter) :
                         copy_to_iter(bh->b_data + off, chunk, iter);
        if (copied != chunk) {
            brelse(bh);
            mutex_unlock(&info->lock);
            return done ? done : -EFAULT;
        }
        if (write) {
            mark_buffer_dirty(bh);
            sync_dirty_buffer(bh);
        }

        brelse(bh);
        done += chunk;
        pos += chunk;
    }

    iocb->ki_pos = pos;
    if (write && done) {
        struct timespec64 ts = current_time(inode);

        inode_set_mtime_to_ts(inode, ts);
        inode_set_ctime_to_ts(inode, ts);
        mark_inode_dirty(inode);
    }
    mutex_unlock(&info->lock);
    return done;
}

static ssize_t fs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    return fs_transfer(iocb, to, false);
}

static ssize_t fs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    return fs_transfer(iocb, from, true);
}

static const struct file_operations fs_file_fops = {
    .read_iter = fs_read_iter,
    .write_iter = fs_write_iter,
    .unlocked_ioctl = fs_unlocked_ioctl,
    .llseek = generic_file_llseek,
};

static const struct inode_operations fs_root_iops = {
    .lookup = fs_lookup,
};

static const struct file_operations fs_root_fops = {
    .iterate_shared = fs_iterate_shared,
    .unlocked_ioctl = fs_unlocked_ioctl,
    .llseek = generic_file_llseek,
};

static __u32 fs_super_checksum(const struct filesystem_superblock *disk)
{
    struct filesystem_superblock tmp = { 0 };

    memcpy(&tmp, disk, sizeof(tmp));
    tmp.checksum = 0;

    return crc32(~0U, (u8 *)&tmp, sizeof(tmp));
}

static void fs_fill_disk_super(struct filesystem_superblock *disk,
                               const struct superblock_info *info)
{
    memset(disk, 0, sizeof(*disk));
    disk->magic = cpu_to_le32(MAGIC_NUMBER);
    disk->version = cpu_to_le32(FS_VERSION);
    disk->sectors_num = cpu_to_le64(info->total_sectors);
    disk->sb1_offset = cpu_to_le64(info->sb1_offset);
    disk->sb2_offset = cpu_to_le64(info->sb2_offset);
    disk->file_count = cpu_to_le64(info->file_count);
    disk->max_file_size = cpu_to_le32(info->max_file_sectors);
    disk->max_name_len = cpu_to_le32(info->max_name_len);
    disk->checksum = cpu_to_le32(fs_super_checksum(disk));
}

static int fs_validate_super(const struct filesystem_superblock *disk,
                             const struct superblock_info *info, __u64 sector)
{
    if (le32_to_cpu(disk->magic) != MAGIC_NUMBER ||
        le32_to_cpu(disk->version) != FS_VERSION ||
        le64_to_cpu(disk->sectors_num) != info->total_sectors ||
        le64_to_cpu(disk->sb1_offset) >= info->total_sectors ||
        le64_to_cpu(disk->sb2_offset) >= info->total_sectors ||
        le64_to_cpu(disk->sb1_offset) == le64_to_cpu(disk->sb2_offset) ||
        le32_to_cpu(disk->max_file_size) == 0 ||
        le32_to_cpu(disk->max_file_size) > 1024 ||
        le32_to_cpu(disk->max_name_len) == 0 ||
        le32_to_cpu(disk->max_name_len) > MAX_NAME_LENGTH ||
        le32_to_cpu(disk->checksum) != fs_super_checksum(disk))
        return -EINVAL;

    if (sector != le64_to_cpu(disk->sb1_offset) &&
        sector != le64_to_cpu(disk->sb2_offset))
        return -EINVAL;
    return 0;
}

static void fs_info_from_disk(struct superblock_info *info,
                              const struct filesystem_superblock *disk)
{
    info->sb1_offset = le64_to_cpu(disk->sb1_offset);
    info->sb2_offset = le64_to_cpu(disk->sb2_offset);
    info->file_count = le64_to_cpu(disk->file_count);
    info->max_file_sectors = le32_to_cpu(disk->max_file_size);
    info->max_name_len = le32_to_cpu(disk->max_name_len);
    info->sb_checksum = le32_to_cpu(disk->checksum);
}

static int fs_write_superblocks(struct super_block *sb, struct superblock_info *info)
{
    struct filesystem_superblock *disk;
    int ret;

    disk = kzalloc(sizeof(*disk), GFP_KERNEL);
    if (!disk)
        return -ENOMEM;

    fs_fill_disk_super(disk, info);
    ret = fs_rw_sector(sb, info->sb1_offset, disk, sizeof(*disk), true);
    if (!ret)
        ret = fs_rw_sector(sb, info->sb2_offset, disk, sizeof(*disk), true);
    kfree(disk);
    return ret;
}

static int fs_format(struct super_block *sb, struct superblock_info *info)
{
    int ret;

    info->file_count = fs_file_count_for(info);
    ret = fs_check_names(info);
    if (ret)
        return ret;

    ret = blkdev_issue_discard(sb->s_bdev, 0, info->total_sectors,
                               GFP_KERNEL);
    if (ret == -EOPNOTSUPP)
        ret = blkdev_issue_zeroout(sb->s_bdev, 0, info->total_sectors,
                                   GFP_KERNEL, 0);
    if (ret)
        return ret;

    return fs_write_superblocks(sb, info);
}

static bool fs_is_disk_empty(struct super_block *sb, struct superblock_info *info)
{
    char *buf;
    int i;
    bool empty = true;
    const int sectors_to_check = 16;

    buf = kmalloc(info->sector_size, GFP_KERNEL);
    if (!buf)
        return false;

    for (i = 0; i < sectors_to_check && i < info->total_sectors; i++) {
        int ret = fs_rw_sector(sb, i, buf, info->sector_size, false);
        if (ret) {
            empty = false;
            break;
        }
        if (memchr_inv(buf, 0, info->sector_size) != NULL) {
            empty = false;
            break;
        }
    }

    kfree(buf);
    return empty;
}

static int fs_load_or_format(struct super_block *sb, struct superblock_info *info)
{
    struct filesystem_superblock *disk;
    int ok[2] = { 0 };
    int ret;

    disk = kcalloc(2, sizeof(*disk), GFP_KERNEL);
    if (!disk)
        return -ENOMEM;

    ret = fs_rw_sector(sb, info->sb1_offset, &disk[0], sizeof(*disk), false);
    if (!ret)
        ok[0] = fs_validate_super(&disk[0], info, info->sb1_offset) == 0;

    ret = fs_rw_sector(sb, info->sb2_offset, &disk[1], sizeof(*disk), false);
    if (!ret)
        ok[1] = fs_validate_super(&disk[1], info, info->sb2_offset) == 0;

    if (ok[0] && ok[1]) {
        fs_info_from_disk(info, &disk[0]);
        if (info->file_count != fs_file_count_for(info)) {
            ret = -EUCLEAN;
            goto out;
        }
        ret = fs_check_names(info);
        goto out;
    }
    if (fs_is_disk_empty(sb, info)) {
        pr_info("empty system, formatting\n");
        ret = fs_format(sb, info);
        if (ret) {
            pr_err("format failed\n");
            goto out;
        }
        ret = fs_rw_sector(sb, info->sb1_offset, &disk[0], sizeof(*disk), false);
        if (ret)
            goto out;
        fs_info_from_disk(info, &disk[0]);
        ret = 0;
        goto out;
    }
    pr_err("superblock validation failed\n");
    ret = -EUCLEAN;
out:
    kfree(disk);
    return ret;
}

static int fs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct superblock_info *info = FS_SB(dentry->d_sb);

    buf->f_type = MAGIC_NUMBER;
    buf->f_bsize = info->sector_size;;
    buf->f_blocks = info->total_sectors;
    buf->f_files = info->file_count;
    buf->f_namelen = info->max_name_len;
    return 0;
}

static const struct super_operations fs_super_ops = {
    .statfs = fs_statfs,
    .drop_inode = generic_delete_inode,
};

static int fs_fill_super(struct super_block *sb, struct fs_context *fc __maybe_unused)
{
    struct superblock_info *info;
    struct inode *root;
    int ret;

    info = kzalloc(sizeof(*info), GFP_KERNEL);
    if (!info)
        return -ENOMEM;
    sb->s_fs_info = info;

    info->device = sb->s_bdev;
    info->total_sectors = bdev_nr_sectors(sb->s_bdev);
    info->sb1_offset = sb1_offset;
    info->sb2_offset = sb2_offset;
    info->max_name_len = max_name_len;
    info->max_file_sectors = max_file_sectors;
    info->sector_size = bdev_logical_block_size(sb->s_bdev);
    if (!info->sector_size || info->sector_size < 512 ||
        !is_power_of_2(info->sector_size)) {
        ret = -EINVAL;
        goto fail;
    }

    if (!sb_set_blocksize(sb, info->sector_size)){
        ret = -EINVAL;
        goto fail;
    }
    if (info->total_sectors <= 2 ||
        info->sb1_offset >= info->total_sectors ||
        info->sb2_offset >= info->total_sectors ||
        info->sb1_offset == info->sb2_offset) {
        ret = -EINVAL;
        goto fail;
    }

    ret = fs_load_or_format(sb, info);
    if (ret)
        goto fail;
    if (info->file_count > FS_MAX_FILES) {
        ret = -EINVAL;
        goto fail;
    }

    sb->s_magic = MAGIC_NUMBER;
    sb->s_op = &fs_super_ops;

    root = fs_make_inode(sb, S_IFDIR | 0755, FS_ROOT_INO, info->sector_size);
    if (!root) {
        ret = -ENOMEM;
        goto fail;
    }
    sb->s_root = d_make_root(root);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto fail;
    }

    pr_info("FS mounted. %llu files initialized.\n", info->file_count);
    return 0;

fail:
    kfree(info);
    sb->s_fs_info = NULL;
    return ret;
}

static void fs_kill_sb(struct super_block *sb)
{
    struct superblock_info *info = FS_SB(sb);

    kill_block_super(sb);
    kfree(info);
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
    if (!disk_name ||
        !max_name_len || max_name_len > MAX_NAME_LENGTH ||
        !max_file_sectors || max_file_sectors > 1024 ||
        sb1_offset == sb2_offset)
        return -EINVAL;

    fs_type.owner = THIS_MODULE;
    fs_type.name = "filesystem";
    fs_type.fs_flags = FS_REQUIRES_DEV;
    fs_type.init_fs_context = fs_init_fs_context;
    fs_type.kill_sb = fs_kill_sb;
    return register_filesystem(&fs_type);
}

static void __exit filesystem_exit(void)
{
    unregister_filesystem(&fs_type);
}

module_init(filesystem_init);
module_exit(filesystem_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("1304 Bobkov Vladislav");
MODULE_VERSION("0.1");
MODULE_DESCRIPTION("Task");
