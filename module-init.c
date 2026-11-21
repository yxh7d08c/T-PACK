typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;

#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_fsync 74
#define SYS_access 21
#define SYS_mmap 9
#define SYS_ioctl 16
#define SYS_pipe 22
#define SYS_sched_yield 24
#define SYS_nanosleep 35
#define SYS_openat 257
#define SYS_init_module 175
#define SYS_iopl 172
#define SYS_exit 60
#define SYS_fork 57
#define SYS_wait4 61
#define SYS_dup2 33
#define SYS_execve 59
#define SYS_chdir 80
#define SYS_getcwd 79
#define SYS_chroot 161
#define SYS_sync 162
#define SYS_setsid 112
#define SYS_mkdir 83
#define SYS_mount 165
#define SYS_mknod 133
#define SYS_unlink 87
#define SYS_newfstatat 262
#define SYS_kill 62
#define SYS_fcntl 72
#define SYS_getdents64 217
#define SYS_syncfs 306

#define AT_FDCWD -100
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_NOCTTY 0400
#define O_NONBLOCK 04000
#define O_DIRECTORY 0200000
#define TIOCSCTTY 0x540E
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 32
#define MS_RDONLY 1
#define MS_MOVE 8192
#define S_IFBLK 0060000
#define S_IFMT 0170000
#define F_GETFL 3
#define F_SETFL 4
#define WNOHANG 1
#define SIGKILL 9

#define MODULE_MAX (8UL * 1024UL * 1024UL)

static int rootfs_ready;
static char current_cwd[256] = "/";

struct hyc3_cpuid_leaf {
    unsigned int eax;
    unsigned int ebx;
    unsigned int ecx;
    unsigned int edx;
};

static int fd_tty0 = -1;
static int fd_tty1 = -1;
static int fd_kmsg = -1;
static int rootfs_tty_only;

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
    if (fd < 0)
        return;
    sys3(SYS_write, fd, (long)s, n);
}

static void log_msg(const char *s)
{
    size_t n = cstr_len(s);
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
    write_fd(fd_kmsg, buf, sizeof(buf));
}

static int open_ro(const char *path)
{
    return (int)sys6(SYS_openat, AT_FDCWD, (long)path, O_RDONLY, 0, 0, 0);
}

static int open_ro_nonblock(const char *path)
{
    return (int)sys6(SYS_openat, AT_FDCWD, (long)path,
                     O_RDONLY | O_NONBLOCK, 0, 0, 0);
}

static int open_rw(const char *path)
{
    return (int)sys6(SYS_openat, AT_FDCWD, (long)path, O_RDWR | O_NOCTTY, 0, 0, 0);
}

static int open_tty(const char *path)
{
    return (int)sys6(SYS_openat, AT_FDCWD, (long)path, O_RDWR, 0, 0, 0);
}

static int open_tty_nb(const char *path)
{
    return (int)sys6(SYS_openat, AT_FDCWD, (long)path,
                     O_RDWR | O_NOCTTY | O_NONBLOCK, 0, 0, 0);
}

static int open_wo(const char *path)
{
    return (int)sys6(SYS_openat, AT_FDCWD, (long)path, O_WRONLY | O_NOCTTY | O_NONBLOCK, 0, 0, 0);
}

static void mkdir_one(const char *path)
{
    sys2(SYS_mkdir, (long)path, 0755);
}

static void mount_one(const char *src, const char *target, const char *type)
{
    sys6(SYS_mount, (long)src, (long)target, (long)type, 0, (long)"", 0);
}

static void yield_many(unsigned long count)
{
    while (count--)
        sys1(SYS_sched_yield, 0);
}

struct hyc3_timespec {
    long tv_sec;
    long tv_nsec;
};

static void sleep_millis(long millis)
{
    unsigned long loops;

    if (millis <= 0)
        return;
    loops = (unsigned long)millis * 4800UL;
    if (loops < 8192UL)
        loops = 8192UL;
    yield_many(loops);
}

static void sleep_seconds(long seconds)
{
    while (seconds-- > 0)
        sleep_millis(1000);
}

static void idle_forever(void)
{
    struct hyc3_timespec ts = { 1, 0 };

    for (;;) {
        sys2(SYS_nanosleep, (long)&ts, 0);
        yield_many(1);
    }
}

static struct hyc3_cpuid_leaf cpuid_leaf(unsigned int leaf)
{
    struct hyc3_cpuid_leaf out;
    unsigned int eax = leaf;
    unsigned int ebx;
    unsigned int ecx = 0;
    unsigned int edx;

    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)
                     :
                     : "memory");
    out.eax = eax;
    out.ebx = ebx;
    out.ecx = ecx;
    out.edx = edx;
    return out;
}

static unsigned int hyc3_vm_index(void)
{
    struct hyc3_cpuid_leaf leaf = cpuid_leaf(0x40000100U);

    if (leaf.ebx == 2 && leaf.eax < 2)
        return leaf.eax;
    return 0xffffffffU;
}


#define COM1_BASE 0x3f8
#define COM2_BASE 0x2f8

static unsigned short serial_base = COM1_BASE;
static const char *serial_prompt = "hyc3-L2-1# ";

#define COM_RBR  (serial_base + 0)
#define COM_THR  (serial_base + 0)
#define COM_IER  (serial_base + 1)
#define COM_FCR  (serial_base + 2)
#define COM_LCR  (serial_base + 3)
#define COM_MCR  (serial_base + 4)
#define COM_LSR  (serial_base + 5)
#define COM_LSR_DR   0x01
#define COM_LSR_THRE 0x20

