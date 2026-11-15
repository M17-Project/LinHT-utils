// Build with gcc sx1255-spi.c -o sx1255-spi -lm -lsx1255

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>

#include <sx1255.h>

// --- Configuration ---
const uint16_t rst_pin_offset = 22;
const char *spi_device = "/dev/spidev0.0";
const char *gpio_chip_path = "/dev/gpiochip0"; // Use device path for gpiod V2

// settings
uint32_t rxf;
uint32_t txf;
float mix_gain;
int8_t dac_gain;
uint8_t lna_gain;
uint8_t pga_gain;
uint16_t pll_bw;
sx1255_rate_t rate;
uint8_t addr;
uint8_t val;

// SX1255 RF range (datasheet), the frequency register overflows above 512 MHz
#define FREQ_MIN 400000000L
#define FREQ_MAX 510000000L

// Parse a whole string as an integer, false on garbage or overflow
static bool parse_long(const char *s, int base, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, base);
    if (errno != 0 || end == s || *end != '\0')
        return false;
    *out = v;
    return true;
}

static bool parse_float(const char *s, float *out)
{
    char *end;
    errno = 0;
    float v = strtof(s, &end);
    if (errno != 0 || end == s || *end != '\0')
        return false;
    *out = v;
    return true;
}

void print_help(const char *program_name)
{
    printf("SX1255 config tool\n\n");
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Optional options:\n");
    printf("  -E, --reset               Reset device\n");
    printf("  -s, --rate=RATE           Set sample rate in kHz (125, 250, 500)\n");
    printf("  -r, --rxf=FREQ            Receive frequency (Hz)\n");
    printf("  -t, --txf=FREQ            Transmit frequency (Hz)\n");
    printf("  -l, --lna_gain=GAIN       RX LNA gain (0..48 dB)\n");
    printf("  -p, --pga_gain=GAIN       RX PGA gain (0..30 dB)\n");
    printf("  -d, --dac_gain=GAIN       TX DAC gain (-9, -6, -3, 0 dB)\n");
    printf("  -m, --mix_gain=GAIN       TX mixer gain (-37.5..-7.5 dB)\n");
    printf("  -a  --rx_pll_bw=VAL       RX PLL bandwidth in kHz (75, 150, 225, 300)\n");
    printf("  -b  --tx_pll_bw=VAL       TX PLL bandwidth in kHz (75, 150, 225, 300)\n");
    printf("  -T, --tx_ena=VAL          TX path enable (0/1)\n");
    printf("  -R, --rx_ena=VAL          RX path enable (0/1)\n");
    printf("  -P, --pll_flags           Get PLL lock flags\n");
    printf("  -L, --rf_loop=VAL         Internal RF loopback enable (0/1)\n");
    printf("  -G, --get_reg=ADDR        Get register value (dec or hex address)\n");
    printf("  -S, --set_reg=ADDR,VAL    Set register value (hex address and value)\n");
    printf("  -h, --help                Display this help message and exit\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -E -s 500 -r 433475000 -l 24 -p 24 -R 1 -P\n", program_name);
}

