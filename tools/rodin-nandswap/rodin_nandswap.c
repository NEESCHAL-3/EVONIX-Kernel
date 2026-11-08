// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <mntent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/swap.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>
#define Z "/sys/block/zram0/"
#define CG "/dev/memcg/"
#define BACKING "/data/nandswap/swapfile"
#define F2FS_MAGIC 0xF2F52010
#define F2FS_SET_PIN _IOW(0xf5, 13, uint32_t)
#define F2FS_GET_PIN _IOR(0xf5, 14, uint32_t)
struct profile {
    unsigned ram_gb;
    unsigned disk_mb;
    unsigned swappiness;
    unsigned increase;
    unsigned limit;
    bool enable;
    char compressor[32];
};
static int prop(const char *k, const char *v) { return __system_property_set(k, v); }
static void getprop(const char *k, char *v) {
    v[0] = 0;
    __system_property_get(k, v);
}
static int readtext(const char *p, char *b, size_t cap) {
    int f = open(p, O_RDONLY | O_CLOEXEC);
    if (f < 0)
        return -1;
    size_t used = 0;
    while (used + 1 < cap) {
        ssize_t n = read(f, b + used, cap - used - 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            int e = errno;
            close(f);
            errno = e;
            return -1;
        }
        if (!n)
            break;
        used += (size_t)n;
    }
    b[used] = 0;
    close(f);
    return 0;
}
static int put(const char *p, const char *v) {
    int f = open(p, O_WRONLY | O_CLOEXEC);
    if (f < 0)
        return -1;
    size_t len = strlen(v);
    ssize_t n;
    do {
        n = write(f, v, len);
    } while (n < 0 && errno == EINTR);
    int e = errno;
    close(f);
    errno = e;
    if (n != (ssize_t)len) {
        if (n >= 0)
            errno = EIO;
        return -1;
    }
    return 0;
}
static int putnum(const char *p, uint64_t n) {
    char b[40];
    snprintf(b, sizeof(b), "%" PRIu64, n);
    return put(p, b);
}
static int native(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (!pid) {
        execv(argv[0], argv);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        errno = EIO;
        return -1;
    }
    return 0;
}
static int tag(const char *scope, const char *name, char *out, size_t cap) {
    char start[80], end[80];
    snprintf(start, sizeof(start), "<%s>", name);
    snprintf(end, sizeof(end), "</%s>", name);
    const char *a = strstr(scope, start);
    if (!a)
        return -1;
    a += strlen(start);
    const char *b = strstr(a, end);
    if (!b || (size_t)(b - a) >= cap)
        return -1;
    memcpy(out, a, b - a);
    out[b - a] = 0;
    return 0;
}
static int number(const char *s, unsigned *v) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || end == s || *end || n > UINT32_MAX)
        return -1;
    *v = n;
    return 0;
}
static int numeric_tag(const char *s, const char *n, unsigned *v) {
    char b[40];
    return tag(s, n, b, sizeof(b)) || number(b, v) ? -1 : 0;
}
static int profile(uint64_t kb, struct profile *p) {
    char xml[16384], section[4096], zram[2048], opt[1024], name[32];
    unsigned enable;
    if (kb < 6ULL * 1024 * 1024 || kb > 16ULL * 1024 * 1024) {
        errno = EINVAL;
        return -1;
    }
    p->ram_gb = kb <= 8ULL * 1024 * 1024 ? 8 : kb <= 12ULL * 1024 * 1024 ? 12 : 16;
    snprintf(name, sizeof(name), "memory_%ug", p->ram_gb);
    if (readtext("/odm/etc/oplus_mm_config.xml", xml, sizeof(xml)) ||
        tag(xml, name, section, sizeof(section)) || tag(section, "zram0", zram, sizeof(zram)) ||
        tag(section, "zram_opt", opt, sizeof(opt)) ||
        numeric_tag(zram, "disk_size_mb", &p->disk_mb) ||
        numeric_tag(zram, "increase_limit", &p->increase) ||
        numeric_tag(zram, "limit_size", &p->limit) ||
        numeric_tag(zram, "hybridswap_enable", &enable) ||
        numeric_tag(opt, "vm_swappiness", &p->swappiness) ||
        tag(zram, "comp_algorithm", p->compressor, sizeof(p->compressor))) {
        errno = EINVAL;
        return -1;
    }
    p->enable = enable == 1;
    if (p->disk_mb < 1024 || p->disk_mb > p->ram_gb * 1024 || p->swappiness > 200 ||
        p->increase > p->disk_mb || p->limit > p->disk_mb || enable > 1 ||
        strspn(p->compressor, "abcdefghijklmnopqrstuvwxyz0123456789_-") != strlen(p->compressor)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
static int active(void) {
    char b[64];
    if (readtext(Z "disksize", b, sizeof(b)))
        return -1;
    return strtoull(b, NULL, 10) > 0;
}
static int enabled(void) {
    char b[256];
    return !readtext(Z "hybridswap_core_enable", b, sizeof(b)) &&
           strstr(b, "hybridswap enable reclaim_in enable");
}
static int fail(unsigned code, const char *step) {
    int e = errno;
    char b[40];
    snprintf(b, sizeof(b), "%u", code);
    prop("persist.sys.oplus.nandswap.err", b);
    prop("persist.sys.oplus.nandswap.condition", "false");
    prop("persist.sys.oplus.hybridswap_app_memcg", "false");
    fprintf(stderr, "NANDSwap failed at %s: %s (%d)\n", step, strerror(e), e);
    return 1;
}
static int tunables(struct profile *p) {
    struct {
        const char *path, *value;
    } list[] = {{CG "apps/memory.name", "apps"},
                {CG "apps/memory.app_score", "300"},
                {CG "system/memory.name", "systemserver"},
                {CG "system/memory.app_score", "500"},
                {CG "memory.name", "root"},
                {CG "memory.app_score", "400"},
                {CG "memory.swapd_memcgs_param", "3 0 99 0 0 0 100 399 60 0 0 400 499 50 0 0"},
                {CG "memory.avail_buffers", "2200 1800 2200 1536"},
                {CG "memory.swapd_max_reclaim_size", "50"},
                {CG "memory.swapd_shrink_parameter", "1000 50"},
                {CG "memory.max_skip_interval", "5000"},
                {CG "memory.reclaim_exceed_sleep_ms", "50"},
                {CG "memory.cpuload_threshold", "60"},
                {CG "memory.max_reclaimin_size_mb", "30"},
                {CG "memory.zram_wm_ratio", "80"},
                {CG "memory.empty_round_skip_interval", "512"},
                {CG "memory.empty_round_check_threshold", "20"}};
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++)
        if (put(list[i].path, list[i].value)) {
            fprintf(stderr, "control: %s\n", list[i].path);
            return -1;
        }
    if (putnum(CG "memory.zram_critical_threshold", p->disk_mb - 128) ||
        putnum(Z "hybridswap_zram_increase", p->increase))
        return -1;
    return 0;
}
static int backing(uint64_t bytes) {
    struct statfs fs;
    if (statfs("/data/nandswap", &fs))
        return -1;
    if ((unsigned long)fs.f_type != F2FS_MAGIC && (unsigned long)fs.f_type != 0xef53) {
        errno = ENOTSUP;
        return -1;
    }
    int fd = open(BACKING, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    int loop = -1;
    bool bound = false, attached = false;
    char path[64];
    int result = -1;
    const char *step = "pin";
    if ((unsigned long)fs.f_type == F2FS_MAGIC) {
        uint32_t v = 1;
        if (ioctl(fd, F2FS_SET_PIN, &v) < 0)
            goto out;
    }
    step = "allocate";
    if (fallocate(fd, 0, 0, (off_t)bytes) || fsync(fd))
        goto out;
    if ((unsigned long)fs.f_type == F2FS_MAGIC) {
        long flags = 0;
        if (ioctl(fd, FS_IOC_GETFLAGS, &flags) || !(flags & FS_NOCOW_FL)) {
            errno = EIO;
            goto out;
        }
    }
    step = "loop control";
    int ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (ctl < 0)
        goto out;
    int index = ioctl(ctl, LOOP_CTL_GET_FREE);
    int e = errno;
    close(ctl);
    errno = e;
    if (index < 0)
        goto out;
    snprintf(path, sizeof(path), "/dev/block/loop%d", index);
    loop = open(path, O_RDWR | O_CLOEXEC);
    if (loop < 0)
        goto out;
    step = "attach loop";
    if (ioctl(loop, LOOP_SET_FD, fd))
        goto out;
    attached = true;
    step = "loop status";
    struct loop_info64 info = {0};
    info.lo_flags = LO_FLAGS_AUTOCLEAR;
    snprintf((char *)info.lo_file_name, LO_NAME_SIZE, "%s", BACKING);
    if (ioctl(loop, LOOP_SET_STATUS64, &info))
        goto out;
    step = "loop block size";
    if (ioctl(loop, LOOP_SET_BLOCK_SIZE, (unsigned long)fs.f_bsize))
        goto out;
    step = "loop direct IO";
    if (ioctl(loop, LOOP_SET_DIRECT_IO, 1))
        goto out;
    step = "loop readback";
    struct loop_info64 actual;
    if (ioctl(loop, LOOP_GET_STATUS64, &actual) || !(actual.lo_flags & LO_FLAGS_DIRECT_IO)) {
        errno = EIO;
        goto out;
    }
    step = "bind kernel";
    if (put(Z "hybridswap_loop_device", path))
        goto out;
    bound = true;
    char verify[256];
    if (readtext(Z "backing_dev", verify, sizeof(verify)) || strncmp(verify, path, strlen(path))) {
        errno = EIO;
        goto out;
    }
    result = 0;
out:
    e = errno;
    if (result)
        fprintf(stderr, "backing error step=%s fd=%d loop=%d bound=%d: %s\n", step, fd, loop, bound,
                strerror(e));
    if (loop >= 0) {
        if (attached && !bound)
            ioctl(loop, LOOP_CLR_FD, 0);
        close(loop);
    }
    close(fd);
    if (result && !bound)
        unlink(BACKING);
    errno = e;
    return result;
}
int main(int argc, char **argv) {
    char b[4096];
    uint64_t kb = 0;
    if (readtext("/proc/meminfo", b, sizeof(b)) || sscanf(b, "MemTotal: %" SCNu64, &kb) != 1)
        return fail(1008, "memory size");
    bool audit = argc == 2 && !strcmp(argv[1], "audit");
    bool simulated = argc == 3 && !strcmp(argv[1], "profile-check");
    if (simulated) {
        char *end;
        kb = strtoull(argv[2], &end, 10);
        if (!kb || *end)
            return 2;
    }
    if (!audit && !simulated && argc != 1)
        return 2;
    struct profile p = {0};
    if (profile(kb, &p))
        return (audit || simulated) ? 2 : fail(1008, "memory profile");
    printf("profile=%uGB zram_mb=%u compressor=%s swappiness=%u\n", p.ram_gb, p.disk_mb,
           p.compressor, p.swappiness);
    if (audit || simulated) {
        if (audit) {
            if (readtext(Z "backing_dev", b, sizeof(b)))
                return 2;
            printf("backing=%s", b);
            printf("core_enabled=%d\n", enabled());
        }
        return 0;
    }
    int journal = open("/data/nandswap/native-init.log",
                       O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (journal >= 0) {
        dup2(journal, STDERR_FILENO);
        close(journal);
    }
    if (access(Z "hybridswap_core_enable", F_OK))
        return fail(1004, "native kernel backend");
    if (enabled()) {
        puts("Native NANDSwap already active; no reinitialization.");
        return 0;
    }
    // Honour the ColorOS RAM Expansion selection; publish success only after binding.
    char selected[PROP_VALUE_MAX], flag[PROP_VALUE_MAX];
    getprop("persist.sys.oplus.nandswap.swapsize", selected);
    getprop("persist.sys.oplus.nandswap", flag);
    unsigned gb = 0;
    bool requested = !strcmp(flag, "true") && p.enable;
    if (requested && !selected[0]) {
        char level[PROP_VALUE_MAX];
        unsigned index = 2;
        getprop("persist.sys.oplus.nandswap.lvl", level);
        if (level[0] && (number(level, &index) || index > 2))
            return fail(1006, "RAM expansion level");
        snprintf(selected, sizeof(selected), "%u", (unsigned[]){4, 6, 8}[index]);
        if (prop("persist.sys.oplus.nandswap.swapsize", selected))
            return fail(1006, "default expansion choice");
    }
    if (requested && (number(selected, &gb) || (gb != 4 && gb != 6 && gb != 8)))
        return fail(1006, "RAM expansion selection");
    int a = active();
    if (a < 0)
        return fail(1008, "swap state");
    if (!a) {
        if (put(Z "comp_algorithm", p.compressor) ||
            putnum(Z "disksize", (uint64_t)p.disk_mb * 1024 * 1024))
            return fail(1008, "ZRAM setup");
        char *mk[] = {"/system/bin/mkswap", "/dev/block/zram0", NULL};
        if (native(mk))
            return fail(1008, "swap header");
    }
    if (swapon("/dev/block/zram0", SWAP_FLAG_PREFER | (32758 << SWAP_FLAG_PRIO_SHIFT)) &&
        errno != EBUSY)
        return fail(1008, "activate ZRAM");

    if (!requested) {
        prop("persist.sys.oplus.nandswap.swapsize.curr", "0");
        prop("persist.sys.oplus.nandswap.err", "0");
        prop("persist.sys.oplus.nandswap.condition", "false");
        prop("persist.sys.oplus.hybridswap_app_memcg", "false");
        puts("RAM Expansion off; native ZRAM active.");
        return 0;
    }
    if (tunables(&p))
        return fail(1008, "native reclaim controls");
    struct statvfs space;
    if (statvfs("/data/nandswap", &space))
        return fail(1006, "free storage");
    uint64_t bytes = (uint64_t)gb * 1024 * 1024 * 1024 / 2;
    uint64_t available = (uint64_t)space.f_bavail * space.f_frsize;
    if (available < bytes + 15ULL * 1024 * 1024 * 1024) {
        errno = ENOSPC;
        return fail(1006, "storage reserve");
    }
    if (backing(bytes))
        return fail(1003, "pinned direct-I/O backing");
    if (put(Z "hybridswap_enable", "1") || !enabled()) {
        errno = EIO;
        return fail(1008, "enable native reclaim");
    }
    if (prop("persist.sys.oplus.nandswap.err", "0") ||
        prop("persist.sys.oplus.nandswap.condition", "true") ||
        prop("persist.sys.oplus.hybridswap_app_memcg", "true") ||
        prop("persist.sys.oplus.hybridswap_app_uid_memcg", "true"))
        return fail(1008, "publish verified state");
    if (prop("persist.sys.oplus.nandswap.swapsize.curr", selected) ||
        prop("persist.sys.oplus.nandswap.cfg", "4,6,8") ||
        prop("persist.sys.oplus.nandswap.storage.min", "15"))
        return fail(1008, "publish capacity");
    puts("Native NANDSwap enabled; pinned direct-I/O backing verified.");
    return 0;
}
