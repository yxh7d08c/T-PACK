#include <fcntl.h>
#include <unistd.h>
#include <sys/reboot.h>

static void say(int fd, const char *s) {
    const char *p = s;
    while (*p) p++;
    write(fd, s, p - s);
}

int main(void) {
    say(1, "HYC3_STATIC_INIT_START stdout\n");
    say(2, "HYC3_STATIC_INIT_START stderr\n");
    int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (fd >= 0) {
        say(fd, "HYC3_STATIC_INIT_START console\n");
        close(fd);
    }
    for (;;) pause();
    return 0;
}
