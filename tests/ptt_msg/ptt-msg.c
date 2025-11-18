#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
// #include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
// #include <time.h>
#include <getopt.h>
#include <zmq.h>
#include <linux/input.h>
#include <arpa/inet.h>
#include <liblinht-ctrl.h>

#define KEY_PRESS 1
#define KEY_RELEASE 0

int kbd;
char *kbd_path = "/dev/input/event0";
char *zmq_ipc = "ipc:///tmp/ptt_msg";

uint8_t sot_pmt[10];
uint8_t eot_pmt[10];
uint8_t pmt_len; // "SOT" and "EOT" PMTs are the same length - single variable is fine

void print_help(const char *program_name)
{
    printf("PTT ZeroMQ message daemon\n\n");
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Optional options:\n");
    printf("  -k, --kbd                 Keyboard device path\n");
    printf("  -u, --usock               ZeroMQ Unix socket\n");
    printf("  -h, --help                Display this help message and exit\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s\n", program_name);
}

int kbd_init(int *fhandle, const char *path)
{
    *fhandle = open(path, O_RDONLY);
    if (*fhandle < 0)
    {
        perror("open keyboard dev");
        return 1;
    }

    return 0;
}

void kbd_cleanup(int fhandle)
{
    close(fhandle);
}

uint8_t string_to_pmt(uint8_t *pmt, const char *msg)
{
    pmt[0] = 2;                                  // pmt type - zmq message
    *((uint16_t *)&pmt[1]) = htons(strlen(msg)); // length
    strcpy((char *)&pmt[3], msg);

    return 3 + strlen(msg);
}

int main(int argc, char *argv[])
{
    int rval = 0;

    // Define the long options
    static struct option long_options[] =
        {
            {"kbd", required_argument, 0, 'k'},
            {"usock", required_argument, 0, 'u'},
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
        case 'k':
            if (strlen(optarg) > 0)
            {
                printf("Setting keyboard device path to %s\n", optarg);
                strcpy(kbd_path, optarg);
            }
            else
            {
                printf("No keyboard device path given - using default (%s)\n", kbd_path);
            }
            break;

        case 'u':
            if (strlen(optarg) > 0)
            {
                printf("Setting ZeroMQ Unix socket to %s\n", optarg);
                strcpy(zmq_ipc, optarg);
            }
            else
            {
                printf("No ZeroMQ Unix socket given - using default (%s)\n", zmq_ipc);
            }
            break;

        case 'h':
            print_help(argv[0]);
            return 0;
            break;
        }
    }

    // init
    if ((rval = kbd_init(&kbd, kbd_path)) != 0)
    {
        return rval;
    }

    // fcntl(kbd, F_SETFL, fcntl(kbd, F_GETFL, 0) | O_NONBLOCK); // non-blocking access

    linht_ctrl_green_led_set(false);
    linht_ctrl_red_led_set(false);

    void *zmq_ctx = zmq_ctx_new();
    void *zmq_pub = zmq_socket(zmq_ctx, ZMQ_PUB);

    if (zmq_bind(zmq_pub, zmq_ipc) != 0)
    {
        printf("ZeroMQ: Error connecting to Unix socket %s.\nExiting.\n", zmq_ipc);
        return 1;
    }

    pmt_len = string_to_pmt(sot_pmt, "SOT");
    string_to_pmt(eot_pmt, "EOT");

    sleep(2); // required by ZMQ

    // main loop
    while (1)
    {
        struct input_event ev;
        ssize_t n = read(kbd, &ev, sizeof(ev));
        if (n == (ssize_t)sizeof(ev))
        {
            if (ev.value == KEY_PRESS)
            {
                if (ev.code == KEY_P)
                {
                    zmq_send(zmq_pub, sot_pmt, pmt_len, 0);
                    linht_ctrl_red_led_set(true);
                    fprintf(stderr, "PTT pressed\n");
                }
                else
                {
                    ;
                }
            }
            else // release
            {
                if (ev.code == KEY_P)
                {
                    zmq_send(zmq_pub, eot_pmt, pmt_len, 0);
                    linht_ctrl_red_led_set(false);
                    fprintf(stderr, "PTT released\n");
                }
                else
                {
                    ;
                }
            }
        }
    }

    // cleanup - TODO: move it elsewhere
    zmq_disconnect(zmq_pub, zmq_ipc);
    zmq_ctx_destroy(&zmq_ctx);
    kbd_cleanup(kbd);
}
