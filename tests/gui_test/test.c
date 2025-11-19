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

// keymap states
#define KEY_PRESS 0
#define KEY_RELEASE 1

// GFX
#define RES_X 160
#define RES_Y 128
#define IMG_PATH "/usr/share/linht/icons"

bool raylib_debug = false;

enum
{
	IMG_WALLPAPER,
	IMG_MUTE,
	IMG_GNSS,
	IMG_BATT_100,
	IMG_VFO_ACT,
	IMG_VFO_INACT,
	IMG_COUNT = IMG_VFO_INACT + 1
};

Texture2D texture[IMG_COUNT];

// settings
const char *settings_file = "/usr/share/linht/settings.yaml";

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
uint16_t cnt;

// keyboard
const char *kbd_path = "/dev/input/event0";
int kbd; // keyboard file handle

// ZeroMQ and PMT
const char *zmq_ipc = "ipc:///tmp/ptt_msg";

uint8_t sot_pmt[10];
uint8_t eot_pmt[10];
uint8_t pmt_len; // "SOT" and "EOT" PMTs are the same length - single variable is fine

// states
bool vfo_a_tx = false;
bool vfo_b_tx = false;
time_t esc_start;
bool esc_pressed = false;

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

void load_gfx(void)
{
	Image img;

	img = LoadImage(IMG_PATH "/wallpaper.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_WALLPAPER] = LoadTextureFromImage(img);

	img = LoadImage(IMG_PATH "/mute.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_MUTE] = LoadTextureFromImage(img);

	img = LoadImage(IMG_PATH "/gnss.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_GNSS] = LoadTextureFromImage(img);

	img = LoadImage(IMG_PATH "/batt_100.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_BATT_100] = LoadTextureFromImage(img);

	img = LoadImage(IMG_PATH "/vfo_act.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_VFO_ACT] = LoadTextureFromImage(img);

	img = LoadImage(IMG_PATH "/vfo_inact.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_VFO_INACT] = LoadTextureFromImage(img);

	UnloadImage(img);
}

int kbd_init(int *fhandle, const char *path)
{
	*fhandle = open(path, O_RDONLY);
	if (*fhandle < 0)
	{
		perror("open keyboard dev");
		return 1;
	}

	fcntl(*fhandle, F_SETFL, fcntl(*fhandle, F_GETFL, 0) | O_NONBLOCK); // non-blocking access

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

void sx1255_pa_enable(bool ena)
{
	if (ena)
	{
		uint8_t tmp = sx1255_read_reg(0x00);
		tmp |= (1 << 3);
		sx1255_write_reg(0x00, tmp);
	}
	else
	{
		uint8_t tmp = sx1255_read_reg(0x00);
		tmp &= (uint8_t)~(1 << 3);
		sx1255_write_reg(0x00, tmp);
	}
}

int main(void)
{
	// load settings
	cyaml_config_t cfg =
		{
			.log_level = CYAML_LOG_DEBUG,
			.mem_fn = cyaml_mem,
		};

	config_t *conf = NULL;
	cyaml_err_t err = cyaml_load_file(settings_file, &cfg, &config_schema, (cyaml_data_t **)&conf, NULL);
	if (err != CYAML_OK)
	{
		fprintf(stderr, "Failed to load: %s\n", cyaml_strerror(err));
		return -1;
	}
	
	// settings (clean this up)
	uint32_t vfo_a_rx_f = conf->channels.vfo_0.rx_freq;
	uint32_t vfo_a_tx_f = conf->channels.vfo_0.tx_freq;
	uint32_t vfo_b_rx_f = conf->channels.vfo_1.rx_freq;
	uint32_t vfo_b_tx_f = conf->channels.vfo_1.tx_freq;
	uint16_t rf_rate = conf->frontend.rf_sample_rate;
	float freq_corr = conf->settings.rf.freq_corr;

	// settings printout
	if (1)
	{
		fprintf(stderr, "Loaded settings:\n");
		fprintf(stderr, "      VCO A RX  %d Hz\n", vfo_a_rx_f);
		fprintf(stderr, "            TX  %d Hz\n", vfo_a_tx_f);
		fprintf(stderr, "      VCO B RX  %d Hz\n", vfo_b_rx_f);
		fprintf(stderr, "            TX  %d Hz\n", vfo_b_tx_f);
		fprintf(stderr, "   Freq. corr.  %+.3f ppm\n", freq_corr);
		fprintf(stderr, "   I DC offset  %+.3f\n", conf->settings.rf.i_dc);
		fprintf(stderr, "   Q DC offset  %+.3f\n", conf->settings.rf.q_dc);
		fprintf(stderr, "    IQ balance  %+.4f\n", conf->settings.rf.iq_bal);
		fprintf(stderr, "      IQ angle  %+.1f deg.\n", conf->settings.rf.iq_theta);
		fprintf(stderr, "RF sample rate  %d kHz\n", rf_rate);
		fprintf(stderr, "----------------------------\n");
	}

	// LEDs
	linht_ctrl_green_led_set(false);
	linht_ctrl_red_led_set(false);

	// SX1255 init and config
	if (sx1255_init(spi_device, gpio_chip_path, rst_pin_offset) != 0)
	{
		fprintf(stderr, "Can not initialize SX1255 device.\nExiting.\n");
		return -1;
	}

	sx1255_reset();
	if(rf_rate == 500)
		sx1255_set_rate(SX1255_RATE_500K);
	else if(rf_rate == 250)
		sx1255_set_rate(SX1255_RATE_250K);
	else if(rf_rate == 125)
		sx1255_set_rate(SX1255_RATE_125K);
	sx1255_set_rx_freq(vfo_a_rx_f * (1.0 + freq_corr * 1e-6));
	sx1255_set_tx_freq(vfo_a_tx_f * (1.0 + freq_corr * 1e-6));
	sx1255_set_lna_gain(20);
	sx1255_set_pga_gain(24);
	sx1255_set_dac_gain(0);
	sx1255_set_mixer_gain(-13.5);
	sx1255_enable_rx(true);
	sx1255_enable_tx(true);
	sx1255_pa_enable(false);
	linht_ctrl_tx_rx_switch_set(true);
	fprintf(stderr, "SX1255 setup finished\n");

	// keyboard
	if ((rval = kbd_init(&kbd, kbd_path)) != 0)
	{
		return rval;
	}

	// ZeroMQ and PMT
	void *zmq_ctx = zmq_ctx_new();
	void *zmq_pub = zmq_socket(zmq_ctx, ZMQ_PUB);

	if (zmq_bind(zmq_pub, zmq_ipc) != 0)
	{
		fprintf(stderr, "ZeroMQ: Error binding to Unix socket %s.\nExiting.\n", zmq_ipc);
		return -1;
	}

	pmt_len = string_to_pmt(sot_pmt, "SOT");
	string_to_pmt(eot_pmt, "EOT");

	// init Raylib
	fprintf(stderr, "Initializing Raylib...\n");
	if (!raylib_debug)
		SetTraceLogLevel(LOG_NONE);
	InitWindow(RES_X, RES_Y, "GUI test");

	Font customFont = LoadFontEx("/usr/share/linht/fonts/Ubuntu-Regular.ttf", 28, 0, 250);
	Font customFont10 = LoadFontEx("/usr/share/linht/fonts/UbuntuCondensed-Regular.ttf", 10, 0, 250);
	Font customFont12 = LoadFontEx("/usr/share/linht/fonts/UbuntuCondensed-Regular.ttf", 12, 0, 250);

	// load images
	load_gfx();

	// set low FPS for embedded display
	SetTargetFPS(5);

	//execute FG, TODO: the parameters are only OK for M17 FG
	char *fg_path = conf->channels.vfo_0.fg;
	fprintf(stderr, "Executing GNU Radio flowgraph (%s)\n", fg_path);
	char fg_str[256];
	sprintf(fg_str, "python %s -o \"%.4f+%.4fj\" -S \"%s\" -D \"%s\" -C %d &> /dev/null &",
		fg_path,
		conf->settings.rf.i_dc,
		conf->settings.rf.q_dc,
		conf->channels.vfo_0.extra.src,
		conf->channels.vfo_0.extra.dst,
		conf->channels.vfo_0.extra.can );
	system(fg_str);
	
	// get time
	esc_start = time(NULL);
	
	// ready!
	fprintf(stderr, "Ready! Awaiting commands...\n");

	// main loop
	while (!WindowShouldClose())
	{
		struct input_event ev;

		ssize_t n = read(kbd, &ev, sizeof(ev)); // non-blocking

		if (n == (ssize_t)sizeof(ev))
		{
			if (ev.value == KEY_PRESS)
			{
				if (ev.code == KEY_P)
				{
					//TODO: bug - this is PTT release event
					sx1255_enable_rx(true);
					sx1255_pa_enable(false);
					linht_ctrl_tx_rx_switch_set(true);
					linht_ctrl_red_led_set(false);
					zmq_send(zmq_pub, eot_pmt, pmt_len, 0);
					vfo_a_tx = false;
					fprintf(stderr, "PTT released\n");
				}
				else if (ev.code == KEY_UP)
				{
					vfo_a_rx_f += 12500; vfo_a_tx_f += 12500; 
					sx1255_set_rx_freq(vfo_a_rx_f * (1.0 + freq_corr * 1e-6));
					sx1255_set_tx_freq(vfo_a_tx_f * (1.0 + freq_corr * 1e-6));
				}
				else if (ev.code == KEY_DOWN)
				{
					vfo_a_rx_f -= 12500; vfo_a_tx_f -= 12500;
					sx1255_set_rx_freq(vfo_a_rx_f * (1.0 + freq_corr * 1e-6));
					sx1255_set_tx_freq(vfo_a_tx_f * (1.0 + freq_corr * 1e-6));
				}
				else if (ev.code == KEY_LEFT)
				{
					;
				}
				else if (ev.code == KEY_RIGHT)
				{
					;
				}
				else if (ev.code == KEY_ESC)
				{
					esc_start = time(NULL);
					esc_pressed = true;
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
					//TODO: bug - this is PTT press event
					sx1255_enable_rx(false);
					sx1255_pa_enable(true);
					linht_ctrl_tx_rx_switch_set(false);
					linht_ctrl_red_led_set(true);
					zmq_send(zmq_pub, sot_pmt, pmt_len, 0);
					vfo_a_tx = true;
					fprintf(stderr, "PTT pressed\n");
				}
				else if (ev.code == KEY_ESC)
				{
					esc_pressed = false;
				}
				else
				{
					;
				}
			}
		}
		
		// check if ESC button has been pressed for at least 5 seconds
		if (time(NULL) - esc_start >= 5 && esc_pressed)
			break;

		BeginDrawing();

		// clear screen
		ClearBackground(BLACK);

		// draw the wallpaper
		DrawTexture(texture[IMG_WALLPAPER], 0, 0, WHITE);

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
		DrawTexture(texture[IMG_GNSS], RES_X - 40, 1, WHITE);

		// battery voltage display
		static char bv[8] = {0};
		static Color bv_col = WHITE;
		if (cnt % 5 == 0)
		{
			FILE *fp = fopen("/sys/bus/iio/devices/iio:device0/in_voltage1_raw", "r");

			if (!fp)
			{
				fprintf(stderr, "Failed to open IIO sysfs entry\n");
				return -1;
			}

			int value;
			if (fscanf(fp, "%d", &value) != 1)
			{
				fprintf(stderr, "Failed to read battery voltage\n");
				fclose(fp);
				return -1;
			}

			fclose(fp);

			uint16_t batt_mv = (uint16_t)(value / 4096.0 * 1.8 * (39.0 + 10.0) / 10.0 * 1000.0);
			sprintf(bv, "%d.%d", batt_mv / 1000, (batt_mv - (batt_mv / 1000) * 1000) / 100);

			if (batt_mv >= 7400)
				bv_col = WHITE;
			else if (batt_mv >= 7000)
				bv_col = ORANGE;
			else
				bv_col = RED;
		}

		// battery voltage - icon or text
		// DrawTexture(texture[IMG_BATT_100], RES_X - 24, 2, WHITE);
		DrawTextEx(customFont, bv, (Vector2){RES_X - 20.0f, 2.0f}, 14.0f, 0, bv_col);

		// VFO A
		DrawTexture(texture[IMG_VFO_ACT], 2, 23, WHITE);
		DrawTextEx(customFont, "A", (Vector2){4.5f, 22.0f}, 16.0f, 0, WHITE);
		DrawTextEx(customFont10, "VFO", (Vector2){3.0f, 38.0f}, 10.0f, 1, WHITE);
		char freq_a_str[10];
		uint32_t freq_a = vfo_a_tx ? vfo_a_tx_f : vfo_a_rx_f;
		sprintf(freq_a_str, "%d.%03d", freq_a / 1000000, (freq_a - (freq_a / 1000000) * 1000000) / 1000);
		DrawTextEx(customFont, freq_a_str, (Vector2){21.0f, 18.0f}, (float)customFont.baseSize, 0, vfo_a_tx ? RED : WHITE);
		sprintf(freq_a_str, "%02d", (freq_a % 1000) / 10);
		DrawTextEx(customFont, freq_a_str, (Vector2){113.0f, 28.0f}, 16.0f, 0, vfo_a_tx ? RED : WHITE);
		DrawTextEx(customFont12, "12.5k", (Vector2){21.0f, 44.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, "", (Vector2){50.0f, 44.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, "", (Vector2){79.0f, 44.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, conf->channels.vfo_0.extra.mode, (Vector2){21.0f, 56.0f}, 12.0f, 1, GREEN);
		DrawTextEx(customFont12, conf->channels.vfo_0.extra.dst, (Vector2){50.0f, 56.0f}, 12.0f, 1, GREEN);
		DrawTextEx(customFont12, "CAN 0", (Vector2){108.0f, 56.0f}, 12.0f, 1, GREEN);
		// DrawTexture(texture[IMG_MUTE], 140, 28, WHITE); //'vfo a mute' icon

		// horizontal separating bar
		DrawLine(0, 74, RES_X - 1, 74, (Color){0x60, 0x60, 0x60, 0xFF});

		// VFO B
		DrawTexture(texture[IMG_VFO_ACT], 2, 79, WHITE);
		DrawTextEx(customFont, "B", (Vector2){4.5f, 78.0f}, 16.0f, 0, WHITE);
		DrawTextEx(customFont10, "VFO", (Vector2){3.0f, 94.0f}, 10.0f, 1, WHITE);
		char freq_b_str[10];
		uint32_t freq_b = vfo_b_tx ? vfo_b_tx_f : vfo_b_rx_f;
		sprintf(freq_b_str, "%d.%03d", freq_b / 1000000, (freq_b - (freq_b / 1000000) * 1000000) / 1000);
		DrawTextEx(customFont, freq_b_str, (Vector2){21.0f, 74.0f}, (float)customFont.baseSize, 0, WHITE);
		sprintf(freq_b_str, "%02d", (freq_b % 1000) / 10);
		DrawTextEx(customFont, freq_b_str, (Vector2){113.0f, 84.0f}, 16.0f, 0, WHITE);
		DrawTextEx(customFont12, "12.5k", (Vector2){21.0f, 100.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, "127.3", (Vector2){50.0f, 100.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, "-7.6M", (Vector2){79.0f, 100.0f}, 12.0f, 1, BLUE);
		DrawTextEx(customFont12, "FM", (Vector2){21.0f, 112.0f}, 12.0f, 1, GREEN);
		DrawTexture(texture[IMG_MUTE], 140, 84, WHITE); //'vfo b mute' icon

		// test
		// Rectangle src = {0, 0, (float)texture.width, (float)texture.height};
		// Rectangle dst = {0, 0, RES_X, RES_Y};
		// DrawTexturePro(texture, src, dst, (Vector2){0, 0}, 0.0f, WHITE);

		EndDrawing();

		// frame counter
		cnt++;
		cnt %= 60 * 5;
	}

	// Cleanup
	fprintf(stderr, "Exit code caught. Cleaning up...\n");
	for (uint8_t i=0; i<IMG_COUNT; i++)
		UnloadTexture(texture[i]);
	UnloadFont(customFont);
	UnloadFont(customFont10);
	UnloadFont(customFont12);
	kbd_cleanup(kbd);
	sx1255_cleanup();
	zmq_unbind(zmq_pub, zmq_ipc);
	zmq_ctx_destroy(&zmq_ctx);
	cyaml_free(&cfg, &config_schema, conf, 0);
	system("kill -TERM `pidof python`"); // kill FG
	CloseWindow();
	
	fprintf(stderr, "Cleanup done. Exiting.\n");
	
	// device shutdown
	// TODO: fix it
	// system("shutdown now");

	return 0;
}
