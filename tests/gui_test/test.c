#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <time.h>
#include <raylib.h>
#include <linux/input.h>
#include <zmq.h>
#include <arpa/inet.h>
#include <sx1255.h>
#include <liblinht-ctrl.h>
#include <cyaml/cyaml.h>
#include "settings.h"

#define RES_X 160
#define RES_Y 128

// keymap states
#define KEY_PRESS 1
#define KEY_RELEASE 0

// return value
int rval;

// SX1255
const uint16_t rst_pin_offset = 22;
const char *spi_device = "/dev/spidev0.0";
const char *gpio_chip_path = "/dev/gpiochip0";

// screen
int fb;				   // framebuffer file handle
size_t ssize;		   // screen size
uint32_t *framebuffer; // framebuffer

// keyboard
const char *kbd_path = "/dev/input/event0";
int kbd; // keyboard file handle

// ZeroMQ and PMT
char *zmq_ipc = "ipc:///tmp/ptt_msg";

uint8_t sot_pmt[10];
uint8_t eot_pmt[10];
uint8_t pmt_len; // "SOT" and "EOT" PMTs are the same length - single variable is fine

// settings
cyaml_schema_field_t ui_fields[] =
{
	CYAML_FIELD_BOOL("backlight", CYAML_FLAG_DEFAULT, ui_t, backlight),
	CYAML_FIELD_UINT("brightness", CYAML_FLAG_DEFAULT, ui_t, brightness),
	CYAML_FIELD_UINT("timeout", CYAML_FLAG_DEFAULT, ui_t, timeout),
	CYAML_FIELD_END
};

cyaml_schema_field_t rf_fields[] =
{
	CYAML_FIELD_FLOAT("freq_corr", CYAML_FLAG_DEFAULT, rf_settings_t, freq_corr),
	CYAML_FIELD_BOOL("calibrated", CYAML_FLAG_DEFAULT, rf_settings_t, calibrated),
	CYAML_FIELD_FLOAT("i_dc", CYAML_FLAG_DEFAULT, rf_settings_t, i_dc),
	CYAML_FIELD_FLOAT("q_dc", CYAML_FLAG_DEFAULT, rf_settings_t, q_dc),
	CYAML_FIELD_FLOAT("iq_bal", CYAML_FLAG_DEFAULT, rf_settings_t, iq_bal),
	CYAML_FIELD_FLOAT("iq_crosstalk", CYAML_FLAG_DEFAULT, rf_settings_t, iq_crosstalk),
	CYAML_FIELD_STRING_PTR("dpd_type", CYAML_FLAG_POINTER, rf_settings_t, dpd_type, 0, CYAML_UNLIMITED),
	CYAML_FIELD_FLOAT("dpd_0", CYAML_FLAG_DEFAULT, rf_settings_t, dpd_0),
	CYAML_FIELD_FLOAT("dpd_1", CYAML_FLAG_DEFAULT, rf_settings_t, dpd_1),
	CYAML_FIELD_FLOAT("dpd_2", CYAML_FLAG_DEFAULT, rf_settings_t, dpd_2),
	CYAML_FIELD_BOOL("bias_t", CYAML_FLAG_DEFAULT, rf_settings_t, bias_t),
	CYAML_FIELD_END
};

cyaml_schema_field_t hw_settings_fields[] =
{
	CYAML_FIELD_FLOAT("timezone", CYAML_FLAG_DEFAULT, hw_settings_t, timezone),
	CYAML_FIELD_MAPPING("keyboard", CYAML_FLAG_DEFAULT, hw_settings_t, keyboard, ui_fields),
	CYAML_FIELD_MAPPING("display", CYAML_FLAG_DEFAULT, hw_settings_t, display, ui_fields),
	CYAML_FIELD_MAPPING("rf", CYAML_FLAG_DEFAULT, hw_settings_t, rf, rf_fields),
	CYAML_FIELD_END
};

