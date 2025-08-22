/*
 * GPIO control - flashlight
 * MX93_PAD_GPIO_IO18__GPIO2_IO18
 */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <gpiod.h>

struct gpiod_chip *chip_v2;
struct gpiod_line_request *light_line_request;

const char *gpio_chip_path = "/dev/gpiochip0";
const uint16_t gpio_pin_offset = 18;

int gpio_init(const char* appname)
{
    // Open the GPIO chip
    chip_v2 = gpiod_chip_open(gpio_chip_path);
    if(!chip_v2)
    {
        fprintf(stderr, "Error opening GPIO chip '%s': %s\n", gpio_chip_path, strerror(errno));
        return -1;
    }

    // Create line settings object to configure individual lines
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    if(!settings)
    {
        perror("Error creating line settings");
        gpiod_chip_close(chip_v2);
        return -1;
    }
    // Configure the settings for our output pin
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, 0); // Initial value is LOW

    // Create a line config object to hold the settings for lines
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    if(!line_cfg)
    {
        perror("Error creating line config");
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip_v2);
        return -1;
    }
    // Add the settings for our specific line (by offset) to the config
    unsigned int offsets[] = {gpio_pin_offset};
    if(gpiod_line_config_add_line_settings(line_cfg, offsets, 1, settings) != 0)
    {
        fprintf(stderr, "Error adding line settings to config for pin %d: %s\n",
                gpio_pin_offset, strerror(errno));
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip_v2);
        return -1;
    }

    // Create a request config object for the overall request
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    if(!req_cfg)
    {
        perror("Error creating request config");
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip_v2);
        return -1;
    }
    gpiod_request_config_set_consumer(req_cfg, appname);

    // Request the lines using the 3-argument function signature
    light_line_request = gpiod_chip_request_lines(chip_v2, req_cfg, line_cfg);
    
    // Free the config objects as they are no longer needed
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);

    if(!light_line_request)
    {
        fprintf(stderr, "Error requesting GPIO lines for pin %d: %s\n",
                gpio_pin_offset, strerror(errno));
        gpiod_chip_close(chip_v2);
        return -1;
    }
    return 0;
}

int flash(uint8_t state)
{
    // Set value using gpiod V2 API
    enum gpiod_line_value values[] = {state ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE};

    if(gpiod_line_request_set_values(light_line_request, values) < 0)
    {
        fprintf(stderr, "Error setting GPIO line value for pin %d to state %d: %s\n",
                gpio_pin_offset, state, strerror(errno));
        return 1;
    }

    return 0;
}

void gpio_cleanup(void)
{
    if(light_line_request)
	{
        gpiod_line_request_release(light_line_request);
    }
    if(chip_v2)
	{
        gpiod_chip_close(chip_v2);
    }
}

int main(int argc, char* argv[])
{
	gpio_init(argv[0]);

	if(argc==1) //no args given
	{
		flash(0);
	}
	else
	{
		if(argv[1][0]=='0')
		{
			flash(0);
		}
		else
		{
			flash(1);
		}
	}
	
	gpio_cleanup();

	return 0;
}
