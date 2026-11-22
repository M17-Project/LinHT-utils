// linht-gnssd: NMEA pass-through for gpsd, the FART field logger and
// other readers. The daemon owns the UART, the readers get a copy of every
// valid sentence:
//   - a pty, reached through a symlink (default /run/linht-gnssd/nmea),
//     for one tty reader such as gpsd or field-log.py --gnss,
//   - a unix stream socket (default /run/linht-gnssd/nmea.sock) for any
//     number of readers.
// Both are read only. What a reader writes is dropped, never sent to the
// receiver.

#ifndef GNSSD_FEED_H
#define GNSSD_FEED_H

#include <stddef.h>

#define FEED_CLIENTS 8

struct feed
{
    int master, slave;              // pty, the slave stays open in the daemon
    char slave_name[64];
    char link[96];
    int listen;
    char sock[96];
    int clients[FEED_CLIENTS];
    void *client_src[FEED_CLIENTS]; // for the event loop of the caller
    int dead[FEED_CLIENTS];         // write failed, the caller closes it
    unsigned dropped;               // lines a slow socket reader did not get
};

void feed_init(struct feed *f);
// Empty paths turn that part off. `group` owns the pty and the socket
// (mode 0660), empty or unknown keeps root. Returns 0 or -errno; a failed
// part is logged and stays off.
int feed_open(struct feed *f, const char *link, const char *sock, const char *group);
void feed_write(struct feed *f, const char *line, size_t n);
// Reads and drops what the pty reader wrote (gpsd probes, echo).
void feed_drain_pty(struct feed *f);
// Returns the slot of the new client or -1.
int feed_accept(struct feed *f);
// A client became readable: drops the data. Returns 1 at EOF or on an
// error, then the caller closes the client.
int feed_client_input(struct feed *f, int slot);
void feed_close_client(struct feed *f, int slot);
void feed_close(struct feed *f);

#endif