cyaml_schema_field_t frontend_fields[] =
{
	CYAML_FIELD_FLOAT("rf_power_out_sp", CYAML_FLAG_DEFAULT, frontend_t, rf_power_out_sp),
	CYAML_FIELD_UINT("rf_switch", CYAML_FLAG_DEFAULT, frontend_t, rf_switch),
	CYAML_FIELD_FLOAT("atten_0", CYAML_FLAG_DEFAULT, frontend_t, atten_0),
	CYAML_FIELD_FLOAT("atten_1", CYAML_FLAG_DEFAULT, frontend_t, atten_1),
	CYAML_FIELD_FLOAT("lna_gain", CYAML_FLAG_DEFAULT, frontend_t, lna_gain),
	CYAML_FIELD_FLOAT("pga_gain", CYAML_FLAG_DEFAULT, frontend_t, pga_gain),
	CYAML_FIELD_FLOAT("dac_gain", CYAML_FLAG_DEFAULT, frontend_t, dac_gain),
	CYAML_FIELD_FLOAT("mix_gain", CYAML_FLAG_DEFAULT, frontend_t, mix_gain),
	CYAML_FIELD_BOOL("tx_enabled", CYAML_FLAG_DEFAULT, frontend_t, tx_enabled),
	CYAML_FIELD_BOOL("rx_enabled", CYAML_FLAG_DEFAULT, frontend_t, rx_enabled),
	CYAML_FIELD_END
};

