#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>
#include <getopt.h>

// Build with gcc sx1255-spi.c -o sx1255-spi -lgpiod -lm

// --- Configuration ---
#define CLK_FREQ (32.0e6f)
#define RST_PIN_OFFSET 537 // The GPIO line offset

const char *spi_device = "/dev/spidev0.0";
const char *gpio_chip_name = "gpiochip0"; // Use `gpiodetect` to verify

// --- Global libgpiod handles ---
struct gpiod_chip *chip_v2;
struct gpiod_line_request *rst_line_request;

// --- Unchanged SPI and SX1255 Functions (Omitted for brevity) ---
uint8_t bits = 8;
uint32_t speed = 500000;
uint8_t mode;

// --- SX1255 ---
typedef enum
{
    RATE_125K,
    RATE_250K,
    RATE_500K
} rate_t;

//default settings
uint32_t rxf = 433.475e6;
uint32_t txf = 438e6;
float mix_gain = 14.0f;  //-9.5dB
int8_t dac_gain = 2;    //-3dB
rate_t rate = RATE_125K;

int spi_init(char *dev) {
    int fd;
    int ret;

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror("Can't open SPI device");
        return fd;
    }

    ret = ioctl(fd, SPI_IOC_WR_MODE, &mode);
    if (ret == -1) {
        perror("Can't set SPI write mode");
        close(fd);
        return -1;
    }
    ret = ioctl(fd, SPI_IOC_RD_MODE, &mode);
    if (ret == -1) {
        perror("Can't set SPI read mode");
        close(fd);
        return -1;
    }

    ret = ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    if (ret == -1) {
        perror("Can't set SPI write bits per word");
        close(fd);
        return -1;
    }
    ret = ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &bits);
    if (ret == -1) {
        perror("Can't set SPI read bits per word");
        close(fd);
        return -1;
    }

    ret = ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    if (ret == -1) {
        perror("Can't set SPI write max speed");
        close(fd);
        return -1;
    }
    ret = ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed);
    if (ret == -1) {
        perror("Can't set SPI read max speed");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

void sx1255_writereg(uint8_t addr, uint8_t val) {
    int fd = open(spi_device, O_RDWR);
    if (fd < 0) {
        perror("sx1255_writereg: open");
        return;
    }
    uint8_t tx[2] = {addr | (1 << 7), val};
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = 0,
        .len = 2,
    };
    ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    close(fd);
    usleep(10000U);
}

uint8_t sx1255_readreg(uint8_t addr) {
    int fd = open(spi_device, O_RDWR);
    if (fd < 0) {
        perror("sx1255_readreg: open");
        return 0;
    }
    uint8_t tx[2] = {addr & ~(1 << 7), 0};
    uint8_t rx[2] = {0, 0};
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = 2,
    };
    ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    close(fd);
    usleep(10000U);
    return rx[1];
}

uint8_t sx1255_set_rx_freq(uint32_t freq) {
    if (freq <= 510e6 && freq >= 410e6) {
        uint32_t val = lround((float)freq * 1048576.0f / CLK_FREQ);
        sx1255_writereg(0x01, (val >> 16) & 0xFF);
        sx1255_writereg(0x02, (val >> 8) & 0xFF);
        sx1255_writereg(0x03, val & 0xFF);
        return 0;
    } else return 1;
}

uint8_t sx1255_set_tx_freq(uint32_t freq) {
    if (freq <= 510e6 && freq >= 410e6) {
        uint32_t val = lround((float)freq * 1048576.0f / CLK_FREQ);
        sx1255_writereg(0x04, (val >> 16) & 0xFF);
        sx1255_writereg(0x05, (val >> 8) & 0xFF);
        sx1255_writereg(0x06, val & 0xFF);
        return 0;
    } else return 1;
}

int8_t sx1255_setrate(rate_t r)
{
    uint8_t n, div;

    switch(r)
    {
        case RATE_125K:
            n = 5;
            div = 2;
        break;

        case RATE_250K:
            n = 4;
            div = 1;
        break;

        case RATE_500K:
            n = 3;
            div = 0;
        break;

        default: //125k
            n = 5;
            div = 2;
        break;
    }

    //interpolation/decimation factor = mant * 3^m * 2^n
    sx1255_writereg(0x13, (0 << 7) | (0 << 6) | (n << 3) | (1 << 2)); 
	
	//mode B2, XTAL/CLK_OUT division factor=2^div
    sx1255_writereg(0x12, (2<<4)|div);

    //return i2s status flag
    return (sx1255_readreg(0x13)&(1<<1))>>1;
}

