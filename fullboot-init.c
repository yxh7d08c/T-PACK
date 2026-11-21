typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;

#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_openat 257
#define SYS_mkdir 83
#define SYS_mount 165
#define SYS_chdir 80
#define SYS_chroot 161
#define SYS_execve 59
#define SYS_fork 57
#define SYS_nanosleep 35
#define SYS_init_module 175
#define SYS_exit 60

#define AT_FDCWD -100
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_NOCTTY 0400
#define O_NONBLOCK 04000
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 32
#define MODULE_MAX (16UL * 1024UL * 1024UL)

struct timespec {
    long tv_sec;
    long tv_nsec;
};

static int fd_tty0 = -1;
static int fd_tty1 = -1;
static int fd_kmsg = -1;

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a;
    register long rsi __asm__("rsi") = b;
    register long rdx __asm__("rdx") = c;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "+a"(rax)
                     : "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return rax;
}

static long sys3(long n, long a, long b, long c)
{
    return sys6(n, a, b, c, 0, 0, 0);
}

static long sys2(long n, long a, long b)
{
    return sys6(n, a, b, 0, 0, 0, 0);
}

static long sys1(long n, long a)
{
    return sys6(n, a, 0, 0, 0, 0, 0);
}

static size_t cstr_len(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void write_fd(int fd, const char *s, size_t n)
{
    if (fd >= 0)
        sys3(SYS_write, fd, (long)s, n);
}

static void log_msg(const char *s)
{
    size_t n = cstr_len(s);
    write_fd(1, s, n);
    write_fd(2, s, n);
    write_fd(fd_tty0, s, n);
    write_fd(fd_tty1, s, n);
    write_fd(fd_kmsg, s, n);
}

static void log_hex(unsigned long v)
{
    static const char h[] = "0123456789abcdef";
    char buf[19];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++)
        buf[2 + i] = h[(v >> ((15 - i) * 4)) & 0xf];
    buf[18] = '\n';
    write_fd(1, buf, sizeof(buf));
    write_fd(2, buf, sizeof(buf));
    write_fd(fd_tty0, buf, sizeof(buf));
    write_fd(fd_tty1, buf, sizeof(buf));
    write_fd(fd_kmsg, buf, sizeof(buf));
}

static int open_ro(const char *path)
{
    return (int)sys6(SYS_openat, AT_FDCWD, (long)path, O_RDONLY, 0, 0, 0);
}

static int open_wo(const char *path)
{
    return (int)sys6(SYS_openat, AT_FDCWD, (long)path, O_WRONLY | O_NOCTTY | O_NONBLOCK, 0, 0, 0);
}

static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    sys2(SYS_nanosleep, (long)&ts, 0);
}

static void mkdir_one(const char *path)
{
    long r = sys2(SYS_mkdir, (long)path, 0755);
    (void)r;
}

static long mount_one(const char *src, const char *target, const char *fstype, unsigned long flags, const char *data)
{
    long r = sys6(SYS_mount, (long)src, (long)target, (long)fstype, flags, (long)data, 0);
    if (r < 0) {
        log_msg("HYC3-FULLBOOT: mount failed ");
        log_msg(target);
        log_msg(" ");
        log_hex((unsigned long)r);
    }
    return r;
}

static long load_module_file(const char *path, const char *params, int required)
{
    char *image;
    int fd;
    long n;
    unsigned long total = 0;

    log_msg("HYC3-FULLBOOT: loading module ");
    log_msg(path);
    log_msg("\n");

    image = (char *)sys6(SYS_mmap, 0, MODULE_MAX, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)image < 0) {
        log_msg("HYC3-FULLBOOT: mmap failed ");
        log_hex((unsigned long)image);
        return (long)image;
    }

    fd = open_ro(path);
    if (fd < 0) {
        log_msg("HYC3-FULLBOOT: open module failed ");
        log_hex((unsigned long)fd);
        sys2(SYS_munmap, (long)image, MODULE_MAX);
        return fd;
    }

    while (total < MODULE_MAX) {
        n = sys3(SYS_read, fd, (long)(image + total), MODULE_MAX - total);
        if (n == 0)
            break;
        if (n < 0) {
            log_msg("HYC3-FULLBOOT: read module failed ");
            log_hex((unsigned long)n);
            sys1(SYS_close, fd);
            sys2(SYS_munmap, (long)image, MODULE_MAX);
            return n;
        }
        total += (unsigned long)n;
    }
    sys1(SYS_close, fd);

    log_msg("HYC3-FULLBOOT: module bytes ");
    log_hex(total);

    n = sys3(SYS_init_module, (long)image, total, (long)params);
    sys2(SYS_munmap, (long)image, MODULE_MAX);
    if (n < 0) {
        log_msg("HYC3-FULLBOOT: init_module failed ");
        log_msg(path);
        log_msg(" ");
        log_hex((unsigned long)n);
        if (required)
            return n;
    } else {
        log_msg("HYC3-FULLBOOT: init_module success ");
        log_msg(path);
        log_msg("\n");
    }
    return 0;
}

