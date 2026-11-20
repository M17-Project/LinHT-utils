// linht-gnssd: NMEA pass-through, a pty and a unix socket

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include "core.h"
#include "feed.h"

// More than this unread in the pty (about 1 s of NMEA at 9600 baud) means
// nobody reads it: the backlog is dropped, so a new reader starts with
// fresh sentences.
#define PTY_BACKLOG 1024

void feed_init(struct feed *f)
{
    memset(f, 0, sizeof(*f));
    f->master = f->slave = f->listen = -1;
    for (int i = 0; i < FEED_CLIENTS; i++)
        f->clients[i] = -1;
}

static void own(const char *path, int fd, const char *group)
{
    struct group *g = group[0] ? getgrnam(group) : NULL;
    gid_t gid = g ? g->gr_gid : 0;
    if (fd >= 0)
    {
        if (fchown(fd, 0, gid) < 0 || fchmod(fd, 0660) < 0)
            core_log(4, "%s: %s", path, strerror(errno));
    }
    else if (chown(path, 0, gid) < 0 || chmod(path, 0660) < 0)
    {
        core_log(4, "%s: %s", path, strerror(errno));
    }
}

static int open_pty(struct feed *f, const char *link, const char *group)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (m < 0)
        return -errno;
    if (grantpt(m) < 0 || unlockpt(m) < 0 || ptsname_r(m, f->slave_name, sizeof(f->slave_name)))
        goto fail;
    int s = open(f->slave_name, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (s < 0)
        goto fail;
    struct termios t;
    if (tcgetattr(s, &t) == 0)
    {
        cfmakeraw(&t);
        cfsetispeed(&t, B9600);
        cfsetospeed(&t, B9600);
        tcsetattr(s, TCSANOW, &t);
    }
    own(f->slave_name, s, group);
    unlink(link);
    if (symlink(f->slave_name, link) < 0)
    {
        int e = errno;
        close(s);
        close(m);
        return -e;
    }
    f->master = m;
    f->slave = s;
    snprintf(f->link, sizeof(f->link), "%s", link);
    return 0;

fail:;
    int e = errno;
    close(m);
    return -e;
}

static int open_sock(struct feed *f, const char *path, const char *group)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(a.sun_path))
        return -ENAMETOOLONG;
    strcpy(a.sun_path, path);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -errno;
    unlink(path);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 4) < 0)
    {
        int e = errno;
        close(s);
        return -e;
    }
    own(path, -1, group);
    f->listen = s;
    snprintf(f->sock, sizeof(f->sock), "%s", path);
    return 0;
}

int feed_open(struct feed *f, const char *link, const char *sock, const char *group)
{
    int rc = 0, r;
    if (link[0] && (r = open_pty(f, link, group)) < 0)
    {
        core_log(3, "NMEA pty %s: %s", link, strerror(-r));
        rc = r;
    }
    if (sock[0] && (r = open_sock(f, sock, group)) < 0)
    {
        core_log(3, "NMEA socket %s: %s", sock, strerror(-r));
        rc = r;
    }
    return rc;
}

void feed_write(struct feed *f, const char *line, size_t n)
{
    if (f->master >= 0)
    {
        int q = 0;
        if (ioctl(f->slave, FIONREAD, &q) == 0 && q > PTY_BACKLOG)
            tcflush(f->slave, TCIFLUSH);
        if (write(f->master, line, n) < 0 && errno == EAGAIN)
            tcflush(f->slave, TCIFLUSH);
    }
    for (int i = 0; i < FEED_CLIENTS; i++)
    {
        if (f->clients[i] < 0 || f->dead[i])
            continue;
        ssize_t w = send(f->clients[i], line, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0 && errno != EAGAIN)
            f->dead[i] = 1;
        else if (w < (ssize_t)n)
            f->dropped++;
    }
}

void feed_drain_pty(struct feed *f)
{
    char buf[256];
    while (f->master >= 0 && read(f->master, buf, sizeof(buf)) > 0)
        ;
}

int feed_accept(struct feed *f)
{
    int c = accept4(f->listen, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0)
        return -1;
    for (int i = 0; i < FEED_CLIENTS; i++)
        if (f->clients[i] < 0)
        {
            f->clients[i] = c;
            f->client_src[i] = NULL;
            f->dead[i] = 0;
            return i;
        }
    close(c);
    return -1;
}

int feed_client_input(struct feed *f, int slot)
{
    char buf[256];
    ssize_t r;
    while ((r = read(f->clients[slot], buf, sizeof(buf))) > 0)
        ;
    return r == 0 || (r < 0 && errno != EAGAIN);
}

void feed_close_client(struct feed *f, int slot)
{
    if (f->clients[slot] >= 0)
        close(f->clients[slot]);
    f->clients[slot] = -1;
    f->dead[slot] = 0;
    f->client_src[slot] = NULL;
}

void feed_close(struct feed *f)
{
    for (int i = 0; i < FEED_CLIENTS; i++)
        feed_close_client(f, i);
    if (f->link[0])
        unlink(f->link);
    if (f->sock[0])
        unlink(f->sock);
    if (f->listen >= 0)
        close(f->listen);
    if (f->slave >= 0)
        close(f->slave);
    if (f->master >= 0)
        close(f->master);
    feed_init(f);
}
