#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <zmq.h>
#include <sqlite3.h>
#include <liblinht-ctrl.h>

// libm17
#include <m17.h>

float sample;                           // last raw sample from ZMQ
float last[8];                          // look-back buffer for finding syncwords
float dist;                             // Euclidean distance for finding syncwords in the symbol stream
float pld[SYM_PER_PLD];                 // raw frame symbols
uint16_t soft_bit[2 * SYM_PER_PLD];     // raw frame soft bits
uint16_t d_soft_bit[2 * SYM_PER_PLD];   // deinterleaved soft bits

lsf_t lsf;                              // complete LSF
uint8_t frame_data[26];                 // decoded frame data, 206 bits
uint8_t packet_data[33 * 25];           // whole packet data

uint8_t syncd = 0;                      // syncword found?
uint8_t fl = 0;                         // Frame=0 of LSF=1
int8_t last_fn;                         // last received frame number (-1 when idle)
uint8_t pushed;                         // counter for pushed symbols

float det_thresh = 5.0f;
char db_path[128] = "/var/lib/linht/messages.db";

uint16_t last_id;
typedef struct message
{
    uint16_t id;
    uint32_t timestamp;
    char protocol[32];
    char src[64];
    char dst[64];
    char message[1024];
    bool read;
} message_t;
message_t msg;

void print_help(const char *program_name)
{
    printf("M17 text message decoder (with SQLite)\n\n");
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Optional options:\n");
    printf("  -d, --dbase               Set the messages database file path\n");
    printf("  -t, --threshold           Set syncword detection threshold (non-negative, default=5.0)\n");
    printf("  -h, --help                Display this help message and exit\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -t 2.0 -d /var/lib/linht/messages.db\n", program_name);
}

