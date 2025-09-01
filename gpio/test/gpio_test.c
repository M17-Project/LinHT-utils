#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>

#define NUM_CHIPS 4
#define LINES_PER_CHIP 32
// Build with gcc gpio_test.c -o gpio_test -lgpiod
// Define your GPIOs here: chip index, line offset, is_output (1=output, 0=input), bias
struct gpio_desc {
    int chip_index;
    int line_offset;
    int is_output;
    int bias;  // 0=disabled, 1=pull-up, 2=pull-down
};

/*
 * MCM-iMX93 GPIO Pin Mapping
 * =========================
 * Format: GPIO_PIN_NAME - Pin Number (Voltage Domain)
 */

/* GPIO Bank 1 - 3.3V Domain */
// GPIO1_IO[4]  - Pin 64  (3.3V)
// GPIO1_IO[6]  - Pin 133 (3.3V)
// GPIO1_IO[12] - Pin 132 (3.3V)
// GPIO1_IO[14] - Pin 131 (3.3V)

/* GPIO Bank 2 - Mixed Voltage Domains */
// GPIO2_IO[0]  - Pin 129 (3.3V)
// GPIO2_IO[1]  - Pin 45  (3.3V)
// GPIO2_IO[2]  - Pin 128 (3.3V)
// GPIO2_IO[3]  - Pin 108 (3.3V)
// GPIO2_IO[4]  - Pin 125 (3.3V) - STATUS LED 1
// GPIO2_IO[5]  - Pin 124 (3.3V) - STATUS LED 2
// GPIO2_IO[6]  - Pin 107 (3.3V)
// GPIO2_IO[7]  - Pin 106 (3.3V)
// GPIO2_IO[8]  - Pin 120 (3.3V)
// GPIO2_IO[9]  - Pin 121 (3.3V)
// GPIO2_IO[10] - Pin 123 (3.3V)
// GPIO2_IO[11] - Pin 122 (3.3V)
// GPIO2_IO[12] - Pin 105 (3.3V)
// GPIO2_IO[13] - Pin 104 (3.3V)
// GPIO2_IO[14] - Pin 102 (3.3V)
// GPIO2_IO[15] - Pin 103 (3.3V)
// GPIO2_IO[16] - Pin 101 (3.3V)
// GPIO2_IO[17] - Pin 100 (3.3V) - SPI1_MCLK
// GPIO2_IO[18] - Pin 118 (3.3V) - PTT
// GPIO2_IO[19] - Pin 119 (3.3V) - 
// GPIO2_IO[20] - Pin 116 (3.3V)
// GPIO2_IO[21] - Pin 117 (3.3V)
// GPIO2_IO[22] - Pin 115 (3.3V)
// GPIO2_IO[23] - Pin 96  (3.3V)
// GPIO2_IO[24] - Pin 99  (3.3V)
// GPIO2_IO[25] - Pin 95  (3.3V)
// GPIO2_IO[26] - Pin 47  (3.3V/1.8V)
// GPIO2_IO[27] - Pin 48  (3.3V/1.8V)
// GPIO2_IO[28] - Pin 98  (3.3V/1.8V)
// GPIO2_IO[29] - Pin 97  (3.3V/1.8V)

/* GPIO Bank 3 - Mixed Voltage Domains */
// GPIO3_IO[0]  - Pin 74  (3.3V/1.8V)
// GPIO3_IO[1]  - Pin 70  (3.3V/1.8V)
// GPIO3_IO[2]  - Pin 69  (3.3V/1.8V)
// GPIO3_IO[3]  - Pin 72  (3.3V/1.8V)
// GPIO3_IO[4]  - Pin 73  (1.8V)
// GPIO3_IO[5]  - Pin 68  (1.8V)
// GPIO3_IO[6]  - Pin 67  (1.8V)
// GPIO3_IO[7]  - Pin 65  (1.8V)
// GPIO3_IO[20] - Pin 109 (1.8V)
// GPIO3_IO[21] - Pin 113 (1.8V)
// GPIO3_IO[22] - Pin 114 (1.8V)
// GPIO3_IO[23] - Pin 112 (1.8V)
// GPIO3_IO[24] - Pin 110 (1.8V)
// GPIO3_IO[25] - Pin 111 (1.8V)
// GPIO3_IO[28] - Pin 77  (1.8V)
// GPIO3_IO[29] - Pin 78  (1.8V)
// GPIO3_IO[30] - Pin 79  (1.8V)
// GPIO3_IO[31] - Pin 80  (1.8V)