cyaml_schema_field_t channel_extra_fields[] =
{
	CYAML_FIELD_STRING_PTR("mode", CYAML_FLAG_POINTER, channel_extra_t, mode, 0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("submode", CYAML_FLAG_POINTER, channel_extra_t, submode, 0, CYAML_UNLIMITED),
	CYAML_FIELD_FLOAT("squelch_level", CYAML_FLAG_DEFAULT, channel_extra_t, squelch_level),
	CYAML_FIELD_FLOAT("ctcss_tone", CYAML_FLAG_DEFAULT, channel_extra_t, ctcss_tone),
	CYAML_FIELD_BOOL("ctcss_tx", CYAML_FLAG_DEFAULT, channel_extra_t, ctcss_tx),
	CYAML_FIELD_BOOL("ctcss_rx", CYAML_FLAG_DEFAULT, channel_extra_t, ctcss_rx),
	CYAML_FIELD_STRING_PTR("src", CYAML_FLAG_POINTER, channel_extra_t, src, 0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("dst", CYAML_FLAG_POINTER, channel_extra_t, dst, 0, CYAML_UNLIMITED),
	CYAML_FIELD_UINT("can", CYAML_FLAG_DEFAULT, channel_extra_t, can),
	CYAML_FIELD_BOOL("encrypted", CYAML_FLAG_DEFAULT, channel_extra_t, encrypted),
	CYAML_FIELD_STRING_PTR("type", CYAML_FLAG_POINTER, channel_extra_t, type, 0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("encr_key", CYAML_FLAG_POINTER, channel_extra_t, encr_key, 0, CYAML_UNLIMITED),
	CYAML_FIELD_BOOL("signed", CYAML_FLAG_DEFAULT, channel_extra_t, signed_flag),
	CYAML_FIELD_STRING_PTR("sign_key", CYAML_FLAG_POINTER, channel_extra_t, sign_key, 0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("meta", CYAML_FLAG_POINTER, channel_extra_t, meta, 0, CYAML_UNLIMITED),
	CYAML_FIELD_END
};

cyaml_schema_field_t channel_fields[] =
{
	CYAML_FIELD_BOOL("active", CYAML_FLAG_DEFAULT, channel_t, active),
	CYAML_FIELD_STRING_PTR("fg", CYAML_FLAG_POINTER, channel_t, fg, 0, CYAML_UNLIMITED),
	CYAML_FIELD_UINT("tx_freq", CYAML_FLAG_DEFAULT, channel_t, tx_freq),
	CYAML_FIELD_UINT("rx_freq", CYAML_FLAG_DEFAULT, channel_t, rx_freq),
	CYAML_FIELD_FLOAT("bw", CYAML_FLAG_DEFAULT, channel_t, bw),
	CYAML_FIELD_MAPPING("extra", CYAML_FLAG_DEFAULT, channel_t, extra, channel_extra_fields),
	CYAML_FIELD_END
};

cyaml_schema_field_t channels_fields[] =
{
	CYAML_FIELD_MAPPING("vfo_0", CYAML_FLAG_DEFAULT, typeof(((config_t *)0)->channels), vfo_0, channel_fields),
	CYAML_FIELD_MAPPING("vfo_1", CYAML_FLAG_DEFAULT, typeof(((config_t *)0)->channels), vfo_1, channel_fields),
	CYAML_FIELD_END
};

cyaml_schema_field_t config_fields[] =
{
	CYAML_FIELD_MAPPING("frontend", CYAML_FLAG_DEFAULT, config_t, frontend, frontend_fields),
	CYAML_FIELD_MAPPING("settings", CYAML_FLAG_DEFAULT, config_t, settings, hw_settings_fields),
	CYAML_FIELD_MAPPING("channels", CYAML_FLAG_DEFAULT, config_t, channels, channels_fields),
	CYAML_FIELD_END
};

static const cyaml_schema_value_t config_schema =
{
	CYAML_VALUE_MAPPING(CYAML_FLAG_POINTER, config_t, config_fields),
};

// framebuffer init
int fb_init(uint32_t **buffer, size_t *ssize, int *fhandle)
{
	*fhandle = open("/dev/fb0", O_RDWR);
	if (*fhandle < 0)
	{
		perror("open");
		return 1;
	}

	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;

	if (ioctl(*fhandle, FBIOGET_FSCREENINFO, &finfo) < 0)
	{
		perror("finfo");
		return 1;
	}

	if (ioctl(*fhandle, FBIOGET_VSCREENINFO, &vinfo) < 0)
	{
		perror("vinfo");
		return 1;
	}

	*ssize = finfo.line_length * vinfo.yres;
	*buffer = (uint32_t *)mmap(0, *ssize, PROT_READ | PROT_WRITE, MAP_SHARED, *fhandle, 0);
	if (*buffer == MAP_FAILED)
	{
		perror("mmap");
		return 1;
	}

	return 0;
}

void fb_cleanup(uint32_t *buffer, size_t ssize, int fhandle)
{
	munmap(buffer, ssize);
	close(fhandle);
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
	pmt[0] = 2;									 // pmt type - zmq message
	*((uint16_t *)&pmt[1]) = htons(strlen(msg)); // length
	strcpy((char *)&pmt[3], msg);

	return 3 + strlen(msg);
}

int main(void)
{
	// settings
	uint32_t freq_a = 433475000, freq_b = 439212500;

	cyaml_config_t cfg =
	{
        .log_level = CYAML_LOG_DEBUG,
        .mem_fn = cyaml_mem,
    };

    config_t *conf = NULL;
    cyaml_err_t err = cyaml_load_file("/usr/share/linht/settings.yaml", &cfg, &config_schema, (cyaml_data_t **)&conf, NULL);
    if (err != CYAML_OK)
	{
        fprintf(stderr, "Failed to load: %s\n", cyaml_strerror(err));
        return -1;
    }

	// printout
	if(1)
	{
		fprintf(stderr, "Loaded settings:\n");
		fprintf(stderr, "VCO A RX: %d Hz\n", conf->channels.vfo_0.rx_freq);
		fprintf(stderr, "VCO A TX: %d Hz\n", conf->channels.vfo_0.tx_freq);
		fprintf(stderr, "VCO B RX: %d Hz\n", conf->channels.vfo_1.rx_freq);
		fprintf(stderr, "VCO B TX: %d Hz\n", conf->channels.vfo_1.tx_freq);
		fprintf(stderr, "-------------------------\n\n");
	}

	// LEDs
	linht_ctrl_green_led_set(false);
	linht_ctrl_red_led_set(false);

	// SX1255
	if (sx1255_init(spi_device, gpio_chip_path, rst_pin_offset) != 0)
	{
		fprintf(stderr, "Can not initialize device\nExiting\n");
		return -1;
	}

	sx1255_reset();
	sx1255_set_rate(SX1255_RATE_500K);
	sx1255_set_rx_freq(freq_a);
	sx1255_set_tx_freq(freq_a);
	sx1255_set_lna_gain(20);
	sx1255_set_pga_gain(24);
	sx1255_set_dac_gain(0);
	sx1255_set_mixer_gain(-13.5);
	sx1255_enable_rx(true);
	sx1255_enable_tx(true);
	system("tx_rx 0");

	// keyboard
	if ((rval = kbd_init(&kbd, kbd_path)) != 0)
	{
		return rval;
	}

	// ZeroMQ and PMT
	void *zmq_ctx = zmq_ctx_new();
	void *zmq_pub = zmq_socket(zmq_ctx, ZMQ_PUB);

	if (zmq_connect(zmq_pub, zmq_ipc) != 0)
	{
		fprintf(stderr, "ZeroMQ: Error connecting to Unix socket %s.\nExiting.\n", zmq_ipc);
		return -1;
	}

	pmt_len = string_to_pmt(sot_pmt, "SOT");
	string_to_pmt(eot_pmt, "EOT");

	Image img;
	Texture2D texture[6] = {0};

	// Initialize Raylib window with DRM backend (no X11)
	// When compiled with PLATFORM_DRM, this will automatically use the framebuffer
	InitWindow(RES_X, RES_Y, "Mockup");

	Font customFont = LoadFontEx("/usr/share/linht/fonts/Ubuntu-Regular.ttf", 28, 0, 250);
	Font customFont10 = LoadFontEx("/usr/share/linht/fonts/UbuntuCondensed-Regular.ttf", 10, 0, 250);
	Font customFont12 = LoadFontEx("/usr/share/linht/fonts/UbuntuCondensed-Regular.ttf", 12, 0, 250);

	// load the wallpaper
	img = LoadImage("/usr/share/linht/icons/wallpaper.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[0] = LoadTextureFromImage(img);

	img = LoadImage("/usr/share/linht/icons/mute.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[1] = LoadTextureFromImage(img);

	img = LoadImage("/usr/share/linht/icons/gnss.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[2] = LoadTextureFromImage(img);

	img = LoadImage("/usr/share/linht/icons/batt_100.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[3] = LoadTextureFromImage(img);

	img = LoadImage("/usr/share/linht/icons/vfo_act.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[4] = LoadTextureFromImage(img);

	img = LoadImage("/usr/share/linht/icons/vfo_inact.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[5] = LoadTextureFromImage(img);

	UnloadImage(img);

	// Set low FPS for embedded display
	SetTargetFPS(5);

	// Main loop
	while (!WindowShouldClose())
	{
		struct input_event ev;
		fcntl(kbd, F_SETFL, fcntl(kbd, F_GETFL, 0) | O_NONBLOCK); // non-blocking access
		ssize_t n = read(kbd, &ev, sizeof(ev));
		if (n == (ssize_t)sizeof(ev))
		{
			if (ev.value == KEY_PRESS)
			{
				if (ev.code == KEY_P)
				{
					// TODO: add a proper TX/RX RF switch control
					system("tx_rx 1");
					linht_ctrl_red_led_set(true);
					zmq_send(zmq_pub, sot_pmt, pmt_len, 0);
					fprintf(stderr, "PTT pressed\n");
				}
				else if (ev.code == KEY_UP)
				{
					freq_a += 12500;
					sx1255_set_rx_freq(freq_a);
					sx1255_set_tx_freq(freq_a);
				}
				else if (ev.code == KEY_DOWN)
				{
					freq_a -= 12500;
					sx1255_set_rx_freq(freq_a);
					sx1255_set_tx_freq(freq_a);
				}
				else if (ev.code == KEY_ESC)
				{
					break;
				}
				else
				{
					;
				}
			}
			else // KEY_RELEASE
			{
				if (ev.code == KEY_P)
				{
					// TODO: add a proper TX/RX RF switch control
					system("tx_rx 0");
					linht_ctrl_red_led_set(false);
					zmq_send(zmq_pub, eot_pmt, pmt_len, 0);
					fprintf(stderr, "PTT released\n");
				}
				else
				{
					;
				}
			}
		}

		BeginDrawing();
		// Clear screen
		ClearBackground(BLACK);

		// draw the wallpaper
		DrawTexture(texture[0], 0, 0, WHITE);

		// draw header
		DrawRectangle(0, 0, RES_X, 17, (Color){0x20, 0x20, 0x20, 0xFF});
		DrawLine(0, 17, RES_X - 1, 17, (Color){0x60, 0x60, 0x60, 0xFF});

		// time
		time_t t = time(NULL);
		struct tm *time_info = localtime(&t);
		char time_s[10];
		sprintf(time_s, "%02d:%02d", time_info->tm_hour, time_info->tm_min);
		DrawTextEx(customFont, time_s, (Vector2){2.0f, 2.0f}, 14.0f, 0, WHITE);

		//'gnss' icon
		DrawTexture(texture[2], RES_X - 40, 1, WHITE);

		// battery
		DrawTexture(texture[3], RES_X - 24, 2, WHITE);

		// VFO A
		DrawTexture(texture[4], 2, 23, WHITE);
		DrawTextEx(customFont, "A", (Vector2){4.5f, 22.0f}, 16.0f, 0, WHITE);
		DrawTextEx(customFont10, "VFO", (Vector2){3.0f, 38.0f}, 10.0f, 1, WHITE);
		char freq_a_str[10];
		sprintf(freq_a_str, "%d.%03d", freq_a / 1000000, (freq_a - (freq_a / 1000000) * 1000000) / 1000);
		DrawTextEx(customFont, freq_a_str, (Vector2){21.0f, 18.0f}, (float)customFont.baseSize, 0, WHITE);
		sprintf(freq_a_str, "%02d", (freq_a % 1000) / 10);
		DrawTextEx(customFont, freq_a_str, (Vector2){113.0f, 28.0f}, 16.0f, 0, WHITE);
		DrawTextEx(customFont12, "12.5k", (Vector2){21.0f, 44.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, "", (Vector2){50.0f, 44.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, "", (Vector2){79.0f, 44.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, "M17", (Vector2){21.0f, 56.0f}, 12.0f, 1, GREEN);
		DrawTextEx(customFont12, "@ALL", (Vector2){50.0f, 56.0f}, 12.0f, 1, GREEN);
		// DrawTexture(texture[1], 140, 28, WHITE); //'vfo a mute' icon

		// horizontal separating bar
		DrawLine(0, 74, RES_X - 1, 74, (Color){0x60, 0x60, 0x60, 0xFF});

		// VFO B
		DrawTexture(texture[4], 2, 79, WHITE);
		DrawTextEx(customFont, "B", (Vector2){4.5f, 78.0f}, 16.0f, 0, WHITE);
		DrawTextEx(customFont10, "VFO", (Vector2){3.0f, 94.0f}, 10.0f, 1, WHITE);
		char freq_b_str[10];
		sprintf(freq_b_str, "%d.%03d", freq_b / 1000000, (freq_b - (freq_b / 1000000) * 1000000) / 1000);
		DrawTextEx(customFont, freq_b_str, (Vector2){21.0f, 74.0f}, (float)customFont.baseSize, 0, WHITE);
		sprintf(freq_b_str, "%02d", (freq_b % 1000) / 10);
		DrawTextEx(customFont, freq_b_str, (Vector2){113.0f, 84.0f}, 16.0f, 0, WHITE);
		DrawTextEx(customFont12, "12.5k", (Vector2){21.0f, 100.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, "127.3", (Vector2){50.0f, 100.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, "-7.6M", (Vector2){79.0f, 100.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, "FM", (Vector2){21.0f, 112.0f}, 12.0f, 1, GREEN);
		DrawTexture(texture[1], 140, 84, WHITE); //'vfo b mute' icon

		// Or if you want more control over positioning:
		// Rectangle src = {0, 0, (float)texture.width, (float)texture.height};
		// Rectangle dst = {0, 0, RES_X, RES_Y};
		// DrawTexturePro(texture, src, dst, (Vector2){0, 0}, 0.0f, WHITE);

		EndDrawing();
	}

	// Cleanup
	UnloadTexture(texture[0]);
	UnloadTexture(texture[1]);
	UnloadTexture(texture[2]);
	UnloadTexture(texture[3]);
	UnloadTexture(texture[4]);
	UnloadTexture(texture[5]);
	UnloadFont(customFont);
	UnloadFont(customFont10);
	UnloadFont(customFont12);
	kbd_cleanup(kbd);
	sx1255_cleanup();
	zmq_disconnect(zmq_pub, zmq_ipc);
	zmq_ctx_destroy(&zmq_ctx);
	cyaml_free(&cfg, &config_schema, conf, 0);
	CloseWindow();

	return 0;
}
