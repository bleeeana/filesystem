#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "../filesystem_ioctl.h"
#define MNT "/mnt"
static int open_mnt(const char *mnt)
{
    int fd = open(mnt, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        perror(mnt);
    return fd;
}
static int simple_ioctl(const char *mnt, unsigned long cmd, const char *msg)
{
    int fd = open_mnt(mnt);
    int ret;
    if (fd < 0)
        return 1;
    ret = ioctl(fd, cmd);
    if (ret < 0)
        perror("ioctl");
    else
        puts(msg);
    close(fd);
    return ret < 0;
}
static int parse_index(const char *s, uint64_t *v)
{
    char *end = NULL;
    errno = 0;
    *v = strtoull(s, &end, 10);
    return errno || !end || *end;
}
static int cmd_hashes(const char *mnt)
{
    struct filesystem_hashes_request req = { 0 };
    struct filesystem_hash_info *e;
    uint64_t i;
    int fd = open_mnt(mnt);
    int ret = 1;
    if (fd < 0 || ioctl(fd, FS_IOCTL_GET_HASHES, &req) < 0)
        goto out_fd;
    e = calloc(req.total ? req.total : 1, sizeof(*e));
    if (!e)
        goto out_fd;
    req.capacity = req.total;
    req.entries = (uint64_t)(uintptr_t)e;
    if (ioctl(fd, FS_IOCTL_GET_HASHES, &req) < 0)
        goto out_free;
    printf("Files: %" PRIu64 "\n", req.total);
    for (i = 0; i < req.copied; i++)
        printf("[%5" PRIu64 "] %-16s first=%" PRIu64 " sectors=%u crc32=0x%08x\n",
               e[i].file_index, e[i].name, e[i].first_sector, e[i].size_sectors, e[i].hash);
    ret = 0;
out_free:
    free(e);
out_fd:
    if (ret)
        perror("hashes");
    if (fd >= 0)
        close(fd);
    return ret;
}
static int cmd_mapping(const char *mnt, const char *target)
{
    struct filesystem_mapping_info map = { 0 };
    uint64_t parsed, *sectors = NULL;
    uint64_t i;
    int fd = open_mnt(mnt);
    int ret = 1;
    if (fd < 0)
        return 1;
    if (parse_index(target, &parsed) == 0)
        map.file_index = parsed;
    else
        snprintf(map.name, sizeof(map.name), "%s", target);
    if (ioctl(fd, FS_IOCTL_GET_MAPPING, &map) < 0)
        goto out;
    sectors = calloc(map.total ? map.total : 1, sizeof(*sectors));
    if (!sectors)
        goto out;
    map.capacity = map.total;
    map.sectors = (uint64_t)(uintptr_t)sectors;
    if (ioctl(fd, FS_IOCTL_GET_MAPPING, &map) < 0)
        goto out;
    printf("Mapping for file_%" PRIu64 " (%" PRIu64 " sectors):\n", map.file_index, map.total);
    for (i = 0; i < map.copied; i++)
        printf("  [%" PRIu64 "] sector=%" PRIu64 "\n", i, sectors[i]);
    ret = 0;
out:
    if (ret)
        perror("mapping");
    free(sectors);
    close(fd);
    return ret;
}
static uint64_t rnd64(void)
{
    return ((uint64_t)rand() << 32) ^ (uint64_t)rand();
}
static int check_file(const char *path)
{
    char expected[64], actual[64] = { 0 };
    int len = snprintf(expected, sizeof(expected), "%" PRIu64 "\n", rnd64());
    int fd = open(path, O_RDWR);
    int ok;
    if (fd < 0 || len <= 0)
        return -1;
    ok = pwrite(fd, expected, len, 0) == len &&
         pread(fd, actual, len, 0) == len &&
         memcmp(expected, actual, len) == 0;
    close(fd);
    return ok ? 0 : -1;
}
static int cmd_demo(const char *mnt)
{
    DIR *dir = opendir(mnt);
    struct dirent *de;
    uint64_t ok = 0, fail = 0;
    if (!dir) {
        perror(mnt);
        return 1;
    }
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    while ((de = readdir(dir))) {
        char path[4096];
        if (de->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", mnt, de->d_name) >= (int)sizeof(path) ||
            check_file(path))
            fail++;
        else
            ok++;
    }
    closedir(dir);
    printf("Demo result: ok=%" PRIu64 " failed=%" PRIu64 "\n", ok, fail);
    return fail != 0;
}
int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    const char *mnt = argc > 2 ? argv[2] : MNT;
    if (!strcmp(cmd, "demo"))
        return cmd_demo(mnt);
    if (!strcmp(cmd, "zero"))
        return simple_ioctl(mnt, FS_IOCTL_ZERO_ALL, "All file data sectors were zeroed.");
    if (!strcmp(cmd, "erase"))
        return simple_ioctl(mnt, FS_IOCTL_ERASE_FS, "Filesystem superblocks and data were erased.");
    if (!strcmp(cmd, "hashes"))
        return cmd_hashes(mnt);
    if (!strcmp(cmd, "mapping") && argc >= 3)
        return cmd_mapping(argc > 3 ? argv[3] : MNT, argv[2]);
    fprintf(stderr, "Usage: %s demo|zero|erase|hashes|mapping ...\n", argv[0]);
    return 1;
}