/* GPIO Bank 4 - 1.8V Domain */
// GPIO4_IO[0]  - Pin 94  (1.8V)
// GPIO4_IO[1]  - Pin 93  (1.8V)
// GPIO4_IO[2]  - Pin 85  (1.8V)
// GPIO4_IO[3]  - Pin 83  (1.8V)
// GPIO4_IO[4]  - Pin 82  (1.8V)
// GPIO4_IO[5]  - Pin 84  (1.8V)
// GPIO4_IO[6]  - Pin 81  (1.8V)
// GPIO4_IO[7]  - Pin 86  (1.8V)
// GPIO4_IO[8]  - Pin 87  (1.8V)
// GPIO4_IO[9]  - Pin 89  (1.8V)
// GPIO4_IO[10] - Pin 88  (1.8V)
// GPIO4_IO[11] - Pin 90  (1.8V)
// GPIO4_IO[12] - Pin 91  (1.8V)
// GPIO4_IO[13] - Pin 92  (1.8V)
// GPIO4_IO[14] - Pin 50  (1.8V)
// GPIO4_IO[15] - Pin 49  (1.8V)
// GPIO4_IO[16] - Pin 51  (1.8V)
// GPIO4_IO[17] - Pin 55  (1.8V)
// GPIO4_IO[18] - Pin 53  (1.8V)
// GPIO4_IO[19] - Pin 52  (1.8V)
// GPIO4_IO[20] - Pin 54  (1.8V)
// GPIO4_IO[21] - Pin 56  (1.8V)
// GPIO4_IO[22] - Pin 58  (1.8V)
// GPIO4_IO[23] - Pin 57  (1.8V)
// GPIO4_IO[24] - Pin 59  (1.8V)
// GPIO4_IO[25] - Pin 60  (1.8V)
// GPIO4_IO[26] - Pin 61  (1.8V)
// GPIO4_IO[27] - Pin 62  (1.8V)

/*
 * Notes:
 * - All pins are general-purpose input/output capable
 * - 3.3V/1.8V pins support dual voltage domains
 * - Reference: MCM-iMX93 Reference Guide (April 2025)
 * 
 * Bias Configuration:
 * - 0 = DISABLED (floating, high impedance)
 * - 1 = PULL_UP (internal pull-up resistor enabled)
 * - 2 = PULL_DOWN (internal pull-down resistor enabled)
 */

 /*
    sh-5.2# gpiodetect
    gpiochip0 [43810000.gpio] (32 lines) -> gpio2: gpio@43810000
    gpiochip1 [43820000.gpio] (32 lines) -> gpio3: gpio@43820000
    gpiochip2 [43830000.gpio] (32 lines) -> gpio4: gpio@43830000
    gpiochip3 [47400000.gpio] (32 lines) -> gpio1: gpio@47400000
 */

struct gpio_desc gpio_list[] = {
    //{0, 18, 0, 2},  // gpiochip2, line 18, input,  pull-up   -> GPIO2_IO[18] - Pin 118 - PTT
    {0,  4, 1, 1},  // gpiochip0, line  4, output, high  -> GPIO2_IO[4]  - Pin  125 - LED1
    {0,  5, 1, 1},  // gpiochip0, line  5, output, high  -> GPIO2_IO[5]  - Pin  124 - LED2
	{1,  5, 1, 0}, // ~{MIC_MUTE} - GPIO3_IO[5]
	{0, 13, 1, 0} //RF_SW_CTRL - GPIO2_IO[13]
	//{1,  7, 1, 0},  // gpiochip1, line  7, output, disabled  -> GPIO3_IO[7]  - Pin  65 - 3.3V regulator
    // Add more as needed...
};

#define NUM_GPIOS ((int)(sizeof(gpio_list)/sizeof(struct gpio_desc)))

struct gpio_handle {
    struct gpiod_chip *chip;
    struct gpiod_line_request *request;
    int is_output;
    int failed;
    int line_offset;
    int current_state;  // For tracking output state
    int bias;          // Pull-up/pull-down configuration
};

// Global variables
struct gpio_handle handles[NUM_GPIOS];
struct termios orig_termios;

// Function prototypes
int setup_keyboard(void);
void restore_keyboard(void);
int kbhit(void);
int getch(void);
void toggle_outputs(void);
void read_inputs(void);
void print_help(void);
const char* get_bias_string(int bias);