int gpio_init(void) {
    // Open the GPIO chip
    chip_v2 = gpiod_chip_open(gpio_chip_name);
    if (!chip_v2) {
        perror("Error opening GPIO chip");
        return -1;
    }

    // Create line settings object to configure individual lines
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    if (!settings) {
        perror("Error creating line settings");
        gpiod_chip_close(chip_v2);
        return -1;
    }
    // Configure the settings for our output pin
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, 0); // Initial value is LOW

    // Create a line config object to hold the settings for lines
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        perror("Error creating line config");
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip_v2);
        return -1;
    }
    // Add the settings for our specific line (by offset) to the config
    unsigned int offsets[] = {RST_PIN_OFFSET};
    if (gpiod_line_config_add_line_settings(line_cfg, offsets, 1, settings) != 0) {
        perror("Error adding line settings to config");
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip_v2);
        return -1;
    }

    // Create a request config object for the overall request
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    if (!req_cfg) {
        perror("Error creating request config");
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip_v2);
        return -1;
    }
    gpiod_request_config_set_consumer(req_cfg, "sx1255-reset");

    // Request the lines using the 3-argument function signature
    rst_line_request = gpiod_chip_request_lines(chip_v2, req_cfg, line_cfg);
    
    // Free the config objects as they are no longer needed
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);

    if (!rst_line_request) {
        perror("Error requesting GPIO lines");
        gpiod_chip_close(chip_v2);
        return -1;
    }
    return 0;
}

int rst(uint8_t state) {
    // Set value using the 3-argument function: (request, index, value)
    // Since we requested one line, its index in the request is 0.
    if (gpiod_line_request_set_value(rst_line_request, 0, state) < 0) {
        perror("Error setting GPIO line value");
        return 1;
    }
    return 0;
}

void gpio_cleanup(void) {
    if (rst_line_request) {
        gpiod_line_request_release(rst_line_request);
    }
    if (chip_v2) {
        gpiod_chip_close(chip_v2);
    }
}

void print_help(const char *program_name)
{
    printf("SX1255 config tool\n\n");
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Optional options:\n");
    printf("  -R, --reset           Reset device\n");
    printf("  -s, --rate=RATE       Set sample rate in kHz (125, 250, 500)\n");
    printf("  -r, --rxf=FREQ        Receive frequency (Hz)\n");
    printf("  -t, --txf=FREQ        Transmit frequency (Hz)\n");
    printf("  -d, --dac_gain=GAIN   DAC gain (dB)\n");
    printf("  -m, --mix_gain=GAIN   Mixer gain (dB)\n");
    printf("  -h, --help            Display this help message and exit\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -s 500 -r 433475000 -t 438000000\n", program_name);
}

