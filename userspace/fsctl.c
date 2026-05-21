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
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../filesystem_ioctl.h"

#define DEFAULT_MOUNT_POINT "/mnt"

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s demo [mountpoint]\n"
            "  %s zero [mountpoint]\n"
            "  %s erase [mountpoint]\n"
            "  %s hashes [mountpoint]\n"
            "  %s mapping <index|file_name> [mountpoint]\n",
            prog, prog, prog, prog, prog);
}

static int open_mount(const char *mountpoint)
{
    int fd = open(mountpoint, O_RDONLY | O_DIRECTORY);

    if (fd < 0)
        perror(mountpoint);
    return fd;
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || !end || *end)
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static uint64_t random_u64(void)
{
    uint64_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd >= 0) {
        ssize_t got = read(fd, &value, sizeof(value));

        close(fd);
        if (got == (ssize_t)sizeof(value))
            return value;
    }

    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    value = ((uint64_t)rand() << 32) ^ (uint64_t)rand();
    return value;
}

static int cmd_zero(const char *mountpoint)
{
    int fd = open_mount(mountpoint);
    int ret;

    if (fd < 0)
        return EXIT_FAILURE;
    ret = ioctl(fd, FS_IOCTL_ZERO_ALL);
    if (ret < 0) {
        perror("FS_IOCTL_ZERO_ALL");
        close(fd);
        return EXIT_FAILURE;
    }
    close(fd);
    puts("All file data sectors were zeroed.");
    return EXIT_SUCCESS;
}

static int cmd_erase(const char *mountpoint)
{
    int fd = open_mount(mountpoint);
    int ret;

    if (fd < 0)
        return EXIT_FAILURE;
    ret = ioctl(fd, FS_IOCTL_ERASE_FS);
    if (ret < 0) {
        perror("FS_IOCTL_ERASE_FS");
        close(fd);
        return EXIT_FAILURE;
    }
    close(fd);
    puts("Filesystem superblocks and data were erased. Remount to reformat.");
    return EXIT_SUCCESS;
}

static int cmd_hashes(const char *mountpoint)
{
    struct filesystem_hashes_request req;
    struct filesystem_hash_info *entries = NULL;
    uint64_t i;
    int fd;

    fd = open_mount(mountpoint);
    if (fd < 0)
        return EXIT_FAILURE;

    memset(&req, 0, sizeof(req));
    if (ioctl(fd, FS_IOCTL_GET_HASHES, &req) < 0) {
        perror("FS_IOCTL_GET_HASHES count");
        close(fd);
        return EXIT_FAILURE;
    }

    entries = calloc(req.total ? req.total : 1, sizeof(*entries));
    if (!entries) {
        perror("calloc");
        close(fd);
        return EXIT_FAILURE;
    }

    req.capacity = req.total;
    req.entries = (uint64_t)(uintptr_t)entries;
    if (ioctl(fd, FS_IOCTL_GET_HASHES, &req) < 0) {
        perror("FS_IOCTL_GET_HASHES");
        free(entries);
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Files: %" PRIu64 "\n", req.total);
    for (i = 0; i < req.copied; i++) {
        printf("[%5" PRIu64 "] %-16s first=%" PRIu64 " sectors=%u crc32=0x%08x\n",
               entries[i].file_index, entries[i].name, entries[i].first_sector,
               entries[i].size_sectors, entries[i].hash);
    }

    free(entries);
    close(fd);
    return EXIT_SUCCESS;
}

static int cmd_mapping(const char *mountpoint, const char *target)
{
    struct filesystem_mapping_info map;
    uint64_t *sectors = NULL;
    uint64_t parsed_index;
    uint64_t i;
    int fd;

    fd = open_mount(mountpoint);
    if (fd < 0)
        return EXIT_FAILURE;

    memset(&map, 0, sizeof(map));
    if (parse_u64(target, &parsed_index) == 0) {
        map.file_index = parsed_index;
    } else {
        snprintf(map.name, sizeof(map.name), "%s", target);
    }

    if (ioctl(fd, FS_IOCTL_GET_MAPPING, &map) < 0) {
        perror("FS_IOCTL_GET_MAPPING count");
        close(fd);
        return EXIT_FAILURE;
    }

    sectors = calloc(map.total ? map.total : 1, sizeof(*sectors));
    if (!sectors) {
        perror("calloc");
        close(fd);
        return EXIT_FAILURE;
    }

    map.capacity = map.total;
    map.sectors = (uint64_t)(uintptr_t)sectors;
    if (ioctl(fd, FS_IOCTL_GET_MAPPING, &map) < 0) {
        perror("FS_IOCTL_GET_MAPPING");
        free(sectors);
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Mapping for file_%" PRIu64 " (%" PRIu64 " sectors):\n",
           map.file_index, map.total);
    for (i = 0; i < map.copied; i++)
        printf("  [%" PRIu64 "] sector=%" PRIu64 "\n", i, sectors[i]);

    free(sectors);
    close(fd);
    return EXIT_SUCCESS;
}

static int write_and_read_number(const char *path, uint64_t value)
{
    char expected[64];
    char actual[64];
    int fd;
    int len;
    ssize_t done;

    len = snprintf(expected, sizeof(expected), "%" PRIu64 "\n", value);
    if (len <= 0 || len >= (int)sizeof(expected))
        return -1;

    fd = open(path, O_RDWR);
    if (fd < 0) {
        perror(path);
        return -1;
    }

    done = pwrite(fd, expected, (size_t)len, 0);
    if (done != len) {
        perror("pwrite");
        close(fd);
        return -1;
    }

    memset(actual, 0, sizeof(actual));
    done = pread(fd, actual, (size_t)len, 0);
    close(fd);
    if (done != len) {
        perror("pread");
        return -1;
    }

    return memcmp(expected, actual, (size_t)len);
}

static int cmd_demo(const char *mountpoint)
{
    DIR *dir;
    struct dirent *entry;
    uint64_t ok = 0;
    uint64_t failed = 0;

    dir = opendir(mountpoint);
    if (!dir) {
        perror(mountpoint);
        return EXIT_FAILURE;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[4096];
        int written;

        if (entry->d_name[0] == '.')
            continue;

        written = snprintf(path, sizeof(path), "%s/%s", mountpoint, entry->d_name);
        if (written <= 0 || written >= (int)sizeof(path)) {
            failed++;
            continue;
        }

        if (write_and_read_number(path, random_u64()) == 0) {
            ok++;
        } else {
            fprintf(stderr, "Verification failed for %s\n", path);
            failed++;
        }
    }

    closedir(dir);
    printf("Demo result: ok=%" PRIu64 " failed=%" PRIu64 "\n", ok, failed);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    const char *mountpoint = DEFAULT_MOUNT_POINT;

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "demo") == 0) {
        if (argc >= 3)
            mountpoint = argv[2];
        return cmd_demo(mountpoint);
    }
    if (strcmp(argv[1], "zero") == 0) {
        if (argc >= 3)
            mountpoint = argv[2];
        return cmd_zero(mountpoint);
    }
    if (strcmp(argv[1], "erase") == 0) {
        if (argc >= 3)
            mountpoint = argv[2];
        return cmd_erase(mountpoint);
    }
    if (strcmp(argv[1], "hashes") == 0) {
        if (argc >= 3)
            mountpoint = argv[2];
        return cmd_hashes(mountpoint);
    }
    if (strcmp(argv[1], "mapping") == 0 && argc >= 3) {
        if (argc >= 4)
            mountpoint = argv[3];
        return cmd_mapping(mountpoint, argv[2]);
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