// Setup non-blocking keyboard input
int setup_keyboard(void) {
    struct termios new_termios;
    
    // Get current terminal settings
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) {
        perror("tcgetattr");
        return -1;
    }
    
    // Configure new terminal settings
    new_termios = orig_termios;
    new_termios.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    new_termios.c_cc[VMIN] = 0;   // Minimum number of characters to read
    new_termios.c_cc[VTIME] = 0;  // Timeout in tenths of seconds
    
    // Apply new settings
    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_termios) < 0) {
        perror("tcsetattr");
        return -1;
    }
    
    return 0;
}

// Restore original keyboard settings
void restore_keyboard(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

// Check if a key has been pressed (non-blocking using select())
int kbhit(void) {
    struct timeval tv = {0L, 0L};  // No timeout - return immediately
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) > 0;
}

// Get a character (this will only be called after kbhit() returns true)
int getch(void) {
    return getchar();
}

// Get bias configuration string
const char* get_bias_string(int bias) {
    switch(bias) {
        case 0: return "disabled";
        case 1: return "pull-up";
        case 2: return "pull-down";
        default: return "unknown";
    }
}

// Toggle all output GPIOs
void toggle_outputs(void) {
    enum gpiod_line_value values[1];
    int i;
    
    printf("Toggling outputs...\n");
    for (i = 0; i < NUM_GPIOS; i++) {
        if (handles[i].failed || !handles[i].is_output)
            continue;
            
        // Toggle the current state
        handles[i].current_state = !handles[i].current_state;
        values[0] = handles[i].current_state ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
        
        if (gpiod_line_request_set_values(handles[i].request, values) < 0) {
            fprintf(stderr, "Failed to set value on chip%d line%d\n",
                    gpio_list[i].chip_index, gpio_list[i].line_offset);
        } else {
            printf("  chip%d line%d: %s\n", 
                   gpio_list[i].chip_index, 
                   gpio_list[i].line_offset,
                   handles[i].current_state ? "HIGH" : "LOW");
        }
    }
}

// Read all input GPIOs
void read_inputs(void) {
    enum gpiod_line_value values[1];
    int i;
    
    printf("Reading inputs...\n");
    for (i = 0; i < NUM_GPIOS; i++) {
        if (handles[i].failed || handles[i].is_output)
            continue;
            
        if (gpiod_line_request_get_values(handles[i].request, values) < 0) {
            fprintf(stderr, "Failed to read value from chip%d line%d\n",
                    gpio_list[i].chip_index, gpio_list[i].line_offset);
        } else {
            printf("  chip%d line%d (%s): %s\n", 
                   gpio_list[i].chip_index, 
                   gpio_list[i].line_offset,
                   get_bias_string(handles[i].bias),
                   (values[0] == GPIOD_LINE_VALUE_ACTIVE) ? "HIGH" : "LOW");
        }
    }
}

// Print help information
void print_help(void) {
    printf("\n=== GPIO Interactive Control ===\n");
    printf("Commands:\n");
    printf("  't' - Toggle all outputs\n");
    printf("  'r' - Read all inputs\n");
    printf("  'h' - Show this help\n");
    printf("  'q' - Quit program\n");
    printf("================================\n\n");
}