int push_message(char *db_path, message_t msg)
{
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int retval = 0;

    retval = sqlite3_open(db_path, &db);
    if (retval != SQLITE_OK)
    {
        printf("Cannot open database: %s\nExiting.\n", sqlite3_errmsg(db));
        return 1;
    }

    // prepare SQL with placeholders
    const char *sql = "INSERT INTO messages (id, timestamp, protocol, source, destination, message, read) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);";

    retval = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (retval != SQLITE_OK)
    {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    // bind values
    sqlite3_bind_int(stmt, 1, msg.id);                              // id
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)msg.timestamp);      // timestamp (seconds since epoch)
    sqlite3_bind_text(stmt, 3, msg.protocol, -1, SQLITE_STATIC);    // protocol
    sqlite3_bind_text(stmt, 4, msg.src, -1, SQLITE_STATIC);         // source
    sqlite3_bind_text(stmt, 5, msg.dst, -1, SQLITE_STATIC);         // destination
    sqlite3_bind_text(stmt, 6, msg.message, -1, SQLITE_STATIC);     // message
    sqlite3_bind_int(stmt, 7, 0);                                   // read flag (0 = unread)

    // execute
    retval = sqlite3_step(stmt);
    if (retval != SQLITE_DONE)
    {
        printf("Insert failed: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return 1;
    }
    else
    {
        // printf("New message inserted.\n");
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return 0;
}

int main(int argc, char *argv[])
{
    linht_ctrl_green_led_set(false);

    // Define the long options
    static struct option long_options[] =
        {
            {"dbase", required_argument, 0, 'd'},
            {"threshold", required_argument, 0, 't'},
            {"help", no_argument, 0, 'h'},
            {0, 0, 0, 0}};

    // autogenerate the arg list
    char arglist[64] = {0};
    for (uint8_t i = 0; i < sizeof(long_options) / sizeof(struct option) - 1; i++)
    {
        arglist[strlen(arglist)] = long_options[i].val;
        if (long_options[i].has_arg != no_argument)
            arglist[strlen(arglist)] = ':';
    }

    int opt;
    int option_index = 0;

    // Parse command line arguments
    while ((opt = getopt_long(argc, argv, arglist, long_options, &option_index)) != -1)
    {
        switch (opt)
        {
        case 'd':
            if (strlen(optarg) > 0)
            {
                printf("Setting database path to %s\n", db_path);
                strcpy(db_path, optarg);
            }
            else
            {
                printf("No database path given - using default (%s)\n", db_path);
            }
            break;

        case 't':
            if (strlen(optarg) > 0)
            {
                float v = strtof(optarg, NULL);
                if(v>=0.0f)
                {
                    printf("Setting detection threshold to %.1f\n", v);
                    det_thresh = v;
                }
                else
                {
                    printf("Invalid value given for the detection threshold - using default (%.1f)\n", det_thresh);
                }
            }
            else
            {
                printf("No valid value given for the detection threshold - using default (%.1f)\n", det_thresh);
            }
            break;

        case 'h':
            print_help(argv[0]);
            return 0;
            break;
        }
    }

    while (fread((uint8_t *)&sample, 4, 1, stdin) > 0)
    {
        if (!syncd)
        {
            // push new symbol
            for (uint8_t i = 0; i < 7; i++)
            {
                last[i] = last[i + 1];
            }

            last[7] = sample;

            // calculate euclidean norm
            dist = eucl_norm(last, pkt_sync_symbols, 8);

            if (dist < det_thresh) // frame syncword detected
            {
                syncd = 1;
                pushed = 0;
                fl = 0;
            }
            else
            {
                // calculate euclidean norm again, this time against LSF syncword
                dist = eucl_norm(last, lsf_sync_symbols, 8);

                if (dist < det_thresh) // LSF syncword
                {
                    syncd = 1;
                    pushed = 0;
                    last_fn = -1;
                    memset(packet_data, 0, 33 * 25);
                    fl = 1;
                }
            }
        }
        else
        {
            pld[pushed++] = sample;

            if (pushed == SYM_PER_PLD) // frame acquired
            {
                // if it is a frame
                if (!fl)
                {
                    // decode packet frame
                    uint8_t rx_fn, rx_last;
                    decode_pkt_frame(frame_data, &rx_last, &rx_fn, pld);

                    // copy data - might require some fixing
                    if (rx_fn <= 31 && rx_fn == last_fn + 1 && !rx_last)
                    {
                        memcpy(&packet_data[rx_fn * 25], frame_data, 25);
                        last_fn++;
                    }
                    else if (rx_last)
                    {
                        memcpy(&packet_data[(last_fn + 1) * 25], frame_data, rx_fn < 25 ? rx_fn : 25); // prevent copying too much data (beyond frame_data end)
                        uint16_t p_len = strlen((const char *)packet_data);

                        if (CRC_M17(packet_data, p_len + 3) == 0)
                        {
                            // dump data
                            if (packet_data[0] == 0x05) // if a text message
                            {
                                // CRC
                                if (CRC_M17(packet_data, p_len + 3) == 0) // 3: terminating null plus a 2-byte CRC
                                {
                                    strcpy(msg.message, (char*)&packet_data[1]);
                                    msg.timestamp = time(NULL);
                                    msg.id = 0; // hard-coded for now
                                    sprintf(msg.protocol, "M17");
                                    msg.read = 0;

                                    // dump to database
                                    printf("Message from %s: %s\n", msg.src, msg.message);
                                    push_message(db_path, msg);

                                    memset((uint8_t*)&msg, 0, sizeof(message_t));

                                    // blink
                                    linht_ctrl_green_led_set(true);
                                    usleep(100e3);
                                    linht_ctrl_green_led_set(false);
                                }
                            }
                        }
                    }
                }
                else // if it is LSF
                {
                    // decode LSF
                    decode_LSF(&lsf, pld);

                    uint16_t crc = ((uint16_t)lsf.crc[0] << 8) | lsf.crc[1];

                    if (LSF_CRC(&lsf) == crc)
                    {
                        //LSF fields are available here
                        decode_callsign_bytes((uint8_t*)msg.dst, lsf.dst);
                        decode_callsign_bytes((uint8_t*)msg.src, lsf.src);
                    }
                }

                // job done
                syncd = 0;
                pushed = 0;

                for (uint8_t i = 0; i < 8; i++)
                    last[i] = 0.0;
            }
        }
    }
}