// --- Main Program Logic ---
int main(int argc, char *argv[])
{
    mode = SPI_MODE_0;

    if(spi_init((char*)spi_device) == 0)
    {
        //printf("SPI config OK\n");
    }
    else
    {
        return -1;
    }

    /*if(gpio_init() != 0)
    {
        fprintf(stderr, "Can not initialize RST pin\nExiting\n");
        return -1;
    }*/

    // Define the long options
    static struct option long_options[] =
    {
        {"reset",       no_argument,       0, 'R'},
        {"rate",        required_argument, 0, 's'},
        {"rxf",         required_argument, 0, 'r'},
        {"txf",         required_argument, 0, 't'},
        {"dac_gain",    required_argument, 0, 'd'},
        {"mix_gain",    required_argument, 0, 'm'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    //autogenerate the arg list
    char arglist[64] = {0};
    for(uint8_t i=0; i<sizeof(long_options)/sizeof(struct option)-1; i++)
    {
        arglist[strlen(arglist)] = long_options[i].val;
        if(long_options[i].has_arg != no_argument)
            arglist[strlen(arglist)] = ':';
    }

    int opt;
    int option_index = 0;

    // Parse command line arguments
    while((opt = getopt_long(argc, argv, arglist, long_options, &option_index)) != -1) 
    {
        switch(opt) 
        {
            case 'R': // reset
                printf("Resetting device...\n");
                /*usleep(100000U);
                rst(1);
                usleep(100000U);
                rst(0);
                usleep(250000U);*/
                gpio_cleanup();
                return 0;
            break;

            case 's': //sample rate
                if(strlen(optarg)>0)
                {
                    uint16_t srate=atoi(optarg);
                    if(srate==125)
                    {
                        printf("Setting sample rate to %d kSa/s\n", srate);
                        rate = RATE_125K;
                    }
                    else if(srate==250)
                    {
                        printf("Setting sample rate to %d kSa/s\n", srate);
                        rate = RATE_250K;
                    }
                    else if(srate==500)
                    {
                        printf("Setting sample rate to %d kSa/s\n", srate);
                        rate = RATE_500K;
                    }
                    else
                    {
                        printf("Unsupported rate setting of %d kSa/s. Using 125 kSa/s\n", srate);
                    }
                }
                else
                {
                    printf("Invalid sample rate.\nExiting.\n");
                    gpio_cleanup();
                    return 1;
                }
            break;

            case 'r': //rx freq
                if(strlen(optarg)>0)
                {
                    rxf=atoi(optarg);
                    printf("Setting RX frequency to %u Hz\n", rxf);
                }
                else
                {
                    printf("Invalid RX frequency.\nExiting.\n");
                    gpio_cleanup();
                    return 1;
                }
            break;

            case 't': //tx freq
                if(strlen(optarg)>0)
                {
                    txf=atoi(optarg);
                    printf("Setting TX frequency to %u Hz\n", txf);
                }
                else
                {
                    printf("Invalid TX frequency.\nExiting.\n");
                    gpio_cleanup();
                    return 1;
                }
            break;

            case 'd': //dac gain
                if(strlen(optarg)>0)
                {
                    dac_gain = atoi(optarg);
                    if(dac_gain==0)
                    {
                        printf("Setting DAC gain to %d dB\n", dac_gain);
                        dac_gain = 3;
                    }
                    else if(dac_gain==-3)
                    {
                        printf("Setting DAC gain to %d dB\n", dac_gain);
                        dac_gain = 2;
                    }
                    else if(dac_gain==-6)
                    {
                        printf("Setting DAC gain to %d dB\n", dac_gain);
                        dac_gain = 1;
                    }
                    else if(dac_gain==-9)
                    {
                        printf("Setting DAC gain to %d dB\n", dac_gain);
                        dac_gain = 0;
                    }
                    else
                    {
                        printf("Unsupported DAC gain setting of %d dB. Using -3 dB.\n", dac_gain);
                    }
                }
                else
                {
                    printf("Invalid DAC gain. Using -3 dB.\n");
                }
            break;
            
            case 'm': //mixer gain
                if(strlen(optarg)>0)
                {
                    mix_gain = atof(optarg);
                    if(mix_gain>=-37.5 && mix_gain<=-7.5)
                    {
                        printf("Setting mixer gain to %.1f dB\n", mix_gain);
                        mix_gain += 37.5f;
                        mix_gain = roundf(mix_gain/2.0f);
                    }
                    else
                    {
                        printf("Invalid mixer gain. Using -9.5 dB.\n");
                    }
                }
                else
                {
                    printf("Invalid mixer gain. Using -9.5 dB.\n");
                }
            break;

            case 'h':
                print_help(argv[0]);
                gpio_cleanup();
                return 0;
            break;
        }
    }

    //device reset
    /*usleep(100000U);
    rst(1);
    usleep(100000U);
    rst(0);
    usleep(250000U);*/

    uint8_t val = sx1255_readreg(0x07);
    printf("SX1255 version 0x%02X", val);
    if (val == 0x11) {
        printf(" - OK\n");
    } else {
        printf(" - ERROR (expected 0x11)\n");
        gpio_cleanup();
        return -1;
    }

    //setting sample rate
    int8_t retval = sx1255_setrate(rate);
    printf("I2S setup %s\n", retval == 0 ? "OK" : "error");
    if(retval!=0)
    {
        printf("Exiting\n");
        gpio_cleanup();
        return -1;
    }
    
    sx1255_set_rx_freq(rxf);

    //printf("Setting TX frequency to %u\n", txf);
    sx1255_set_tx_freq(txf);

    sx1255_writereg(0x0D, (0x01 << 5) | (0x05 << 2) | 0x03);
    sx1255_writereg(0x0E, 0x00);
    sx1255_writereg(0x0C, (0x01 << 5) | (0x0F << 1) | 0x00);

    sx1255_writereg(0x0B, 5);
    sx1255_writereg(0x0A, (0 << 5) | 0);
    sx1255_writereg(0x08, ((uint8_t)dac_gain << 4) | (uint8_t)mix_gain); //mixer and DAC gains

    sx1255_writereg(0x00, (1 << 3) | (1 << 2) | (1 << 1) | 1);

    val = sx1255_readreg(0x11);
    printf("TX PLL %s\n", (val & (1 << 0)) ? "locked" : "unlocked");
    printf("RX PLL %s\n", (val & (1 << 1)) ? "locked" : "unlocked");

    gpio_cleanup();
    return 0;
}
