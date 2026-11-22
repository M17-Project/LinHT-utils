// linht-gnssd: the receiver UART and the RESET_N line

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/rtc.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/timex.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "tty.h"

static speed_t speed(unsigned baud)
{
    switch (baud)
    {
    case 4800:
        return B4800;
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    }
    return 0;
}

int tty_open(const char *path, unsigned baud)
{
    speed_t sp = speed(baud);
    if (!sp)
        return -EINVAL;
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0)
    {
        int e = errno;
        close(fd);
        return e == EWOULDBLOCK ? -EWOULDBLOCK : -e;
    }
    struct termios t;
    if (isatty(fd))
    {
        if (tcgetattr(fd, &t) < 0)
            goto fail;
        cfmakeraw(&t);
        t.c_cflag = CS8 | CLOCAL | CREAD;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        cfsetispeed(&t, sp);
        cfsetospeed(&t, sp);
        if (tcsetattr(fd, TCSANOW, &t) < 0)
            goto fail;
        tcflush(fd, TCIOFLUSH);
    }
    return fd;

fail:;
    int e = errno;
    close(fd);
    return -e;
}

int gpio_pulse_low(const char *chip, int line, unsigned ms)
{
    if (!chip[0] || line < 0)
        return -ENOTSUP;
    int cfd = open(chip, O_RDWR | O_CLOEXEC);
    if (cfd < 0)
        return -errno;

    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = (unsigned)line;
    req.num_lines = 1;
    snprintf(req.consumer, sizeof(req.consumer), "linht-gnssd");
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 0;    // physical low
    req.config.attrs[0].mask = 1;
    int r = 0;
    if (ioctl(cfd, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
    {
        r = -errno;
        close(cfd);
        return r;
    }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
    // Back to an input. Releasing an output line keeps it driven on some
    // GPIO drivers, so the direction is changed before the release.
    struct gpio_v2_line_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = GPIO_V2_LINE_FLAG_INPUT;
    if (ioctl(req.fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &cfg) < 0)
        r = -errno;
    close(req.fd);
    close(cfd);
    return r;
}

int rtc_set_from_system(const char *dev, double offset, time_t *written)
{
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    double t = now.tv_sec + now.tv_nsec * 1e-9 - offset;
    double next = floor(t) + 1;
    double wait = next - t;
    struct timespec ts = { (time_t)wait, (long)((wait - floor(wait)) * 1e9) };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
    time_t sec = (time_t)next;
    struct tm tm;
    gmtime_r(&sec, &tm);
    struct rtc_time rt;
    memset(&rt, 0, sizeof(rt));
    rt.tm_sec = tm.tm_sec;
    rt.tm_min = tm.tm_min;
    rt.tm_hour = tm.tm_hour;
    rt.tm_mday = tm.tm_mday;
    rt.tm_mon = tm.tm_mon;
    rt.tm_year = tm.tm_year;
    rt.tm_wday = tm.tm_wday;
    rt.tm_yday = tm.tm_yday;
    int r = ioctl(fd, RTC_SET_TIME, &rt) < 0 ? -errno : 0;
    close(fd);
    if (r == 0 && written)
        *written = sec;
    return r;
}

int rtc_driver_name(const char *dev, char *out, size_t size)
{
    char real[PATH_MAX], path[PATH_MAX + 32];
    if (!realpath(dev, real))
        return -errno;
    const char *base = strrchr(real, '/');
    snprintf(path, sizeof(path), "/sys/class/rtc/%s/name", base ? base + 1 : real);
    FILE *f = fopen(path, "r");
    if (!f)
        return -errno;
    int r = fgets(out, (int)size, f) ? 0 : -EIO;
    fclose(f);
    if (r == 0)
        out[strcspn(out, "\n")] = 0;
    return r;
}

int clock_ntp_synced(void)
{
    struct timex tx;
    memset(&tx, 0, sizeof(tx));
    if (adjtimex(&tx) < 0)
        return 0;
    return tx.maxerror < 16000000;
}