int main(void) {
    char chip_name[32];
    int i;
    struct gpiod_request_config *req_cfg;
    struct gpiod_line_config *line_cfg;
    struct gpiod_line_settings *line_settings;
    unsigned int offsets[1];
    int running = 1;
    char key;

    printf("GPIO Interactive Control Program with Pull-up/Pull-down Support\n");
    printf("Initializing GPIOs...\n");

    // Initialize all handles
    for (i = 0; i < NUM_GPIOS; i++) {
        handles[i].failed = 0;
        handles[i].line_offset = gpio_list[i].line_offset;
        handles[i].is_output = gpio_list[i].is_output;
        handles[i].bias = gpio_list[i].bias;
        handles[i].current_state = 0;  // Start with LOW
        
        snprintf(chip_name, sizeof(chip_name), "/dev/gpiochip%d", gpio_list[i].chip_index);
        handles[i].chip = gpiod_chip_open(chip_name);
        if (!handles[i].chip) {
            fprintf(stderr, "Failed to open chip %s\n", chip_name);
            handles[i].failed = 1;
            continue;
        }

        // Create request configuration
        req_cfg = gpiod_request_config_new();
        if (!req_cfg) {
            fprintf(stderr, "Failed to create request config\n");
            handles[i].failed = 1;
            gpiod_chip_close(handles[i].chip);
            continue;
        }
        gpiod_request_config_set_consumer(req_cfg, "gpiod2-interactive");

        // Create line configuration
        line_cfg = gpiod_line_config_new();
        if (!line_cfg) {
            fprintf(stderr, "Failed to create line config\n");
            handles[i].failed = 1;
            gpiod_request_config_free(req_cfg);
            gpiod_chip_close(handles[i].chip);
            continue;
        }

        // Create line settings
        line_settings = gpiod_line_settings_new();
        if (!line_settings) {
            fprintf(stderr, "Failed to create line settings\n");
            handles[i].failed = 1;
            gpiod_line_config_free(line_cfg);
            gpiod_request_config_free(req_cfg);
            gpiod_chip_close(handles[i].chip);
            continue;
        }

        // Configure line settings
        if (handles[i].is_output) {
            gpiod_line_settings_set_direction(line_settings, GPIOD_LINE_DIRECTION_OUTPUT);
            gpiod_line_settings_set_output_value(line_settings, GPIOD_LINE_VALUE_INACTIVE);
        } else {
            gpiod_line_settings_set_direction(line_settings, GPIOD_LINE_DIRECTION_INPUT);
            
            // Configure pull-up/pull-down based on bias setting
            switch(handles[i].bias) {
                case 1:  // Pull-up
                    gpiod_line_settings_set_bias(line_settings, GPIOD_LINE_BIAS_PULL_UP);
                    break;
                case 2:  // Pull-down
                    gpiod_line_settings_set_bias(line_settings, GPIOD_LINE_BIAS_PULL_DOWN);
                    break;
                default: // Disabled/floating
                    gpiod_line_settings_set_bias(line_settings, GPIOD_LINE_BIAS_DISABLED);
                    break;
            }
        }

        // Add line settings to line config
        offsets[0] = gpio_list[i].line_offset;
        if (gpiod_line_config_add_line_settings(line_cfg, offsets, 1, line_settings) < 0) {
            fprintf(stderr, "Failed to add line settings for line %d on %s\n", 
                    gpio_list[i].line_offset, chip_name);
            handles[i].failed = 1;
            gpiod_line_settings_free(line_settings);
            gpiod_line_config_free(line_cfg);
            gpiod_request_config_free(req_cfg);
            gpiod_chip_close(handles[i].chip);
            continue;
        }

        // Request the line
        handles[i].request = gpiod_chip_request_lines(handles[i].chip, req_cfg, line_cfg);
        if (!handles[i].request) {
            fprintf(stderr, "Failed to request line %d on %s\n", 
                    gpio_list[i].line_offset, chip_name);
            handles[i].failed = 1;
        }

        // Clean up config objects
        gpiod_line_settings_free(line_settings);
        gpiod_line_config_free(line_cfg);
        gpiod_request_config_free(req_cfg);
        
        if (handles[i].failed) {
            gpiod_chip_close(handles[i].chip);
            continue;
        }
    }

    // Print status
    printf("\nGPIO status:\n");
    for (i = 0; i < NUM_GPIOS; i++) {
        printf("  chip%d line%d %s (%s): %s\n",
            gpio_list[i].chip_index,
            gpio_list[i].line_offset,
            gpio_list[i].is_output ? "output" : "input",
            gpio_list[i].is_output ? "n/a" : get_bias_string(gpio_list[i].bias),
            handles[i].failed ? "FAILED" : "OK");
    }

    // Setup keyboard input
    if (setup_keyboard() < 0) {
        fprintf(stderr, "Failed to setup keyboard input\n");
        goto cleanup;
    }

    print_help();

    // Main interactive loop
    printf("Ready for commands (press 'h' for help):\n");
    while (running) {
        if (kbhit()) {
            key = getch();
            
            switch (key) {
                case 't':
                case 'T':
                    toggle_outputs();
                    break;
                    
                case 'r':
                case 'R':
                    read_inputs();
                    break;
                    
                case 'h':
                case 'H':
                    print_help();
                    break;
                    
                case 'q':
                case 'Q':
                    printf("Exiting...\n");
                    running = 0;
                    break;
                    
                default:
                    printf("Unknown command '%c'. Press 'h' for help.\n", key);
                    break;
            }
        }
        
        // Small delay to prevent excessive CPU usage
        usleep(10000);  // 10ms
    }

    // Restore keyboard settings
    restore_keyboard();

cleanup:
    // Clean up GPIO resources
    for (i = 0; i < NUM_GPIOS; i++) {
        if (!handles[i].failed) {
            gpiod_line_request_release(handles[i].request);
            gpiod_chip_close(handles[i].chip);
        }
    }

    printf("Program terminated.\n");
    return 0;
}
