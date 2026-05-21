#ifndef FILESYSTEM_IOCTL_H
#define FILESYSTEM_IOCTL_H

#ifdef __KERNEL__
    #include <linux/ioctl.h>
    #include <linux/types.h>
#else
    #include <stdint.h>
    #include <sys/ioctl.h>
    typedef uint32_t __u32;
    typedef uint64_t __u64;
#endif

#define FS_IOCTL_MAGIC 'f'
#define FS_IOCTL_NAME_LEN 256

struct filesystem_hash_info {
    __u64 file_index;
    __u64 first_sector;
    __u32 hash;
    __u32 size_sectors;
    char name[FS_IOCTL_NAME_LEN];
};

struct filesystem_hashes_request {
    __u64 capacity;
    __u64 total;
    __u64 copied;
    __u64 entries;
};

struct filesystem_mapping_info {
    char name[FS_IOCTL_NAME_LEN];
    __u64 file_index;
    __u64 capacity;
    __u64 total;
    __u64 copied;
    __u64 sectors;
};

#define FS_IOCTL_ZERO_ALL    _IO(FS_IOCTL_MAGIC, 1)
#define FS_IOCTL_ERASE_FS    _IO(FS_IOCTL_MAGIC, 2)
#define FS_IOCTL_GET_HASHES  _IOWR(FS_IOCTL_MAGIC, 3, struct filesystem_hashes_request)
#define FS_IOCTL_GET_MAPPING _IOWR(FS_IOCTL_MAGIC, 4, struct filesystem_mapping_info)

#endif