static unsigned char hyc3_inb(unsigned short port)
{
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void hyc3_outb(unsigned short port, unsigned char value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int str_has_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static char *skip_spaces(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static void refresh_cwd(void)
{
    long rc = sys2(SYS_getcwd, (long)current_cwd, sizeof(current_cwd));
    if (rc < 0) {
        current_cwd[0] = '/';
        current_cwd[1] = 0;
    }
}

static char *cd_target(char *line)
{
    char *arg = skip_spaces(line + 2);
    if (*arg == 0 || str_eq(arg, "~"))
        return "/root";
    return arg;
}

static void cstr_copy_cap(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if (cap == 0)
        return;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void cstr_append_cap(char *dst, size_t cap, const char *src)
{
    size_t i = cstr_len(dst);
    size_t j = 0;

    if (cap == 0)
        return;
    while (src[j] && i + 1 < cap)
        dst[i++] = src[j++];
    dst[i] = 0;
}

static void cstr_append_n_cap(char *dst, size_t cap, const char *src, size_t n)
{
    size_t i = cstr_len(dst);
    size_t j = 0;

    if (cap == 0)
        return;
    while (j < n && src[j] && i + 1 < cap)
        dst[i++] = src[j++];
    dst[i] = 0;
}

static void repl_write(int fd, const char *buf, size_t n);
static void repl_puts(int fd, const char *s);
static void repl_build_host_path(char *host, size_t cap, char *arg, const char *def);

static void cstr_append_ulong_dec_cap(char *dst, size_t cap, unsigned long v)
{
    char tmp[32];
    size_t n = 0;

    if (v == 0) {
        cstr_append_cap(dst, cap, "0");
        return;
    }
    while (v && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n) {
        char one[2];
        one[0] = tmp[--n];
        one[1] = 0;
        cstr_append_cap(dst, cap, one);
    }
}

static void repl_put_ulong_dec(int fd, unsigned long v)
{
    char buf[32];

    buf[0] = 0;
    cstr_append_ulong_dec_cap(buf, sizeof(buf), v);
    repl_puts(fd, buf);
}

static void repl_build_proc_path(char *dst, size_t cap, long pid, const char *suffix)
{
    cstr_copy_cap(dst, cap, "/proc/");
    cstr_append_ulong_dec_cap(dst, cap, (unsigned long)pid);
    cstr_append_cap(dst, cap, "/");
    cstr_append_cap(dst, cap, suffix);
}

static void repl_dump_file_limited(int out_fd, const char *label, const char *path, unsigned long limit)
{
    char buf[512];
    unsigned long total = 0;
    int fd;

    repl_puts(out_fd, "\n--- HYC3-DIAG ");
    repl_puts(out_fd, label);
    repl_puts(out_fd, " ");
    repl_puts(out_fd, path);
    repl_puts(out_fd, " ---\n");

    fd = open_ro_nonblock(path);
    if (fd < 0)
        fd = open_ro(path);
    if (fd < 0) {
        repl_puts(out_fd, "open failed\n");
        return;
    }

    while (total < limit) {
        unsigned long want = sizeof(buf);
        long n;

        if (want > limit - total)
            want = limit - total;
        if (want == 0)
            break;
        n = sys3(SYS_read, fd, (long)buf, want);
        if (n <= 0)
            break;
        repl_write(out_fd, buf, (size_t)n);
        total += (unsigned long)n;
    }
    if (total == limit)
        repl_puts(out_fd, "\n--- HYC3-DIAG truncated ---\n");
    repl_puts(out_fd, "\n");
    sys1(SYS_close, fd);
}

static void repl_dump_timeout_diag(int out_fd, const char *op, long pid, const char *arg)
{
    char path[128];
    char host[512];

    repl_puts(out_fd, "\n=== HYC3-DIAG timeout op=");
    repl_puts(out_fd, op);
    repl_puts(out_fd, " pid=");
    repl_put_ulong_dec(out_fd, pid > 0 ? (unsigned long)pid : 0UL);
    repl_puts(out_fd, " cwd=");
    repl_puts(out_fd, current_cwd);
    repl_puts(out_fd, " arg=");
    repl_puts(out_fd, arg ? arg : "");
    repl_puts(out_fd, " rootfs_ready=");
    repl_put_ulong_dec(out_fd, (unsigned long)rootfs_ready);
    repl_puts(out_fd, " ===\n");

    if (arg) {
        repl_build_host_path(host, sizeof(host), (char *)arg, current_cwd);
        repl_puts(out_fd, "HYC3-DIAG host_path=");
        repl_puts(out_fd, host);
        repl_puts(out_fd, "\n");
    }

    if (pid > 0) {
        repl_build_proc_path(path, sizeof(path), pid, "wchan");
        repl_dump_file_limited(out_fd, "child-wchan", path, 256);
        repl_build_proc_path(path, sizeof(path), pid, "stack");
        repl_dump_file_limited(out_fd, "child-stack", path, 4096);
        repl_build_proc_path(path, sizeof(path), pid, "status");
        repl_dump_file_limited(out_fd, "child-status", path, 4096);
        repl_build_proc_path(path, sizeof(path), pid, "io");
        repl_dump_file_limited(out_fd, "child-io", path, 1024);
    }

    repl_dump_file_limited(out_fd, "mounts", "/proc/mounts", 4096);
    repl_dump_file_limited(out_fd, "diskstats", "/proc/diskstats", 4096);
    repl_dump_file_limited(out_fd, "vda-stat", "/sys/block/vda/stat", 1024);
    repl_dump_file_limited(out_fd, "vda-inflight", "/sys/block/vda/inflight", 1024);
    repl_dump_file_limited(out_fd, "interrupts", "/proc/interrupts", 8192);
    repl_dump_file_limited(out_fd, "softirqs", "/proc/softirqs", 8192);
}

static void rootfs_pop_path_component(char *path)
{
    size_t len = cstr_len(path);

    while (len > 1 && path[len - 1] == '/')
        len--;
    while (len > 1 && path[len - 1] != '/')
        len--;
    if (len <= 1) {
        path[0] = '/';
        path[1] = 0;
    } else {
        path[len] = 0;
    }
}

static void rootfs_append_path_component(char *dst, size_t cap,
                                         const char *src, size_t n)
{
    if (!str_eq(dst, "/"))
        cstr_append_cap(dst, cap, "/");
    cstr_append_n_cap(dst, cap, src, n);
}

static void rootfs_normalize_abs_path(char *dst, size_t cap, const char *raw)
{
    const char *p = raw;

    cstr_copy_cap(dst, cap, "/");
    while (*p) {
        const char *part;
        size_t n = 0;

        while (*p == '/')
            p++;
        part = p;
        while (part[n] && part[n] != '/')
            n++;
        if (n == 0)
            break;
        if (n == 1 && part[0] == '.') {
            /* skip */
        } else if (n == 2 && part[0] == '.' && part[1] == '.') {
            rootfs_pop_path_component(dst);
        } else {
            rootfs_append_path_component(dst, cap, part, n);
        }
        p = part + n;
    }
}

static void rootfs_build_host_path(char *dst, size_t cap, const char *path)
{
    cstr_copy_cap(dst, cap, "/sysroot");
    if (path[0] != '/')
        cstr_append_cap(dst, cap, "/");
    cstr_append_cap(dst, cap, path);
}

static void rootfs_build_next_cwd(char *dst, size_t cap, const char *target)
{
    char raw[256];

    if (target[0] == '/') {
        rootfs_normalize_abs_path(dst, cap, target);
        return;
    }

    if (current_cwd[0] != '/')
        cstr_copy_cap(raw, sizeof(raw), "/");
    else
        cstr_copy_cap(raw, sizeof(raw), current_cwd);

    if (!str_eq(raw, "/") && raw[cstr_len(raw) - 1] != '/')
        cstr_append_cap(raw, sizeof(raw), "/");
    cstr_append_cap(raw, sizeof(raw), target);
    rootfs_normalize_abs_path(dst, cap, raw);
}

static long rootfs_change_cwd(char *line)
{
    char next[256];

    /*
     * In real-rootfs shell mode PID 1 still runs outside the chroot and
     * commands are executed in short-lived children.  Do not chdir PID 1 into
     * /sysroot here: a slow or stuck rootfs lookup would freeze the console.
     * Keep cd as REPL state and let ls/external commands validate the path.
     */
    rootfs_build_next_cwd(next, sizeof(next), cd_target(line));
    if (next[0] != '/')
        return -1;
    cstr_copy_cap(current_cwd, sizeof(current_cwd), next);
    return 0;
}


static void serial_putc(char c)
{
    unsigned long spin;

    if (c == '\n')
        serial_putc('\r');

    for (spin = 0; spin < 1000000UL; spin++) {
        if (hyc3_inb(COM_LSR) & COM_LSR_THRE)
            break;
        __asm__ volatile("pause" ::: "memory");
    }
    hyc3_outb(COM_THR, (unsigned char)c);
}

static void serial_prepare_raw(void);

static void serial_write_buf(const char *s, size_t n)
{
    while (n--)
        serial_putc(*s++);
}

static void serial_puts(const char *s)
{
    serial_write_buf(s, cstr_len(s));
}


static void rootfs_serial_select(unsigned int vm_index)
{
    serial_base = (vm_index == 1) ? COM2_BASE : COM1_BASE;
    serial_prepare_raw();
}

static void refresh_log_fds(void)
{
    if (fd_kmsg < 0)
        fd_kmsg = open_wo("/dev/kmsg");
    if (fd_tty0 < 0)
        fd_tty0 = open_wo("/dev/ttyS0");
    if (fd_tty1 < 0)
        fd_tty1 = open_wo("/dev/ttyS1");
}

static void rootfs_log_msg(unsigned int vm_index, const char *s)
{
    size_t n = cstr_len(s);
    int fd;
    unsigned short old_base = serial_base;

    /* Mirror rootfs breadcrumbs to the VM-owned raw serial port.  L2-2 can
     * reach early userspace with kmsg quiet if it stalls before console flush. */
    if ((vm_index == 0 || vm_index == 1) && sys1(SYS_iopl, 3) >= 0) {
        serial_base = (vm_index == 1) ? COM2_BASE : COM1_BASE;
        serial_write_buf(s, n);
        serial_base = old_base;
    }

    /* Open kmsg for each rootfs breadcrumb.  Reusing the early fd across fork
     * and devtmpfs remounts made PID1 progress invisible on this TDX path. */
    fd = open_wo("/dev/kmsg");
    if (fd >= 0) {
        write_fd(fd, s, n);
        sys1(SYS_close, fd);
    }
}

static void rootfs_log_hex(unsigned int vm_index, unsigned long v)
{
    static const char h[] = "0123456789abcdef";
    char buf[20];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++)
        buf[2 + i] = h[(v >> ((15 - i) * 4)) & 0xf];
    buf[18] = '\n';
    buf[19] = 0;
    rootfs_log_msg(vm_index, buf);
}

static int proc_partitions_dumped;

static void rootfs_log_buf(unsigned int vm_index, const char *buf, size_t n)
{
    int fd;

    (void)vm_index;
    fd = open_wo("/dev/kmsg");
    if (fd >= 0) {
        sys3(SYS_write, fd, (long)buf, n);
        sys1(SYS_close, fd);
    }
}

static void dump_file_sample(unsigned int vm_index, const char *path, const char *tag)
{
    char buf[1536];
    int fd;
    long n;

    fd = open_ro(path);
    if (fd < 0) {
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: dump open failed ");
        rootfs_log_msg(vm_index, tag);
        rootfs_log_msg(vm_index, " rc ");
        rootfs_log_hex(vm_index, (unsigned long)fd);
        return;
    }
    n = sys3(SYS_read, fd, (long)buf, sizeof(buf) - 1);
    sys1(SYS_close, fd);
    if (n <= 0) {
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: dump read failed ");
        rootfs_log_msg(vm_index, tag);
        rootfs_log_msg(vm_index, " rc ");
        rootfs_log_hex(vm_index, (unsigned long)n);
        return;
    }
    buf[n] = 0;
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: dump begin ");
    rootfs_log_msg(vm_index, tag);
    rootfs_log_msg(vm_index, "\n");
    rootfs_log_buf(vm_index, buf, (size_t)n);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: dump end ");
    rootfs_log_msg(vm_index, tag);
    rootfs_log_msg(vm_index, "\n");
}

static void dump_proc_partitions(unsigned int vm_index)
{
    dump_file_sample(vm_index, "/proc/partitions", "/proc/partitions");
    dump_file_sample(vm_index, "/proc/devices", "/proc/devices");
    dump_file_sample(vm_index, "/proc/diskstats", "/proc/diskstats");
}

static void serial_prepare_raw(void)
{
    unsigned long i;

    /* Own the selected COM port from userspace.  Output-only serial works without IRQs; input
     * is polled here so it does not depend on the current L2 interrupt path. */
    hyc3_outb(COM_IER, 0x00);
    hyc3_outb(COM_FCR, 0x00);
    hyc3_outb(COM_MCR, 0x03);
    for (i = 0; i < 256; i++) {
        if (!(hyc3_inb(COM_LSR) & COM_LSR_DR))
            break;
        (void)hyc3_inb(COM_RBR);
    }
}

static int serial_getc(void)
{
    unsigned long spin = 0;

    for (;;) {
        if (hyc3_inb(COM_LSR) & COM_LSR_DR)
            return hyc3_inb(COM_RBR);
        if (++spin == 100000UL) {
            spin = 0;
            yield_many(1);
        }
        __asm__ volatile("pause" ::: "memory");
    }
}

struct hyc3_linux_dirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static void repl_write(int fd, const char *buf, size_t n)
{
    if (fd >= 0)
        write_fd(fd, buf, n);
    else
        serial_write_buf(buf, n);
}

static void repl_puts(int fd, const char *s)
{
    repl_write(fd, s, cstr_len(s));
}

static char *repl_arg_for_command(char *line, const char *cmd)
{
    size_t i;

    for (i = 0; cmd[i]; i++) {
        if (line[i] != cmd[i])
            return 0;
    }
    if (line[i] == 0)
        return line + i;
    if (line[i] != ' ' && line[i] != '\t')
        return 0;
    return skip_spaces(line + i);
}

static char *repl_skip_options(char *arg)
{
    arg = skip_spaces(arg);
    while (arg[0] == '-') {
        while (*arg && *arg != ' ' && *arg != '\t')
            arg++;
        arg = skip_spaces(arg);
    }
    return arg;
}

static void repl_copy_token(char *dst, size_t cap, char *arg, const char *def)
{
    size_t i = 0;

    arg = skip_spaces(arg);
    if (*arg == 0)
        arg = (char *)def;
    if (cap == 0)
        return;
    while (arg[i] && arg[i] != ' ' && arg[i] != '\t' && i + 1 < cap) {
        dst[i] = arg[i];
        i++;
    }
    dst[i] = 0;
}

static void repl_build_host_path(char *host, size_t cap, char *arg, const char *def)
{
    char token[256];
    char guest[256];

    repl_copy_token(token, sizeof(token), arg, def);
    if (rootfs_ready) {
        if (token[0] == 0)
            cstr_copy_cap(guest, sizeof(guest), current_cwd);
        else
            rootfs_build_next_cwd(guest, sizeof(guest), token);
        rootfs_build_host_path(host, cap, guest);
    } else {
        cstr_copy_cap(host, cap, token[0] ? token : def);
    }
}

static void repl_parent_path(char *parent, size_t cap, const char *path)
{
    size_t i = 0;
    size_t last = 0;

    if (cap == 0)
        return;
    while (path[i]) {
        if (path[i] == '/')
            last = i;
        i++;
    }
    if (last == 0) {
        cstr_copy_cap(parent, cap, "/");
        return;
    }
    if (last >= cap)
        last = cap - 1;
    for (i = 0; i < last; i++)
        parent[i] = path[i];
    parent[last] = 0;
}

static void repl_sync_path(const char *host)
{
    char parent[512];
    int dirfd;

    repl_parent_path(parent, sizeof(parent), host);
    dirfd = (int)sys6(SYS_openat, AT_FDCWD, (long)parent, O_RDONLY | O_DIRECTORY, 0, 0, 0);
    if (dirfd >= 0) {
        sys1(SYS_fsync, dirfd);
        sys1(SYS_syncfs, dirfd);
        sys1(SYS_close, dirfd);
    }
    sys1(SYS_sync, 0);
}

static void repl_touch_file(int out_fd, char *arg)
{
    char host[512];
    int fd;

    arg = skip_spaces(arg);
    if (*arg == 0) {
        repl_puts(out_fd, "touch: missing file\n");
        return;
    }
    repl_build_host_path(host, sizeof(host), arg, "");
    fd = (int)sys6(SYS_openat, AT_FDCWD, (long)host, O_WRONLY | O_CREAT, 0644, 0, 0);
    if (fd < 0) {
        repl_puts(out_fd, "touch: cannot create file\n");
        return;
    }
    sys1(SYS_close, fd);
    repl_puts(out_fd, "touch: created\n");
}

static void repl_mkdir_dir(int out_fd, char *arg)
{
    char host[512];
    long rc;

    arg = skip_spaces(arg);
    if (*arg == 0) {
        repl_puts(out_fd, "mkdir: missing directory\n");
        return;
    }
    repl_build_host_path(host, sizeof(host), arg, "");
    rc = sys2(SYS_mkdir, (long)host, 0755);
    if (rc < 0) {
        repl_puts(out_fd, "mkdir: cannot create directory\n");
        return;
    }
    repl_puts(out_fd, "mkdir: created\n");
}

typedef void (*repl_fs_op_fn)(int out_fd, char *arg);

static void repl_fs_op_async(int out_fd, char *arg, const char *op, const char *timeout_msg, repl_fs_op_fn fn)
{
    int pipefd[2];
    long rc;
    long pid;
    int status = 0;
    char buf[256];
    int done = 0;
    int i;

    rc = sys1(SYS_pipe, (long)pipefd);
    if (rc < 0) {
        repl_puts(out_fd, "pipe failed\n");
        return;
    }
    rc = sys2(SYS_fcntl, pipefd[0], F_GETFL);
    if (rc >= 0)
        sys3(SYS_fcntl, pipefd[0], F_SETFL, rc | O_NONBLOCK);

    pid = sys1(SYS_fork, 0);
    if (pid < 0) {
        repl_puts(out_fd, "fork failed\n");
        sys1(SYS_close, pipefd[0]);
        sys1(SYS_close, pipefd[1]);
        return;
    }
    if (pid == 0) {
        sys1(SYS_close, pipefd[0]);
        fn(pipefd[1], arg);
        sys1(SYS_close, pipefd[1]);
        sys1(SYS_exit, 0);
    }

    sys1(SYS_close, pipefd[1]);
    for (i = 0; i < 800; i++) {
        for (;;) {
            long n = sys3(SYS_read, pipefd[0], (long)buf, sizeof(buf));
            if (n > 0)
                repl_write(out_fd, buf, (size_t)n);
            else if (n == 0) {
                done = 1;
                break;
            } else {
                break;
            }
        }
        rc = sys6(SYS_wait4, pid, (long)&status, WNOHANG, 0, 0, 0);
        if (rc == pid || rc < 0) {
            done = 1;
            break;
        }
        sleep_millis(10);
    }
    if (!done) {
        repl_puts(out_fd, timeout_msg);
        repl_dump_timeout_diag(out_fd, op, pid, arg);
        sys2(SYS_kill, pid, SIGKILL);
        sys6(SYS_wait4, pid, (long)&status, WNOHANG, 0, 0, 0);
    }
    sys1(SYS_close, pipefd[0]);
}

static void repl_sync_rootfs(int out_fd)
{
    int fd = open_ro("/sysroot");

    if (fd >= 0) {
        sys1(SYS_syncfs, fd);
        sys1(SYS_close, fd);
    }
    sys1(SYS_sync, 0);
    repl_puts(out_fd, "sync: done\n");
}

static int repl_is_dot_name(const char *name)
{
    return (name[0] == '.' && name[1] == 0) ||
           (name[0] == '.' && name[1] == '.' && name[2] == 0);
}

static void repl_ls_dir(int out_fd, char *arg)
{
    char host[512];
    char buf[1024];
    long n;
    int fd;
    int any = 0;

    arg = repl_skip_options(arg);
    repl_build_host_path(host, sizeof(host), arg, current_cwd);
    fd = (int)sys6(SYS_openat, AT_FDCWD, (long)host,
                   O_RDONLY | O_DIRECTORY, 0, 0, 0);
    if (fd < 0) {
        repl_puts(out_fd, "ls: cannot open directory\n");
        return;
    }

    for (;;) {
        long bpos = 0;
        n = sys3(SYS_getdents64, fd, (long)buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            repl_puts(out_fd, "ls: getdents failed\n");
            break;
        }
        while (bpos < n) {
            struct hyc3_linux_dirent64 *d = (struct hyc3_linux_dirent64 *)(buf + bpos);
            const char *name = d->d_name;
            if (d->d_reclen == 0)
                break;
            if (!repl_is_dot_name(name)) {
                repl_puts(out_fd, name);
                repl_puts(out_fd, "\n");
                any = 1;
            }
            bpos += d->d_reclen;
        }
    }
    sys1(SYS_close, fd);
    if (!any)
        repl_puts(out_fd, "\n");
}

static void repl_ls_dir_async(int out_fd, char *arg)
{
    int pipefd[2];
    long rc;
    long pid;
    int status = 0;
    char buf[256];
    int done = 0;
    int i;

    rc = sys1(SYS_pipe, (long)pipefd);
    if (rc < 0) {
        repl_puts(out_fd, "ls: pipe failed\n");
        return;
    }
    rc = sys2(SYS_fcntl, pipefd[0], F_GETFL);
    if (rc >= 0)
        sys3(SYS_fcntl, pipefd[0], F_SETFL, rc | O_NONBLOCK);

    pid = sys1(SYS_fork, 0);
    if (pid < 0) {
        repl_puts(out_fd, "ls: fork failed\n");
        sys1(SYS_close, pipefd[0]);
        sys1(SYS_close, pipefd[1]);
        return;
    }
    if (pid == 0) {
        sys1(SYS_close, pipefd[0]);
        repl_ls_dir(pipefd[1], arg);
        sys1(SYS_close, pipefd[1]);
        sys1(SYS_exit, 0);
    }

    sys1(SYS_close, pipefd[1]);
    for (i = 0; i < 800; i++) {
        for (;;) {
            long n = sys3(SYS_read, pipefd[0], (long)buf, sizeof(buf));
            if (n > 0)
                repl_write(out_fd, buf, (size_t)n);
            else if (n == 0) {
                done = 1;
                break;
            } else {
                break;
            }
        }
        rc = sys6(SYS_wait4, pid, (long)&status, WNOHANG, 0, 0, 0);
        if (rc == pid || rc < 0) {
            done = 1;
            break;
        }
        sleep_millis(10);
    }
    if (!done) {
        repl_puts(out_fd, "\nls timed out; rootfs directory I/O is still pending\n");
        repl_dump_timeout_diag(out_fd, "ls", pid, arg);
        sys2(SYS_kill, pid, SIGKILL);
        sys6(SYS_wait4, pid, (long)&status, WNOHANG, 0, 0, 0);
    }
    sys1(SYS_close, pipefd[0]);
}

static void repl_cat_file(int out_fd, char *arg)
{
    char host[512];
    char buf[512];
    int fd;
    long n;
    unsigned long total = 0;

    arg = skip_spaces(arg);
    if (*arg == 0) {
        repl_puts(out_fd, "cat: missing file\n");
        return;
    }
    repl_build_host_path(host, sizeof(host), arg, "");
    fd = open_ro(host);
    if (fd < 0) {
        repl_puts(out_fd, "cat: cannot open file\n");
        return;
    }
    while (total < 65536UL) {
        n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            repl_puts(out_fd, "cat: read failed\n");
            break;
        }
        repl_write(out_fd, buf, (size_t)n);
        total += (unsigned long)n;
    }
    if (total >= 65536UL)
        repl_puts(out_fd, "\ncat: truncated at 64K\n");
    sys1(SYS_close, fd);
}

static void repl_cat_file_async(int out_fd, char *arg)
{
    int pipefd[2];
    long rc;
    long pid;
    int status = 0;
    char buf[256];
    int done = 0;
    int i;

    rc = sys1(SYS_pipe, (long)pipefd);
    if (rc < 0) {
        repl_puts(out_fd, "cat: pipe failed\n");
        return;
    }
    rc = sys2(SYS_fcntl, pipefd[0], F_GETFL);
    if (rc >= 0)
        sys3(SYS_fcntl, pipefd[0], F_SETFL, rc | O_NONBLOCK);

    pid = sys1(SYS_fork, 0);
    if (pid < 0) {
        repl_puts(out_fd, "cat: fork failed\n");
        sys1(SYS_close, pipefd[0]);
        sys1(SYS_close, pipefd[1]);
        return;
    }
    if (pid == 0) {
        sys1(SYS_close, pipefd[0]);
        repl_cat_file(pipefd[1], arg);
        sys1(SYS_close, pipefd[1]);
        sys1(SYS_exit, 0);
    }

    sys1(SYS_close, pipefd[1]);
    for (i = 0; i < 800; i++) {
        for (;;) {
            long n = sys3(SYS_read, pipefd[0], (long)buf, sizeof(buf));
            if (n > 0)
                repl_write(out_fd, buf, (size_t)n);
            else if (n == 0) {
                done = 1;
                break;
            } else {
                break;
            }
        }
        rc = sys6(SYS_wait4, pid, (long)&status, WNOHANG, 0, 0, 0);
        if (rc == pid || rc < 0) {
            done = 1;
            break;
        }
        sleep_millis(10);
    }
    if (!done) {
        repl_puts(out_fd, "\ncat timed out; rootfs file I/O is still pending\n");
        repl_dump_timeout_diag(out_fd, "cat", pid, arg);
        sys2(SYS_kill, pid, SIGKILL);
        sys6(SYS_wait4, pid, (long)&status, WNOHANG, 0, 0, 0);
    }
    sys1(SYS_close, pipefd[0]);
}

static int repl_builtin_command(int out_fd, char *line)
{
    char *arg;

    arg = repl_arg_for_command(line, "ls");
    if (!arg)
        arg = repl_arg_for_command(line, "/bin/ls");
    if (arg) {
        repl_ls_dir_async(out_fd, arg);
        return 1;
    }

    arg = repl_arg_for_command(line, "cat");
    if (!arg)
        arg = repl_arg_for_command(line, "/bin/cat");
    if (arg) {
        repl_cat_file_async(out_fd, arg);
        return 1;
    }

    arg = repl_arg_for_command(line, "touch");
    if (!arg)
        arg = repl_arg_for_command(line, "/bin/touch");
    if (!arg)
        arg = repl_arg_for_command(line, "/usr/bin/touch");
    if (arg) {
        repl_fs_op_async(out_fd, arg, "touch", "\ntouch timed out; rootfs file I/O is still pending\n", repl_touch_file);
        return 1;
    }

    arg = repl_arg_for_command(line, "mkdir");
    if (!arg)
        arg = repl_arg_for_command(line, "/bin/mkdir");
    if (!arg)
        arg = repl_arg_for_command(line, "/usr/bin/mkdir");
    if (arg) {
        repl_fs_op_async(out_fd, arg, "mkdir", "\nmkdir timed out; rootfs directory I/O is still pending\n", repl_mkdir_dir);
        return 1;
    }

    arg = repl_arg_for_command(line, "diag");
    if (!arg)
        arg = repl_arg_for_command(line, "hyc3diag");
    if (arg) {
        repl_dump_timeout_diag(out_fd, "manual", 0, arg);
        return 1;
    }

    if (str_eq(line, "sync") || str_eq(line, "/bin/sync") || str_eq(line, "/usr/bin/sync")) {
        repl_sync_rootfs(out_fd);
        return 1;
    }

    if (str_eq(line, "whoami") || str_eq(line, "/usr/bin/whoami")) {
        repl_puts(out_fd, "root\n");
        return 1;
    }
    if (str_eq(line, "id") || str_eq(line, "/usr/bin/id")) {
        repl_puts(out_fd, "uid=0(root) gid=0(root) groups=0(root)\n");
        return 1;
    }
    if (str_eq(line, "uname -r") || str_eq(line, "/bin/uname -r") ||
        str_eq(line, "/usr/bin/uname -r")) {
        int fd = open_ro("/proc/sys/kernel/osrelease");
        char buf[128];
        long n;
        if (fd >= 0) {
            n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
            if (n > 0)
                repl_write(out_fd, buf, (size_t)n);
            sys1(SYS_close, fd);
        } else {
            repl_puts(out_fd, "unknown\n");
        }
        return 1;
    }
    return 0;
}

static void repl_external_command(int out_fd, char *line)
{
    static char *env[] = {
        "HOME=/root",
        "PATH=/bin:/usr/bin:/sbin:/usr/sbin",
        "TERM=dumb",
        0,
    };
    int pipefd[2];
    long rc;
    long pid;
    int status = 0;
    char buf[256];
    int done = 0;
    int i;

    rc = sys1(SYS_pipe, (long)pipefd);
    if (rc < 0) {
        repl_puts(out_fd, "pipe failed\n");
        return;
    }

    rc = sys2(SYS_fcntl, pipefd[0], F_GETFL);
    if (rc >= 0)
        sys3(SYS_fcntl, pipefd[0], F_SETFL, rc | O_NONBLOCK);

    pid = sys1(SYS_fork, 0);
    if (pid < 0) {
        repl_puts(out_fd, "fork failed\n");
        sys1(SYS_close, pipefd[0]);
        sys1(SYS_close, pipefd[1]);
        return;
    }

    if (pid == 0) {
        static char *argv[] = { "/bin/sh", "-c", 0, 0 };
        int nullfd;

        argv[2] = line;
        if (rootfs_ready) {
            sys1(SYS_chroot, (long)"/sysroot");
            sys1(SYS_chdir, (long)current_cwd);
        }
        sys1(SYS_close, pipefd[0]);
        sys2(SYS_dup2, pipefd[1], 1);
        sys2(SYS_dup2, pipefd[1], 2);
        if (out_fd >= 0) {
            sys2(SYS_dup2, out_fd, 0);
        } else {
            nullfd = open_ro("/dev/null");
            if (nullfd >= 0) {
                sys2(SYS_dup2, nullfd, 0);
                if (nullfd > 2)
                    sys1(SYS_close, nullfd);
            }
        }
        if (pipefd[1] > 2)
            sys1(SYS_close, pipefd[1]);
        sys3(SYS_execve, (long)"/bin/sh", (long)argv, (long)env);
        write_fd(1, "exec /bin/sh failed\n", 20);
        sys1(SYS_exit, 127);
    }

    sys1(SYS_close, pipefd[1]);
    for (i = 0; i < 1000; i++) {
        for (;;) {
            long n = sys3(SYS_read, pipefd[0], (long)buf, sizeof(buf));
            if (n > 0)
                repl_write(out_fd, buf, (size_t)n);
            else if (n == 0) {
                done = 1;
                break;
            } else {
                break;
            }
        }
        rc = sys6(SYS_wait4, pid, (long)&status, WNOHANG, 0, 0, 0);
        if (rc == pid || rc < 0) {
            done = 1;
            break;
        }
        sleep_millis(10);
    }
    if (!done) {
        repl_puts(out_fd, "\ncommand timed out; REPL remains alive\n");
        repl_dump_timeout_diag(out_fd, "external", pid, line);
        sys2(SYS_kill, pid, SIGKILL);
        sys6(SYS_wait4, pid, (long)&status, WNOHANG, 0, 0, 0);
    }
    sys1(SYS_close, pipefd[0]);
}

static void serial_exec_command(char *line)
{
    if (line[0] == 0)
        return;
    if (str_eq(line, "help")) {
        serial_puts("Builtins: cd, pwd, clear, exit, ls, cat, touch, mkdir, sync, diag, whoami, id, uname -r. External commands run with a timeout. Try: ls /, cat /etc/os-release\n");
        return;
    }
    if (str_eq(line, "clear")) {
        serial_puts("\033[H\033[J");
        return;
    }
    if (str_eq(line, "pwd")) {
        if (!rootfs_ready)
            refresh_cwd();
        serial_puts(current_cwd);
        serial_puts("\n");
        return;
    }
    if (str_eq(line, "cd") || str_has_prefix(line, "cd ")) {
        long cd_rc;

        if (rootfs_ready)
            cd_rc = rootfs_change_cwd(line);
        else
            cd_rc = sys1(SYS_chdir, (long)cd_target(line));
        if (cd_rc < 0)
            serial_puts("cd: failed\n");
        if (!rootfs_ready)
            refresh_cwd();
        return;
    }
    if (str_eq(line, "exit")) {
        serial_puts("PID1 keeps this L2 alive; detach with Ctrl-] and stop QEMU from the host.\n");
        return;
    }
    if (repl_builtin_command(-1, line))
        return;
    repl_external_command(-1, line);
}

static void serial_read_line(char *line, size_t cap)
{
    size_t len = 0;

    for (;;) {
        int c = serial_getc();

        if (c == '\r' || c == '\n') {
            serial_putc('\n');
            line[len] = 0;
            return;
        }
        if (c == 0x7f || c == 0x08) {
            if (len > 0) {
                len--;
                serial_puts("\b \b");
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7f && len + 1 < cap) {
            line[len++] = (char)c;
            serial_putc((char)c);
        }
    }
}


static void tty_puts(int fd, const char *s)
{
    write_fd(fd, s, cstr_len(s));
}

static void tty_exec_command(int fd, char *line)
{
    if (line[0] == 0)
        return;
    if (str_eq(line, "help")) {
        tty_puts(fd, "Builtins: cd, pwd, clear, exit, ls, cat, touch, mkdir, sync, diag, whoami, id, uname -r. External commands run with a timeout. Try: ls /, cat /etc/os-release\n");
        return;
    }
    if (str_eq(line, "clear")) {
        tty_puts(fd, "\033[H\033[J");
        return;
    }
    if (str_eq(line, "pwd")) {
        if (!rootfs_ready)
            refresh_cwd();
        tty_puts(fd, current_cwd);
        tty_puts(fd, "\n");
        return;
    }
    if (str_eq(line, "cd") || str_has_prefix(line, "cd ")) {
        long cd_rc;

        if (rootfs_ready)
            cd_rc = rootfs_change_cwd(line);
        else
            cd_rc = sys1(SYS_chdir, (long)cd_target(line));
        if (cd_rc < 0)
            tty_puts(fd, "cd: failed\n");
        if (!rootfs_ready)
            refresh_cwd();
        return;
    }
    if (str_eq(line, "exit")) {
        tty_puts(fd, "PID1 keeps this L2 alive; detach with Ctrl-] and stop QEMU from the host.\n");
        return;
    }
    if (repl_builtin_command(fd, line))
        return;
    repl_external_command(fd, line);
}

static void tty_read_line(int fd, char *line, size_t cap)
{
    size_t len = 0;

    for (;;) {
        char c;
        long n = sys3(SYS_read, fd, (long)&c, 1);

        if (n <= 0) {
            yield_many(1);
            continue;
        }
        if (c == '\r' || c == '\n') {
            tty_puts(fd, "\n");
            line[len] = 0;
            return;
        }
        if (c == 0x7f || c == 0x08) {
            if (len > 0)
                len--;
            continue;
        }
        if (c >= 0x20 && c < 0x7f && len + 1 < cap)
            line[len++] = c;
    }
}

static long tty_repl(void)
{
    char line[256];
    int fd;

    log_msg("HYC3-BRINGUP: tty_repl enter\n");
    sys1(SYS_setsid, 0);
    fd = open_tty_nb("/dev/ttyS0");
    log_msg("HYC3-BRINGUP: open /dev/ttyS0 returned ");
    log_hex((unsigned long)fd);
    if (fd < 0) {
        fd = open_tty_nb("/dev/console");
        log_msg("HYC3-BRINGUP: open /dev/console returned ");
        log_hex((unsigned long)fd);
    }
    if (fd < 0)
        return fd;

    log_msg("HYC3-BRINGUP: tty_repl configuring tty\n");
    sys3(SYS_ioctl, fd, TIOCSCTTY, 0);
    sys2(SYS_dup2, fd, 0);
    sys2(SYS_dup2, fd, 1);
    sys2(SYS_dup2, fd, 2);

    tty_puts(fd, "\nHYC3 L2-1 tty console ready. Type 'help'.\nhyc3-L2-1# ");
    for (;;) {
        tty_read_line(fd, line, sizeof(line));
        tty_exec_command(fd, line);
        tty_puts(fd, "hyc3-L2-1# ");
    }
}

static long serial_repl(unsigned short base, const char *prompt)
{
    char line[256];
    long rc = sys1(SYS_iopl, 3);

    if (rc < 0)
        return rc;

    serial_base = base;
    serial_prompt = prompt;
    serial_prepare_raw();
    if (rootfs_ready) {
        sleep_millis(1500);
        serial_puts("\nHYC3 real qcow2 rootfs console ready. Type 'help'.\n");
    } else {
        serial_puts("\nHYC3 raw initramfs console ready. Type 'help'.\n");
    }
    serial_puts(serial_prompt);
    for (;;) {
        serial_read_line(line, sizeof(line));
        serial_exec_command(line);
        serial_puts(serial_prompt);
    }
}


static void start_l2_1_shell(unsigned int vm_index)
{
    static char *argv[] = { "/bin/sh", "-i", 0 };
    static char *env_l2_1[] = {
        "HOME=/root",
        "PATH=/bin:/usr/bin:/sbin:/usr/sbin",
        "TERM=linux",
        "PS1=hyc3-L2-1# ",
        0,
    };
    long pid;

    if (vm_index != 0)
        return;

    log_msg("HYC3-BRINGUP: launching interactive L2-1 shell on console/ttyS0\n");
    pid = sys1(SYS_fork, 0);
    if (pid < 0) {
        log_msg("HYC3-BRINGUP: fork shell failed ");
        log_hex((unsigned long)pid);
        return;
    }

    if (pid == 0) {
        int fd;
        long repl_rc;

        repl_rc = serial_repl(COM1_BASE, "hyc3-L2-1# ");
        log_msg("HYC3-BRINGUP: raw serial REPL unavailable ");
        log_hex((unsigned long)repl_rc);

        repl_rc = tty_repl();
        log_msg("HYC3-BRINGUP: tty REPL unavailable ");
        log_hex((unsigned long)repl_rc);

        sys1(SYS_setsid, 0);
        fd = open_tty("/dev/console");
        if (fd < 0)
            fd = open_tty("/dev/ttyS0");
        if (fd >= 0) {
            sys3(SYS_ioctl, fd, TIOCSCTTY, 0);
            sys2(SYS_dup2, fd, 0);
            sys2(SYS_dup2, fd, 1);
            sys2(SYS_dup2, fd, 2);
            if (fd > 2)
                sys1(SYS_close, fd);
            log_msg("HYC3-BRINGUP: L2-1 shell attached; parent continues module test\n");
        } else {
            log_msg("HYC3-BRINGUP: open shell tty failed ");
            log_hex((unsigned long)fd);
        }
        sys1(SYS_chdir, (long)"/");
        sys3(SYS_execve, (long)"/bin/sh", (long)argv, (long)env_l2_1);
        log_msg("HYC3-BRINGUP: exec /bin/sh failed\n");
        idle_forever();
    }

    log_msg("HYC3-BRINGUP: L2-1 shell forked; yielding before module load\n");
    yield_many(256);
}


static long load_module_from_file(const char *path, const char *params)
{
    int fd;
    char *image;
    long n;
    unsigned long total = 0;

    image = (char *)sys6(SYS_mmap, 0, MODULE_MAX, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)image < 0)
        return (long)image;

    fd = open_ro(path);
    if (fd < 0)
        return fd;

    while (total < MODULE_MAX) {
        n = sys3(SYS_read, fd, (long)(image + total), MODULE_MAX - total);
        if (n == 0)
            break;
        if (n < 0) {
            sys1(SYS_close, fd);
            return n;
        }
        total += (unsigned long)n;
    }
    sys1(SYS_close, fd);
    return sys3(SYS_init_module, (long)image, total, (long)params);
}

static long mount_fs_data(const char *src, const char *target, const char *type,
                          unsigned long flags, const char *data)
{
    if (!data)
        data = "";
    return sys6(SYS_mount, (long)src, (long)target, (long)type,
                flags, (long)data, 0);
}

static long mount_fs(const char *src, const char *target, const char *type,
                     unsigned long flags)
{
    return mount_fs_data(src, target, type, flags, "");
}

static int wait_for_path_logged(const char *path, int loops, long nsec)
{
    int i;
    int report_period = nsec > 0 ? 200 : 5000;

    for (i = 0; i < loops; i++) {
        long exists = sys2(SYS_access, (long)path, 0);
        if (exists == 0) {
            log_msg("HYC3-BRINGUP: path ready ");
            log_msg(path);
            log_msg("\n");
            return 0;
        }
        if ((i % report_period) == 0) {
            log_msg("HYC3-BRINGUP: still waiting for ");
            log_msg(path);
            log_msg("\n");
        }
        if (nsec > 0)
            sleep_millis(nsec / 1000000L);
        else
            yield_many(2048);
    }
    return -1;
}

static int wait_for_root_disk(const char *path)
{
    return wait_for_path_logged(path, 6000, 10000000L);
}

static const char *sysfs_dev_for_rootdev(const char *rootdev)
{
    if (str_eq(rootdev, "/dev/sda"))
        return "/sys/block/sda/dev";
    if (str_eq(rootdev, "/dev/sda1"))
        return "/sys/class/block/sda1/dev";
    if (str_eq(rootdev, "/dev/vda"))
        return "/sys/block/vda/dev";
    if (str_eq(rootdev, "/dev/vda1"))
        return "/sys/class/block/vda1/dev";
    return 0;
}

static const char *sysfs_uevent_for_rootdev(const char *rootdev)
{
    if (str_eq(rootdev, "/dev/sda"))
        return "/sys/block/sda/uevent";
    if (str_eq(rootdev, "/dev/sda1"))
        return "/sys/class/block/sda1/uevent";
    if (str_eq(rootdev, "/dev/vda"))
        return "/sys/block/vda/uevent";
    if (str_eq(rootdev, "/dev/vda1"))
        return "/sys/class/block/vda1/uevent";
    return 0;
}

static void dump_sysfs_block(unsigned int vm_index, const char *rootdev)
{
    const char *sysdev = sysfs_dev_for_rootdev(rootdev);
    const char *uevent = sysfs_uevent_for_rootdev(rootdev);

    if (sysdev)
        dump_file_sample(vm_index, sysdev, sysdev);
    if (uevent)
        dump_file_sample(vm_index, uevent, uevent);
}

static int parse_uint_dec(const char **pp, unsigned int *out)
{
    const char *p = *pp;
    unsigned int v = 0;
    int any = 0;

    while (*p >= '0' && *p <= '9') {
        v = v * 10U + (unsigned int)(*p - '0');
        p++;
        any = 1;
    }
    if (!any)
        return -1;
    *out = v;
    *pp = p;
    return 0;
}

static unsigned long hyc3_makedev(unsigned int major, unsigned int minor)
{
    return ((unsigned long)(minor & 0xffU)) |
           ((unsigned long)(major & 0xfffU) << 8) |
           ((unsigned long)(minor & ~0xffU) << 12) |
           ((unsigned long)(major & ~0xfffU) << 32);
}


struct hyc3_stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned long st_nlink;
    unsigned int st_mode;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned int __pad0;
    unsigned long st_rdev;
    long st_size;
    long st_blksize;
    long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    long __unused[3];
};

static unsigned int hyc3_dev_major(unsigned long dev)
{
    return (unsigned int)(((dev >> 8) & 0xfffUL) | ((dev >> 32) & ~0xfffUL));
}

static unsigned int hyc3_dev_minor(unsigned long dev)
{
    return (unsigned int)((dev & 0xffUL) | ((dev >> 12) & ~0xffUL));
}

static int block_node_matches(const char *path, unsigned long expected_dev,
                              unsigned int vm_index)
{
    struct hyc3_stat st;
    long rc;
    int is_block;

    st.st_mode = 0;
    st.st_rdev = 0;
    rc = sys6(SYS_newfstatat, AT_FDCWD, (long)path, (long)&st, 0, 0, 0);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: stat rootdev rc ");
    rootfs_log_hex(vm_index, (unsigned long)rc);
    if (rc < 0)
        return 0;

    rootfs_log_msg(vm_index, "HYC3-BRINGUP: stat rootdev mode ");
    rootfs_log_hex(vm_index, st.st_mode);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: stat rootdev rdev ");
    rootfs_log_hex(vm_index, st.st_rdev);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: stat rootdev rdev major ");
    rootfs_log_hex(vm_index, hyc3_dev_major(st.st_rdev));
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: stat rootdev rdev minor ");
    rootfs_log_hex(vm_index, hyc3_dev_minor(st.st_rdev));

    is_block = ((st.st_mode & S_IFMT) == S_IFBLK);
    if (!is_block || st.st_rdev != expected_dev) {
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: rootdev node mismatch expected ");
        rootfs_log_hex(vm_index, expected_dev);
        return 0;
    }
    return 1;
}

static int read_sysfs_dev(const char *path, unsigned int *major, unsigned int *minor)
{
    char buf[64];
    const char *p;
    int fd;
    long n;

    fd = open_ro(path);
    if (fd < 0)
        return fd;
    n = sys3(SYS_read, fd, (long)buf, sizeof(buf) - 1);
    sys1(SYS_close, fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    p = buf;
    if (parse_uint_dec(&p, major) < 0)
        return -1;
    if (*p != ':')
        return -1;
    p++;
    if (parse_uint_dec(&p, minor) < 0)
        return -1;
    return 0;
}

static int ensure_block_node_from_sysfs(const char *rootdev, unsigned int vm_index)
{
    const char *sysdev = sysfs_dev_for_rootdev(rootdev);
    unsigned int major = 0;
    unsigned int minor = 0;
    unsigned long dev;
    int i;
    long rc = -1;

    if (!sysdev)
        return -1;

    for (i = 0; i < 3000; i++) {
        rc = read_sysfs_dev(sysdev, &major, &minor);
        if (rc == 0 && (major != 0 || minor != 0))
            break;
        if ((i % 200) == 0) {
            rootfs_log_msg(vm_index, "HYC3-BRINGUP: waiting sysfs dev ");
            rootfs_log_msg(vm_index, sysdev);
            rootfs_log_msg(vm_index, " rc ");
            rootfs_log_hex(vm_index, (unsigned long)rc);
            rootfs_log_msg(vm_index, "HYC3-BRINGUP: sysfs major ");
            rootfs_log_hex(vm_index, major);
            rootfs_log_msg(vm_index, "HYC3-BRINGUP: sysfs minor ");
            rootfs_log_hex(vm_index, minor);
        }
        sleep_millis(10);
    }
    if (rc < 0 || (major == 0 && minor == 0))
        return -1;

    rootfs_log_msg(vm_index, "HYC3-BRINGUP: sysfs rootdev major ");
    rootfs_log_hex(vm_index, major);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: sysfs rootdev minor ");
    rootfs_log_hex(vm_index, minor);

    dev = hyc3_makedev(major, minor);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: mknod rootdev dev ");
    rootfs_log_hex(vm_index, dev);
    rc = sys3(SYS_mknod, (long)rootdev, S_IFBLK | 0600, (long)dev);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: mknod rootdev rc ");
    rootfs_log_hex(vm_index, (unsigned long)rc);
    if (block_node_matches(rootdev, dev, vm_index))
        return 0;

    rootfs_log_msg(vm_index, "HYC3-BRINGUP: recreating mismatched rootdev node\n");
    sys1(SYS_unlink, (long)rootdev);
    rc = sys3(SYS_mknod, (long)rootdev, S_IFBLK | 0600, (long)dev);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: remknod rootdev rc ");
    rootfs_log_hex(vm_index, (unsigned long)rc);
    if (!block_node_matches(rootdev, dev, vm_index))
        return (rc < 0) ? (int)rc : -1;
    return 0;
}

static int wait_for_block_open_silent(const char *path, int loops)
{
    int i;

    for (i = 0; i < loops; i++) {
        int fd = open_ro_nonblock(path);
        if (fd >= 0) {
            sys1(SYS_close, fd);
            return 0;
        }
        sleep_millis(10);
    }
    return -1;
}


static int skip_partition_space(const char **pp)
{
    const char *p = *pp;

    while (*p == ' ' || *p == '\t')
        p++;
    *pp = p;
    return 0;
}

static int name_eq_span(const char *p, const char *end, const char *name)
{
    size_t i = 0;

    while (p + i < end && name[i]) {
        if (p[i] != name[i])
            return 0;
        i++;
    }
    return name[i] == 0 && p + i == end;
}

static int read_proc_partition_dev_once(const char *name,
                                        unsigned int *major,
                                        unsigned int *minor)
{
    char buf[4096];
    const char *line;
    int fd;
    long n;

    fd = open_ro("/proc/partitions");
    if (fd < 0)
        return fd;
    n = sys3(SYS_read, fd, (long)buf, sizeof(buf) - 1);
    sys1(SYS_close, fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;

    line = buf;
    while (*line) {
        const char *p = line;
        const char *name_start;
        const char *name_end;
        unsigned int maj = 0;
        unsigned int min = 0;
        unsigned int blocks = 0;

        while (*p && *p != '\n' && (*p == ' ' || *p == '\t'))
            p++;
        if (parse_uint_dec(&p, &maj) == 0) {
            skip_partition_space(&p);
            if (parse_uint_dec(&p, &min) == 0) {
                skip_partition_space(&p);
                if (parse_uint_dec(&p, &blocks) == 0) {
                    skip_partition_space(&p);
                    name_start = p;
                    while (*p && *p != '\n' && *p != ' ' && *p != '\t')
                        p++;
                    name_end = p;
                    if (name_eq_span(name_start, name_end, name)) {
                        if (maj == 0 && min == 0)
                            return -2;
                        *major = maj;
                        *minor = min;
                        return 0;
                    }
                }
            }
        }
        while (*line && *line != '\n')
            line++;
        if (*line == '\n')
            line++;
    }
    return -1;
}

static const char *partition_name_for_rootdev(const char *rootdev)
{
    if (str_eq(rootdev, "/dev/sda"))
        return "sda";
    if (str_eq(rootdev, "/dev/sda1"))
        return "sda1";
    if (str_eq(rootdev, "/dev/vda"))
        return "vda";
    if (str_eq(rootdev, "/dev/vda1"))
        return "vda1";
    return 0;
}

static int ensure_block_node_from_proc(const char *rootdev, unsigned int vm_index)
{
    const char *part_name = partition_name_for_rootdev(rootdev);
    unsigned int major = 0;
    unsigned int minor = 0;
    int i;
    long rc = -1;

    if (!part_name)
        return -1;

    for (i = 0; i < 1200; i++) {
        rc = read_proc_partition_dev_once(part_name, &major, &minor);
        if (rc == 0)
            break;
        if (rc == -2 && !proc_partitions_dumped) {
            proc_partitions_dumped = 1;
            dump_proc_partitions(vm_index);
        }
        if ((i % 200) == 0) {
            rootfs_log_msg(vm_index, "HYC3-BRINGUP: waiting /proc/partitions for nonzero ");
            rootfs_log_msg(vm_index, part_name);
            rootfs_log_msg(vm_index, " rc ");
            rootfs_log_hex(vm_index, (unsigned long)rc);
        }
        sleep_millis(10);
    }
    if (rc < 0)
        return -1;

    rootfs_log_msg(vm_index, "HYC3-BRINGUP: /proc/partitions ");
    rootfs_log_msg(vm_index, part_name);
    rootfs_log_msg(vm_index, " major ");
    rootfs_log_hex(vm_index, major);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: /proc/partitions ");
    rootfs_log_msg(vm_index, part_name);
    rootfs_log_msg(vm_index, " minor ");
    rootfs_log_hex(vm_index, minor);

    {
        unsigned long dev = hyc3_makedev(major, minor);

        rootfs_log_msg(vm_index, "HYC3-BRINGUP: proc mknod rootdev dev ");
        rootfs_log_hex(vm_index, dev);
        rc = sys3(SYS_mknod, (long)rootdev, S_IFBLK | 0600, (long)dev);
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: proc mknod rootdev rc ");
        rootfs_log_hex(vm_index, (unsigned long)rc);
        if (block_node_matches(rootdev, dev, vm_index))
            return 0;
        sys1(SYS_unlink, (long)rootdev);
        rc = sys3(SYS_mknod, (long)rootdev, S_IFBLK | 0600, (long)dev);
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: proc remknod rootdev rc ");
        rootfs_log_hex(vm_index, (unsigned long)rc);
        if (!block_node_matches(rootdev, dev, vm_index))
            return (rc < 0) ? (int)rc : -1;
    }
    return 0;
}

static int wait_for_block_open_logged(const char *path, int loops)
{
    int i;

    for (i = 0; i < loops; i++) {
        int fd = open_ro_nonblock(path);
        if (fd >= 0) {
            sys1(SYS_close, fd);
            log_msg("HYC3-BRINGUP: block device opens ");
            log_msg(path);
            log_msg("\n");
            return 0;
        }
        if ((i % 200) == 0) {
            log_msg("HYC3-BRINGUP: block open waiting ");
            log_msg(path);
            log_msg(" rc ");
            log_hex((unsigned long)fd);
        }
        sleep_millis(10);
    }

    log_msg("HYC3-BRINGUP: block device never opened ");
    log_msg(path);
    log_msg("\n");
    return -1;
}

static int wait_for_module_dir(const char *name)
{
    (void)name;
    return 0;
}

static void load_module_logged(const char *path, const char *params, const char *tag)
{
    long rc;

    log_msg("HYC3-BRINGUP: loading ");
    log_msg(tag);
    log_msg("\n");
    rc = load_module_from_file(path, params);
    log_msg("HYC3-BRINGUP: ");
    log_msg(tag);
    log_msg(" rc ");
    log_hex((unsigned long)rc);
}

static void fork_storage_probe(const char *mode)
{
    long child;

    child = sys1(SYS_fork, 0);
    if (child == 0) {
        if (str_eq(mode, "virtio")) {
            load_module_logged("/lib/modules/hyc3/virtio_blk.ko", "", "virtio_blk");
        } else if (str_eq(mode, "ahci")) {
            load_module_logged("/lib/modules/hyc3/sd_mod.ko", "probe=sync", "sd_mod");
            load_module_logged("/lib/modules/hyc3/libata.ko", "", "libata");
            load_module_logged("/lib/modules/hyc3/libahci.ko", "", "libahci");
            load_module_logged("/lib/modules/hyc3/ahci.ko", "", "ahci");
        } else {
            load_module_logged("/lib/modules/hyc3/sd_mod.ko", "probe=sync", "sd_mod");
            load_module_logged("/lib/modules/hyc3/vmw_pvscsi.ko", "", "vmw_pvscsi");
        }
        log_msg("HYC3-BRINGUP: storage probe child done\n");
        sys1(SYS_exit, 0);
    }

    log_msg("HYC3-BRINGUP: storage probe fork rc ");
    log_hex((unsigned long)child);
}

static void fork_xfs_probe(void)
{
    long child;

    child = sys1(SYS_fork, 0);
    if (child == 0) {
        load_module_logged("/lib/modules/hyc3/libcrc32c.ko", "", "libcrc32c");
        load_module_logged("/lib/modules/hyc3/xfs.ko", "", "xfs");
        log_msg("HYC3-BRINGUP: fs probe child done\n");
        sys1(SYS_exit, 0);
    }

    log_msg("HYC3-BRINGUP: fs probe fork rc ");
    log_hex((unsigned long)child);
}

static void load_storage_probe_sync(const char *mode)
{
    if (str_eq(mode, "virtio")) {
        load_module_logged("/lib/modules/hyc3/virtio_blk.ko", "", "virtio_blk");
    } else if (str_eq(mode, "ahci")) {
        load_module_logged("/lib/modules/hyc3/sd_mod.ko", "probe=sync", "sd_mod");
        load_module_logged("/lib/modules/hyc3/libata.ko", "", "libata");
        load_module_logged("/lib/modules/hyc3/libahci.ko", "", "libahci");
        load_module_logged("/lib/modules/hyc3/ahci.ko", "", "ahci");
    } else {
        load_module_logged("/lib/modules/hyc3/sd_mod.ko", "probe=sync", "sd_mod");
        load_module_logged("/lib/modules/hyc3/vmw_pvscsi.ko", "", "vmw_pvscsi");
    }
    log_msg("HYC3-BRINGUP: storage probe sync done\n");
}

static void start_storage_probe_async(const char *mode, unsigned int vm_index)
{
    long child = sys1(SYS_fork, 0);

    if (child == 0) {
        sleep_millis(500);
        yield_many(4096);
        load_storage_probe_sync(mode);
        sys1(SYS_exit, 0);
    }

    rootfs_log_msg(vm_index, "HYC3-BRINGUP: storage probe fork rc ");
    rootfs_log_hex(vm_index, (unsigned long)child);
    if (child < 0) {
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: storage fork failed; probing synchronously\n");
        load_storage_probe_sync(mode);
    }
}


static void load_network_probe_sync(void)
{
    log_msg("HYC3-BRINGUP: network probe sync begin\n");
    load_module_logged("/lib/modules/hyc3/failover.ko", "", "failover");
    load_module_logged("/lib/modules/hyc3/net_failover.ko", "", "net_failover");
    load_module_logged("/lib/modules/hyc3/virtio_net.ko", "", "virtio_net");
    log_msg("HYC3-BRINGUP: network probe sync done\n");
}

static void load_xfs_probe_sync(void)
{
    load_module_logged("/lib/modules/hyc3/libcrc32c.ko", "", "libcrc32c");
    load_module_logged("/lib/modules/hyc3/xfs.ko", "", "xfs");
    log_msg("HYC3-BRINGUP: fs probe sync done\n");
}

static void start_xfs_probe_async(unsigned int vm_index)
{
    long child = sys1(SYS_fork, 0);

    if (child == 0) {
        sleep_millis(700);
        yield_many(4096);
        load_xfs_probe_sync();
        sys1(SYS_exit, 0);
    }

    rootfs_log_msg(vm_index, "HYC3-BRINGUP: fs probe fork rc ");
    rootfs_log_hex(vm_index, (unsigned long)child);
    if (child < 0) {
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: fs fork failed; probing synchronously\n");
        load_xfs_probe_sync();
    }
}

static int cmdline_has(const char *needle)
{
    int fd;
    char buf[1024];
    long n;
    size_t i;
    size_t j;
    size_t needle_len = cstr_len(needle);

    fd = open_ro("/proc/cmdline");
    if (fd < 0)
        return 0;
    n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
    sys1(SYS_close, fd);
    if (n <= 0)
        return 0;

    for (i = 0; i + needle_len <= (size_t)n; i++) {
        for (j = 0; j < needle_len; j++) {
            if (buf[i + j] != needle[j])
                break;
        }
        if (j == needle_len)
            return 1;
    }
    return 0;
}

static int buffer_contains(const char *buf, size_t n, const char *needle)
{
    size_t i;
    size_t j;
    size_t needle_len = cstr_len(needle);

    if (needle_len == 0)
        return 1;
    for (i = 0; i + needle_len <= n; i++) {
        for (j = 0; j < needle_len; j++) {
            if (buf[i + j] != needle[j])
                break;
        }
        if (j == needle_len)
            return 1;
    }
    return 0;
}

static int proc_filesystems_has(const char *type)
{
    char buf[2048];
    int fd;
    long n;

    fd = open_ro("/proc/filesystems");
    if (fd < 0)
        return 0;
    n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
    sys1(SYS_close, fd);
    if (n <= 0)
        return 0;
    return buffer_contains(buf, (size_t)n, type);
}

static int wait_for_filesystem_type(unsigned int vm_index, const char *type, int loops)
{
    int i;

    for (i = 0; i < loops; i++) {
        if (proc_filesystems_has(type)) {
            rootfs_log_msg(vm_index, "HYC3-BRINGUP: filesystem registered ");
            rootfs_log_msg(vm_index, type);
            rootfs_log_msg(vm_index, "\n");
            return 0;
        }
        if ((i % 200) == 0) {
            rootfs_log_msg(vm_index, "HYC3-BRINGUP: waiting filesystem ");
            rootfs_log_msg(vm_index, type);
            rootfs_log_msg(vm_index, "\n");
        }
        sleep_millis(10);
    }
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: filesystem never registered ");
    rootfs_log_msg(vm_index, type);
    rootfs_log_msg(vm_index, "\n");
    return -1;
}

static const char *requested_rootdev_for_mode(const char *mode)
{
    if (cmdline_has("hyc3_rootdev=/dev/sda1"))
        return "/dev/sda1";
    if (cmdline_has("hyc3_rootdev=/dev/sda"))
        return "/dev/sda";
    if (cmdline_has("hyc3_rootdev=/dev/vda1"))
        return "/dev/vda1";
    if (cmdline_has("hyc3_rootdev=/dev/vda"))
        return "/dev/vda";
    if (str_eq(mode, "virtio"))
        return "/dev/vda";
    return "/dev/sda";
}

static void exec_real_init(unsigned int vm_index)
{
    static char *argv[] = { "/sbin/init", 0 };
    static char *envp[] = {
        "HOME=/",
        "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
        "TERM=linux",
        0,
    };
    long rc;

    rootfs_log_msg(vm_index, "HYC3-BRINGUP: exec real /sbin/init from mounted rootfs\n");
    sys1(SYS_chdir, (long)"/sysroot");
    rc = sys6(SYS_mount, (long)".", (long)"/", 0, MS_MOVE, 0, 0);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: switch_root move rc ");
    rootfs_log_hex(vm_index, (unsigned long)rc);
    if (rc < 0)
        return;
    rc = sys1(SYS_chroot, (long)".");
    if (rc < 0) {
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: chroot failed ");
        rootfs_log_hex(vm_index, (unsigned long)rc);
        return;
    }
    sys1(SYS_chdir, (long)"/");
    rc = sys3(SYS_execve, (long)"/sbin/init", (long)argv, (long)envp);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: exec /sbin/init failed ");
    rootfs_log_hex(vm_index, (unsigned long)rc);
    rootfs_ready = 0;
}


static int path_exists(const char *path)
{
    return sys2(SYS_access, (long)path, 0) == 0;
}

static long mount_rootfs_once(const char *rootdev, unsigned int vm_index)
{
    unsigned long flags = 0;
    const char *data = "nouuid";
    long rc;

    if (vm_index == 1 && cmdline_has("hyc3_l2_2_ro=1")) {
        flags = MS_RDONLY;
        data = "nouuid,norecovery";
    }
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: mount rootfs syscall begin flags ");
    rootfs_log_hex(vm_index, flags);
    rc = mount_fs_data(rootdev, "/sysroot", "xfs", flags, data);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: mount rootfs syscall rc ");
    rootfs_log_hex(vm_index, (unsigned long)rc);
    return rc;
}

static int rootfs_mount_visible(void)
{
    return path_exists("/sysroot/etc") || path_exists("/sysroot/bin") ||
           path_exists("/sysroot/sbin/init");
}

static int mount_rootfs_async_for_l2_2(const char *rootdev, unsigned int vm_index)
{
    long child;
    int status = 0;
    int i;

    child = sys1(SYS_fork, 0);
    if (child == 0) {
        long rc = -1;
        int j;

        for (j = 0; j < 300; j++) {
            rc = mount_rootfs_once(rootdev, vm_index);
            if (rc == 0)
                sys1(SYS_exit, 0);
            sleep_millis(10);
        }
        sys1(SYS_exit, 1);
    }

    rootfs_log_msg(vm_index, "HYC3-BRINGUP: mount rootfs worker fork rc ");
    rootfs_log_hex(vm_index, (unsigned long)child);
    if (child < 0)
        return -1;

    for (i = 0; i < 1500; i++) {
        long wait_rc;

        if (rootfs_mount_visible()) {
            rootfs_log_msg(vm_index, "HYC3-BRINGUP: mount rootfs worker visible\n");
            sys6(SYS_wait4, child, (long)&status, WNOHANG, 0, 0, 0);
            return 0;
        }
        wait_rc = sys6(SYS_wait4, child, (long)&status, WNOHANG, 0, 0, 0);
        if (wait_rc == child) {
            if (rootfs_mount_visible())
                return 0;
            rootfs_log_msg(vm_index, "HYC3-BRINGUP: mount rootfs worker exited status ");
            rootfs_log_hex(vm_index, (unsigned long)status);
            return -1;
        }
        if ((i % 100) == 0)
            rootfs_log_msg(vm_index, "HYC3-BRINGUP: waiting for L2-2 mount worker\n");
        sleep_millis(10);
    }

    rootfs_log_msg(vm_index, "HYC3-BRINGUP: L2-2 mount worker timed out; falling back\n");
    sys2(SYS_kill, child, SIGKILL);
    sys6(SYS_wait4, child, (long)&status, WNOHANG, 0, 0, 0);
    return -1;
}

static int try_mount_rootfs(const char *mode, unsigned int vm_index)
{
    long rc = -1;
    int i;
    const char *rootdev;

    if (!mode || mode[0] == 0)
        mode = "virtio";

    rootdev = requested_rootdev_for_mode(mode);

    mkdir_one("/dev");
    rc = mount_fs("devtmpfs", "/dev", "devtmpfs", 0);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: devtmpfs mount rc ");
    rootfs_log_hex(vm_index, (unsigned long)rc);

    rootfs_log_msg(vm_index, "HYC3-BRINGUP: direct rootfs target ");
    rootfs_log_msg(vm_index, rootdev);
    rootfs_log_msg(vm_index, " mode ");
    rootfs_log_msg(vm_index, mode);
    rootfs_log_msg(vm_index, "\n");

    rootfs_tty_only = 1;
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: storage probe sync begin before block wait\n");
    load_storage_probe_sync(mode);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: storage probe sync complete before block wait\n");

    dump_sysfs_block(vm_index, rootdev);
    rc = ensure_block_node_from_sysfs(rootdev, vm_index);
    if (rc < 0) {
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: sysfs/mknod rootdev failed rc ");
        rootfs_log_hex(vm_index, (unsigned long)rc);
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: falling back to /proc/partitions rootdev lookup\n");
        rc = ensure_block_node_from_proc(rootdev, vm_index);
        if (rc < 0) {
            rootfs_tty_only = 0;
            rootfs_log_msg(vm_index, "HYC3-BRINGUP: proc/mknod rootdev failed rc ");
            rootfs_log_hex(vm_index, (unsigned long)rc);
            return -1;
        }
    }

    rootfs_log_msg(vm_index, "HYC3-BRINGUP: rootdev node ready; probing block open ");
    rootfs_log_msg(vm_index, rootdev);
    rootfs_log_msg(vm_index, "\n");
    if (str_eq(mode, "virtio")) {
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: skipping virtio block open probe; sysfs node is ready\n");
    } else if (wait_for_block_open_logged(rootdev, 1000) < 0) {
        unsigned int expect_major = str_eq(rootdev, "/dev/vda") ? 253U : 8U;

        rootfs_log_msg(vm_index, "HYC3-BRINGUP: block open failed; dumping block state before one mount attempt\n");
        dump_proc_partitions(vm_index);
        dump_sysfs_block(vm_index, rootdev);
        block_node_matches(rootdev, hyc3_makedev(expect_major, 0), vm_index);
        mkdir_one("/sysroot");
        rc = mount_fs_data(rootdev, "/sysroot", "xfs", 0, "nouuid");
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: mount rootfs single rc ");
        rootfs_log_hex(vm_index, (unsigned long)rc);
        return -1;
    }

    if (cmdline_has("hyc3_real_init=systemd")) {
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: fs probe sync begin after block node\n");
        load_xfs_probe_sync();
        if (wait_for_filesystem_type(vm_index, "xfs", 3000) < 0) {
            rootfs_tty_only = 0;
            return -1;
        }
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: fs probe sync ready after block node\n");
    } else {
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: fs probe async begin after block node\n");
        start_xfs_probe_async(vm_index);
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: fs probe async launched after block node\n");
    }

    mkdir_one("/sysroot");
    if (vm_index == 1 && !cmdline_has("hyc3_real_init=systemd")) {
        rc = mount_rootfs_async_for_l2_2(rootdev, vm_index);
    } else {
        int max_mount_tries = cmdline_has("hyc3_real_init=systemd") ? 6000 : 300;

        for (i = 0; i < max_mount_tries; i++) {
            rc = mount_rootfs_once(rootdev, vm_index);
            if (rc == 0)
                break;
            if ((i % 50) == 0) {
                rootfs_log_msg(vm_index, "HYC3-BRINGUP: mount rootfs retry rc ");
                rootfs_log_hex(vm_index, (unsigned long)rc);
                dump_sysfs_block(vm_index, rootdev);
            }
            sleep_millis(50);
        }
    }
    if (rc < 0) {
        rootfs_tty_only = 0;
        rootfs_log_msg(vm_index, "HYC3-BRINGUP: mount rootfs failed rc ");
        rootfs_log_hex(vm_index, (unsigned long)rc);
        return -1;
    }


    rootfs_tty_only = 0;
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: mount rootfs rc ");
    rootfs_log_hex(vm_index, (unsigned long)rc);

    rootfs_ready = 1;
    mkdir_one("/sysroot/proc");
    mkdir_one("/sysroot/sys");
    mkdir_one("/sysroot/dev");
    mkdir_one("/sysroot/run");
    mount_fs("proc", "/sysroot/proc", "proc", 0);
    mount_fs("sysfs", "/sysroot/sys", "sysfs", 0);
    mount_fs("devtmpfs", "/sysroot/dev", "devtmpfs", 0);
    mount_fs("tmpfs", "/sysroot/run", "tmpfs", 0);
    rootfs_log_msg(vm_index, "HYC3-BRINGUP: rootfs mounted rw; ready for full OS handoff\n");
    return 0;
}
static void park_after_module(unsigned int vm_index)
{
    long repl_rc;

    if (vm_index == 0) {
        log_msg("HYC3-BRINGUP: L2-1 rootfs shell is active; PID1 idling\n");
        idle_forever();
    }

    log_msg("HYC3-BRINGUP: L2-2 worker/rootfs is alive; entering COM2 raw console\n");
    repl_rc = serial_repl(COM2_BASE, "hyc3-L2-2# ");
    log_msg("HYC3-BRINGUP: L2-2 raw serial REPL unavailable ");
    log_hex((unsigned long)repl_rc);
    idle_forever();
}


static const char *requested_rootfs_mode(void)
{
    if (cmdline_has("hyc3_rootfs=virtio"))
        return "virtio";
    if (cmdline_has("hyc3_rootfs=ahci"))
        return "ahci";
    if (cmdline_has("hyc3_rootfs=pvscsi") || cmdline_has("hyc3_rootfs=1"))
        return "pvscsi";
    return 0;
}


static long run_chroot_cmd_wait(unsigned int vm_index, char **argv, const char *tag, int wait_loops)
{
    static char *env[] = {
        "HOME=/root",
        "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
        "TERM=dumb",
        0,
    };
    long child;
    int status = 0;
    int i;

    rootfs_log_msg(vm_index, "HYC3-SSH: starting ");
    rootfs_log_msg(vm_index, tag);
    rootfs_log_msg(vm_index, "\n");

    child = sys1(SYS_fork, 0);
    if (child == 0) {
        sys1(SYS_chroot, (long)"/sysroot");
        sys1(SYS_chdir, (long)"/");
        sys3(SYS_execve, (long)argv[0], (long)argv, (long)env);
        sys1(SYS_exit, 127);
    }
    if (child < 0) {
        rootfs_log_msg(vm_index, "HYC3-SSH: fork failed ");
        rootfs_log_hex(vm_index, (unsigned long)child);
        return child;
    }

    for (i = 0; i < wait_loops; i++) {
        long rc = sys6(SYS_wait4, child, (long)&status, WNOHANG, 0, 0, 0);
        if (rc == child) {
            rootfs_log_msg(vm_index, "HYC3-SSH: command exited ");
            rootfs_log_msg(vm_index, tag);
            rootfs_log_msg(vm_index, " status ");
            rootfs_log_hex(vm_index, (unsigned long)status);
            return status;
        }
        if (rc < 0) {
            rootfs_log_msg(vm_index, "HYC3-SSH: wait failed ");
            rootfs_log_msg(vm_index, tag);
            rootfs_log_msg(vm_index, " rc ");
            rootfs_log_hex(vm_index, (unsigned long)rc);
            return rc;
        }
        sleep_millis(10);
    }

    rootfs_log_msg(vm_index, "HYC3-SSH: command still running/daemonized ");
    rootfs_log_msg(vm_index, tag);
    rootfs_log_msg(vm_index, "\n");
    return 0;
}

static void maybe_start_l2_1_ssh(unsigned int vm_index)
{
    char *ip_link[] = { "/usr/sbin/ip", "link", "set", "dev", "eth0", "up", 0 };
    char *ip_addr[] = { "/usr/sbin/ip", "addr", "add", "10.0.2.15/24", "dev", "eth0", 0 };
    char *ip_route[] = { "/usr/sbin/ip", "route", "add", "default", "via", "10.0.2.2", "dev", "eth0", 0 };
    char *ssh_keygen[] = { "/usr/bin/ssh-keygen", "-A", 0 };
    char *sshd[] = { "/usr/sbin/sshd", "-E", "/var/log/hyc3-sshd.log", 0 };

    if (vm_index != 0 || !rootfs_ready || !cmdline_has("hyc3_l2_1_ssh=1"))
        return;

    rootfs_log_msg(vm_index, "HYC3-SSH: L2-1-only SSH bootstrap requested\n");
    load_network_probe_sync();
    mkdir_one("/sysroot/run");
    mkdir_one("/sysroot/run/sshd");
    mkdir_one("/sysroot/var");
    mkdir_one("/sysroot/var/log");
    run_chroot_cmd_wait(vm_index, ssh_keygen, "ssh-keygen -A", 1000);
    run_chroot_cmd_wait(vm_index, ip_link, "ip link set eth0 up", 1000);
    run_chroot_cmd_wait(vm_index, ip_addr, "ip addr add 10.0.2.15/24", 1000);
    run_chroot_cmd_wait(vm_index, ip_route, "ip route add default", 1000);
    run_chroot_cmd_wait(vm_index, sshd, "sshd", 1000);
    rootfs_log_msg(vm_index, "HYC3-SSH: L2-1 sshd bootstrap finished; host forward should be 127.0.0.1:10021 -> guest:22\n");
}

static void maybe_start_l2_1_ssh_async(unsigned int vm_index)
{
    long child;

    if (vm_index != 0 || !rootfs_ready || !cmdline_has("hyc3_l2_1_ssh=1"))
        return;

    child = sys1(SYS_fork, 0);
    if (child == 0) {
        maybe_start_l2_1_ssh(vm_index);
        sys1(SYS_exit, 0);
    }

    rootfs_log_msg(vm_index, "HYC3-SSH: async bootstrap fork rc ");
    rootfs_log_hex(vm_index, (unsigned long)child);
}

static void run_hyc3_module_test(void)
{
    int fd;
    char *image;
    long n;
    unsigned long total = 0;
    const char *module_path = "/tdcall_module.ko";
    const char *params = "autotest=1";

    if (cmdline_has("hyc3_autotest=0") || cmdline_has("hyc3_skip_shared_test=1"))
        params = "ready_only=1 autotest=0";

    log_msg("HYC3-BRINGUP: loading tdcall_module.ko params ");
    log_msg(params);
    log_msg("\n");

    image = (char *)sys6(SYS_mmap, 0, MODULE_MAX, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)image < 0) {
        log_msg("HYC3-BRINGUP: mmap failed ");
        log_hex((unsigned long)image);
        return;
    }

    fd = open_ro(module_path);
    if (fd < 0) {
        log_msg("HYC3-BRINGUP: open module failed ");
        log_hex((unsigned long)fd);
        return;
    }

    while (total < MODULE_MAX) {
        n = sys3(SYS_read, fd, (long)(image + total), MODULE_MAX - total);
        if (n == 0)
            break;
        if (n < 0) {
            log_msg("HYC3-BRINGUP: read module failed ");
            log_hex((unsigned long)n);
            sys1(SYS_close, fd);
            return;
        }
        total += (unsigned long)n;
    }
    sys1(SYS_close, fd);

    log_msg("HYC3-BRINGUP: module bytes ");
    log_hex(total);

    n = sys3(SYS_init_module, (long)image, total, (long)params);
    if (n < 0) {
        log_msg("HYC3-BRINGUP: init_module failed ");
        log_hex((unsigned long)n);
    } else {
        log_msg("HYC3-BRINGUP: init_module success\n");
    }
}

static void run_hyc3_module_test_async(void)
{
    long child = sys1(SYS_fork, 0);

    if (child == 0) {
        run_hyc3_module_test();
        sys1(SYS_exit, 0);
    }

    log_msg("HYC3-BRINGUP: module test fork rc ");
    log_hex((unsigned long)child);
    yield_many(4096);
}


void _start(void)
{
    unsigned int vm_index;
    const char *rootfs_mode;
    int module_loaded = 0;

    mkdir_one("/dev");
    mount_one("devtmpfs", "/dev", "devtmpfs");
    refresh_log_fds();

    mkdir_one("/proc");
    mkdir_one("/sys");
    mount_one("proc", "/proc", "proc");
    mount_one("sysfs", "/sys", "sysfs");

    refresh_cwd();
    log_msg("HYC3-BRINGUP: init started\n");
    vm_index = hyc3_vm_index();
    rootfs_mode = requested_rootfs_mode();

    if (rootfs_mode) {
        if (vm_index == 0) {
            log_msg("HYC3-BRINGUP: L2-1 rootfs requested; mounting full OS first\n");
        } else if (vm_index == 1) {
            log_msg("HYC3-BRINGUP: L2-2 rootfs requested; mounting full OS first\n");
        } else {
            log_msg("HYC3-BRINGUP: unknown VM index for rootfs path\n");
        }

        if (try_mount_rootfs(rootfs_mode, vm_index) == 0) {
            if (cmdline_has("hyc3_real_init=systemd")) {
                if (!cmdline_has("hyc3_skip_module=1")) {
                    rootfs_log_msg(vm_index, "HYC3-BRINGUP: loading tdcall_module before systemd handoff\n");
                    run_hyc3_module_test();
                    module_loaded = 1;
                }
                rootfs_log_msg(vm_index, "HYC3-BRINGUP: systemd handoff requested; execing real /sbin/init\n");
                exec_real_init(vm_index);
                rootfs_log_msg(vm_index, "HYC3-BRINGUP: full OS exec failed; falling back to rootfs shell\n");
            } else {
                rootfs_log_msg(vm_index, "HYC3-BRINGUP: real rootfs shell mode; systemd handoff disabled by default\n");
                if (!cmdline_has("hyc3_skip_module=1")) {
                    rootfs_log_msg(vm_index, "HYC3-BRINGUP: loading tdcall_module before SSH/rootfs shell\n");
                    run_hyc3_module_test();
                    module_loaded = 1;
                }
                maybe_start_l2_1_ssh_async(vm_index);
            }
        } else {
            if (cmdline_has("hyc3_real_init=systemd") && !cmdline_has("hyc3_allow_initramfs_fallback=1")) {
                rootfs_log_msg(vm_index, "HYC3-BRINGUP: rootfs mount failed; systemd handoff cannot continue\n");
                idle_forever();
            }
            rootfs_log_msg(vm_index, "HYC3-BRINGUP: rootfs mount failed; falling back to initramfs shell\n");
            log_msg("HYC3-BRINGUP: starting tdcall_module after rootfs failure\n");
            run_hyc3_module_test_async();
            module_loaded = 1;
        }
    }

    if (!module_loaded && !rootfs_mode) {
        run_hyc3_module_test_async();
        module_loaded = 1;
    }

    if (rootfs_mode) {
        start_l2_1_shell(vm_index);
        if (!module_loaded && rootfs_ready && !cmdline_has("hyc3_skip_module=1")) {
            log_msg("HYC3-BRINGUP: starting tdcall_module after real rootfs shell setup\n");
            run_hyc3_module_test_async();
            module_loaded = 1;
        }
        log_msg("HYC3-BRINGUP: real rootfs command shell remains available\n");
    } else {
        start_l2_1_shell(vm_index);
        log_msg("HYC3-BRINGUP: rootfs mount disabled; use HYC3_ROOTFS=pvscsi to test it\n");
    }
    park_after_module(vm_index);
}
