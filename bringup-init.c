#include <unistd.h>
#include <time.h>

static void say(const char *s) {
    const char *p = s;
    while (*p) p++;
    (void)write(1, s, (size_t)(p - s));
}

int main(void) {
    const struct timespec ts = {30, 0};
    say("HYC3-BRINGUP: init started\n");
    say("HYC3-BRINGUP: entering idle loop\n");
    for (;;) {
        say("HYC3-BRINGUP: alive\n");
        nanosleep(&ts, 0);
    }
}
