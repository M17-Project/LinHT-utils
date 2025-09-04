/*
 * Simple key scan app with framebuffer access.
 * Includes sample ISS pass skyplot.
 *
 * Compile with `make`
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

#include <predict/predict.h>

#define DEG2RAD(x) ((x) * M_PI / 180.0f)
#define RAD2DEG(x) ((x) / M_PI * 180.0f)

#define RES_X 160
#define RES_Y 128

// return value
int rval;

// screen
int fb;				   // framebuffer file handle
size_t ssize;		   // screen size
uint32_t *framebuffer; // framebuffer

// keyboard
const char *kbd_path = "/dev/input/event0";
int kbd; // keyboard file handle

// keymap and states
#define KEY_ESC 1
#define KEY_1 2
#define KEY_2 3
#define KEY_3 4
#define KEY_4 5
#define KEY_5 6
#define KEY_6 7
#define KEY_7 8
#define KEY_8 9
#define KEY_9 10
#define KEY_0 11
#define KEY_ENTER 28
#define KEY_HASH 43
#define KEY_KPASTERISK 55
#define KEY_F1 59
#define KEY_F2 60
#define KEY_UP 103
#define KEY_DOWN 108

#define KEY_PRESS 0
#define KEY_RELEASE 1

char tle[2][128] =
	{
		"1 25544U 98067A   25246.19936559  .00014267  00000-0  25396-3 0  9994",
		"2 25544  51.6337 280.4082 0003342 309.8573  50.2122 15.50393129527260"};

const float lat = 52.43f;
const float lon = 20.71f;

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

static inline void put_pixel(uint32_t *fb, int x, int y, uint32_t color)
{
	if (x >= 0 && x < RES_X && y >= 0 && y < RES_Y)
		fb[y * RES_X + x] = color;
}

void draw_line(uint32_t *fb, int x0, int y0, int x1, int y1, uint32_t color)
{
	int dx = abs(x1 - x0);
	int dy = -abs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy; // error term

	while (1)
	{
		put_pixel(fb, x0, y0, color);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * err;
		if (e2 >= dy)
		{
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx)
		{
			err += dx;
			y0 += sy;
		}
	}
}

void draw_circle(uint32_t *fb, uint16_t cx, uint16_t cy, uint8_t r, uint32_t color, bool filled)
{
	float t1 = (float)r / 16;
	uint16_t x = r;
	uint16_t y = 0;

	while (x >= y)
	{
		if (filled)
		{
			for (int i = cx - x; i <= cx + x; i++)
			{
				put_pixel(fb, i, cy + y, color);
				put_pixel(fb, i, cy - y, color);
			}
			for (int i = cx - y; i <= cx + y; i++)
			{
				put_pixel(fb, i, cy + x, color);
				put_pixel(fb, i, cy - x, color);
			}
		}
		else
		{
			put_pixel(fb, cx + x, cy + y, color);
			put_pixel(fb, cx + y, cy + x, color);
			put_pixel(fb, cx - y, cy + x, color);
			put_pixel(fb, cx - x, cy + y, color);
			put_pixel(fb, cx - x, cy - y, color);
			put_pixel(fb, cx - y, cy - x, color);
			put_pixel(fb, cx + y, cy - x, color);
			put_pixel(fb, cx + x, cy - y, color);
		}

		y++;
		t1 = t1 + y;
		float t2 = t1 - x;
		if (t2 >= 0)
		{
			t1 = t2;
			x--;
		}
	}
}

int main(void)
{
	// screen init
	if ((rval = fb_init(&framebuffer, &ssize, &fb)) != 0)
	{
		return rval;
	}

	// keyboard
	if ((rval = kbd_init(&kbd, kbd_path)) != 0)
	{
		return rval;
	}

	// sample ISS pass
	struct predict_position orbit;
	struct predict_observation observation;

	predict_orbital_elements_t *orbital_elements = predict_parse_tle(tle[0], tle[1]);
	predict_observer_t *observer = predict_create_observer("OpenRTX", DEG2RAD(lat), DEG2RAD(lon), 75);

	float az_el[2][601] = {{0, 0}};
	for (int t = -5 * 60; t < 5 * 60; t++)
	{
		predict_julian_date_t curr_time = predict_to_julian(1756948800 + t);

		predict_orbit(orbital_elements, &orbit, curr_time);
		predict_observe_orbit(observer, &orbit, &observation);

		az_el[0][t + 300] = RAD2DEG(observation.azimuth);
		az_el[1][t + 300] = RAD2DEG(observation.elevation);
	}

	struct input_event ev;
	while (1)
	{
		ssize_t n = read(kbd, &ev, sizeof(ev));
		if (n == (ssize_t)sizeof(ev))
		{
			/*printf("time %ld.%06ld\ttype %u\tcode %u\tvalue %d\n",
				   ev.time.tv_sec, ev.time.tv_usec,
				   ev.type, ev.code, ev.value);*/
			if (ev.value == KEY_PRESS)
			{
				if (ev.code == KEY_UP)
				{
					// printf("Up!\n");
					// flip all pixels
					for (size_t i = 0; i < ssize / 4; i++)
					{
						framebuffer[i] ^= 0x00FFFFFF; // 0x00FF0000; //AARRGGBB
					}
				}

				else if (ev.code == KEY_DOWN)
				{
					draw_circle(framebuffer, RES_X / 2, RES_Y / 2, RES_Y / 2 - 1, 0x00FFFFFF, false);
					draw_circle(framebuffer, RES_X / 2, RES_Y / 2, RES_Y / 4 - 1, 0x00FFFFFF, false);
					draw_line(framebuffer, RES_X / 2, 1, RES_X / 2, RES_Y - 1, 0x00CCCCCC);
					draw_line(framebuffer, RES_X / 2 - RES_Y / 2 + 1, RES_Y / 2, RES_X / 2 + RES_Y / 2 - 1, RES_Y / 2, 0x00CCCCCC);
				}

				else if (ev.code == KEY_ESC)
				{
					// black
					for (size_t i = 0; i < ssize / 4; i++)
					{
						framebuffer[i] = 0;
					}
				}

				else if (ev.code == KEY_ENTER)
				{
					uint8_t x = RES_X / 2;
					uint8_t y = RES_Y / 2;
					uint8_t r = RES_Y / 2;

					for (uint16_t i = 0; i < 601; i++)
					{
						float az = DEG2RAD(az_el[0][i] - 90);
						float el = 90.0 - az_el[1][i];

						if (az_el[1][i] >= 0.0f)
							put_pixel(framebuffer, x + r * el / 90.0f * cos(az), y + r * el / 90.0f * sinf(az), 0x0000FF00);
					}
				}
			}
		}
	}

	kbd_cleanup(kbd);					// clean keyboard access
	fb_cleanup(framebuffer, ssize, fb); // clean framebuffer access

	return 0;
}