static int wait_for_path(const char *path, int tries, long delay_ms)
{
    int i;
    for (i = 0; i < tries; i++) {
        int fd = open_ro(path);
        if (fd >= 0) {
            sys1(SYS_close, fd);
            return 0;
        }
        if ((i % 20) == 0) {
            log_msg("HYC3-FULLBOOT: waiting for ");
            log_msg(path);
            log_msg("\n");
        }
        sleep_ms(delay_ms);
    }
    log_msg("HYC3-FULLBOOT: timed out waiting for ");
    log_msg(path);
    log_msg("\n");
    return -1;
}

static void idle_forever(void)
{
    unsigned long i = 0;
    for (;;) {
        __asm__ volatile("pause" ::: "memory");
        if (++i == 0x40000000UL) {
            log_msg("HYC3-FULLBOOT: idle after failure\n");
            i = 0;
        }
    }
}

void _start(void)
{
    const char *init_argv[] = { "/sbin/init", 0 };
    const char *init_envp[] = {
        "HOME=/",
        "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
        "TERM=linux",
        0,
    };
    long r;

    fd_tty0 = open_wo("/dev/ttyS0");
    fd_tty1 = open_wo("/dev/ttyS1");
    fd_kmsg = open_wo("/dev/kmsg");

    log_msg("HYC3-FULLBOOT: init started\n");
    load_module_file("/tdcall_module.ko", "", 0);

    mkdir_one("/proc");
    mkdir_one("/sys");
    mkdir_one("/dev");
    mkdir_one("/newroot");

    mount_one("proc", "/proc", "proc", 0, "");
    mount_one("sysfs", "/sys", "sysfs", 0, "");
    mount_one("devtmpfs", "/dev", "devtmpfs", 0, "mode=0755");

    load_module_file("/mods/libcrc32c.ko", "", 0);
    load_module_file("/mods/crc32c-intel.ko", "", 0);
    load_module_file("/mods/sd_mod.ko", "", 0);

    r = sys1(SYS_fork, 0);
    if (r == 0) {
        load_module_file("/mods/vmw_pvscsi.ko", "", 0);
        sys1(SYS_exit, 0);
    }
    log_msg("HYC3-FULLBOOT: storage probe forked\n");

    if (wait_for_path("/dev/sda1", 240, 250) < 0)
        idle_forever();

    load_module_file("/mods/xfs.ko", "", 0);

    r = mount_one("/dev/sda1", "/newroot", "xfs", 0, "");
    if (r < 0)
        idle_forever();

    mount_one("proc", "/newroot/proc", "proc", 0, "");
    mount_one("sysfs", "/newroot/sys", "sysfs", 0, "");
    mount_one("devtmpfs", "/newroot/dev", "devtmpfs", 0, "mode=0755");

    log_msg("HYC3-FULLBOOT: exec real /sbin/init\n");
    sys1(SYS_chdir, (long)"/newroot");
    r = sys1(SYS_chroot, (long)".");
    if (r < 0) {
        log_msg("HYC3-FULLBOOT: chroot failed ");
        log_hex((unsigned long)r);
        idle_forever();
    }
    sys1(SYS_chdir, (long)"/");
    r = sys3(SYS_execve, (long)"/sbin/init", (long)init_argv, (long)init_envp);
    log_msg("HYC3-FULLBOOT: exec /sbin/init failed ");
    log_hex((unsigned long)r);
    idle_forever();
}