// --- Main Program Logic ---
int main(int argc, char *argv[])
{
    if (sx1255_init(spi_device, gpio_chip_path, rst_pin_offset) != 0)
    {
        fprintf(stderr, "Can not initialize device\nExiting\n");
        return -1;
    }

    uint8_t val = sx1255_get_chip_version();
    printf("Detected SX1255 chip version V%d%c", (val >> 4) & 0xF, 'A' + (val & 0xF) - 1);
    if (val == 0x11)
    {
        printf(" - OK\n");
    }
    else
    {
        printf(" - WARNING (expected V1A)\n");
    }

    // Define the long options
    static struct option long_options[] =
    {
        {"reset", no_argument, 0, 'E'},
        {"rate", required_argument, 0, 's'},
        {"rxf", required_argument, 0, 'r'},
        {"txf", required_argument, 0, 't'},
        {"lna_gain", required_argument, 0, 'l'},
        {"pga_gain", required_argument, 0, 'p'},
        {"dac_gain", required_argument, 0, 'd'},
        {"mix_gain", required_argument, 0, 'm'},
        {"rx_pll_bw", required_argument, 0, 'a'},
        {"tx_pll_bw", required_argument, 0, 'b'},
        {"tx_ena", required_argument, 0, 'T'},
        {"rx_ena", required_argument, 0, 'R'},
        {"pll_flags", no_argument, 0, 'P'},
        {"rf_loop", required_argument, 0, 'L'},
        {"get_reg", required_argument, 0, 'G'},
        {"set_reg", required_argument, 0, 'S'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

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
        // reset
        case 'E':
            printf("Resetting device... ");
            sx1255_reset();
            printf("completed\n");
            break;

        // sample rate
        case 's':
            if (strlen(optarg) > 0)
            {
                long srate = 0;
                parse_long(optarg, 10, &srate);
                if (srate == 125)
                {
                    printf("Setting sample rate to %ld kSa/s\n", srate);
                    rate = SX1255_RATE_125K;
                }
                else if (srate == 250)
                {
                    printf("Setting sample rate to %ld kSa/s\n", srate);
                    rate = SX1255_RATE_250K;
                }
                else if (srate == 500)
                {
                    printf("Setting sample rate to %ld kSa/s\n", srate);
                    rate = SX1255_RATE_500K;
                }
                else
                {
                    printf("Unsupported rate setting of %s kSa/s. Using 125 kSa/s\n", optarg);
                    rate = SX1255_RATE_125K;
                }
            }
            else
            {
                printf("Invalid sample rate. Using 125 kHz.\n");
                rate = SX1255_RATE_125K;
            }

            int8_t retval = sx1255_set_rate(rate);
            printf("I2S setup %s\n", retval == 0 ? "OK" : "error");
            if (retval != 0)
            {
                printf("Exiting\n");
                sx1255_cleanup();
                return -1;
            }
            break;

        // rx freq
        case 'r':
            {
                long f;
                if (!parse_long(optarg, 10, &f) || f < FREQ_MIN || f > FREQ_MAX)
                {
                    printf("Invalid RX frequency '%s'. Allowed %ld..%ld Hz, not changed.\n",
                           optarg, FREQ_MIN, FREQ_MAX);
                    break;
                }
                rxf = (uint32_t)f;
                printf("Setting RX frequency to %u Hz\n", rxf);
                if (sx1255_set_rx_freq(rxf) != 0)
                    printf("RX frequency write failed or the PLL did not lock\n");
            }
            break;

        // tx freq
        case 't':
            {
                long f;
                if (!parse_long(optarg, 10, &f) || f < FREQ_MIN || f > FREQ_MAX)
                {
                    printf("Invalid TX frequency '%s'. Allowed %ld..%ld Hz, not changed.\n",
                           optarg, FREQ_MIN, FREQ_MAX);
                    break;
                }
                txf = (uint32_t)f;
                printf("Setting TX frequency to %u Hz\n", txf);
                if (sx1255_set_tx_freq(txf) != 0)
                    printf("TX frequency write failed or the PLL did not lock\n");
            }
            break;
    
        // lna gain
        case 'l':
            if (strlen(optarg) > 0)
            {
                long g;
                if (parse_long(optarg, 10, &g) && g >= 0 && g <= 48)
                {
                    lna_gain = (uint8_t)g;
                    printf("Setting LNA gain to %d dB\n", lna_gain);
                }
                else
                {
                    printf("Invalid LNA gain. Using 48 dB.\n");
                    lna_gain = 48;
                }
            }
            else
            {
                printf("Missing LNA gain. Using 48 dB.\n");
                lna_gain = 48;
            }
            sx1255_set_lna_gain(lna_gain);
            break;

        // pga gain
        case 'p':
            if (strlen(optarg) > 0)
            {
                long g;
                if (parse_long(optarg, 10, &g) && g >= 0 && g <= 30)
                {
                    pga_gain = (uint8_t)g;
                    printf("Setting PGA gain to %d dB\n", pga_gain);
                }
                else
                {
                    printf("PGA gain out of range. Using 30 dB.\n");
                    pga_gain = 30;
                }
            }
            else
            {
                printf("Missing PGA gain. Using 30 dB.\n");
                pga_gain = 30;
            }
            sx1255_set_pga_gain(pga_gain);
            break;

        // dac gain
        case 'd':
            if (strlen(optarg) > 0)
            {
                long g;
                if (parse_long(optarg, 10, &g) && (g == 0 || g == -3 || g == -6 || g == -9))
                {
                    dac_gain = (int8_t)g;
                    printf("Setting DAC gain to %d dB\n", dac_gain);
                }
                else
                {
                    printf("Unsupported DAC gain setting of %s dB. Using -3 dB.\n", optarg);
                    dac_gain = -3;
                }
            }
            else
            {
                printf("Missing DAC gain. Using -3 dB.\n");
                dac_gain = -3;
            }
            sx1255_set_dac_gain(dac_gain);
            break;

        // mixer gain
        case 'm':
            if (strlen(optarg) > 0)
            {
                if (parse_float(optarg, &mix_gain) && mix_gain >= -37.5 && mix_gain <= -7.5)
                {
                    printf("Setting mixer gain to %.1f dB\n", mix_gain);
                }
                else
                {
                    printf("Invalid mixer gain. Using -9.5 dB.\n");
                    mix_gain = -9.5;
                }
            }
            else
            {
                printf("Missing mixer gain. Using -9.5 dB.\n");
                mix_gain = -9.5;
            }
            sx1255_set_mixer_gain(mix_gain);
            break;

        // rx pll bw
        case 'a':
            if (strlen(optarg) > 0)
            {
                long bw;
                if (parse_long(optarg, 10, &bw) && bw >= 75 && bw <= 300)
                {
                    pll_bw = (uint16_t)bw;
                    printf("Setting RX PLL bandwidth to %d kHz\n", (pll_bw/75)*75);
                }
                else
                {
                    printf("Invalid RX PLL bandwidth. Using 75 kHz.\n");
                    pll_bw = 75;
                }
            }
            else
            {
                printf("Missing RX PLL bandwidth. Using 75 kHz.\n");
                pll_bw = 75;
            }
            sx1255_set_rx_pll_bw(pll_bw);
            break;

        // tx pll bw
        case 'b':
            if (strlen(optarg) > 0)
            {
                long bw;
                if (parse_long(optarg, 10, &bw) && bw >= 75 && bw <= 300)
                {
                    pll_bw = (uint16_t)bw;
                    printf("Setting TX PLL bandwidth to %d kHz\n", (pll_bw/75)*75);
                }
                else
                {
                    printf("Invalid TX PLL bandwidth. Using 75 kHz.\n");
                    pll_bw = 75;
                }
            }
            else
            {
                printf("Missing TX PLL bandwidth. Using 75 kHz.\n");
                pll_bw = 75;
            }
            sx1255_set_tx_pll_bw(pll_bw);
            break;

        // enable/disable TX front end
        case 'T':
            if (strlen(optarg) > 0)
            {
                val = (atoi(optarg) != 0);
                if (val == 0)
                {
                    sx1255_enable_tx(false);
                    printf("Disabling TX path.\n");
                }
                else
                {
                    sx1255_enable_tx(true);
                    printf("Enabling TX path.\n");
                }
            }
            else
            {
                printf("Missing TX enable parameter.\n");
            }
            break;

        // enable/disable RX front end
        case 'R':
            if (strlen(optarg) > 0)
            {
                val = (atoi(optarg) != 0);
                if (val == 0)
                {
                    sx1255_enable_rx(false);
                    printf("Disabling RX path.\n");
                }
                else
                {
                    sx1255_enable_rx(true);
                    printf("Enabling RX path.\n");
                }
            }
            else
            {
                printf("Missing RX enable parameter.\n");
            }
            break;

        // get PLL lock flags
        case 'P':
            val = sx1255_read_reg(0x11);
            printf("TX PLL %s\n", (val & (1 << 0)) ? "locked" : "unlocked");
            printf("RX PLL %s\n", (val & (1 << 1)) ? "locked" : "unlocked");
            break;

        // enable/disable internal RF loopback
        case 'L':
            if (strlen(optarg) > 0)
            {
                val = (atoi(optarg) != 0);
                if (val == 0)
                {
                    sx1255_enable_rf_loopback(false);
                    printf("Disabling RF loopback.\n");
                }
                else
                {
                    sx1255_enable_rf_loopback(true);
                    printf("Enabling RF loopback.\n");
                }
            }
            else
            {
                printf("Missing RF loopback enable parameter.\n");
            }
            break;

        // get register value
        case 'G':
            if (strlen(optarg) > 0)
            {
                long a;
                if (parse_long(optarg, 0, &a) && a >= 0 && a <= 0x13)
                {
                    addr = (uint8_t)a;
                    printf("Register 0x%02X value: 0x%02X\n", addr, sx1255_read_reg(addr));
                }
                else
                    printf("Register readout error: address out of range.\n");
            }
            else
            {
                printf("Register readout error: missing register address.\n");
            }
            break;

        // set register value (=hex,hex)
        case 'S':
            if (strlen(optarg) > 0)
            {
                char *comma = strchr(optarg, ',');
                long a, v;
                if (!comma)
                {
                    printf("Register write error: expected ADDR,VAL.\n");
                    break;
                }
                *comma = '\0';
                if (!parse_long(optarg, 16, &a) || !parse_long(comma + 1, 16, &v) || v < 0 || v > 0xFF)
                {
                    printf("Register write error: invalid address or value.\n");
                    break;
                }

                if (a >= 0 && a <= 0x13)
                {
                    addr = (uint8_t)a;
                    val = (uint8_t)v;
                    printf("Setting register 0x%02X to 0x%02X\n", addr, val);
                    sx1255_write_reg(addr, val);
                }
                else
                    printf("Register write error: address out of range.\n");
            }
            else
            {
                printf("Register write error: missing params.\n");
            }
            break;

        // help
        case 'h':
            print_help(argv[0]);
            sx1255_cleanup();
            return 0;
            break;
        }
    }

    sx1255_cleanup();
    return 0;
}
