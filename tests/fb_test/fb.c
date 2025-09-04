/*
 * Simple key scan app with framebuffer access.
 *
 * Compile with `gcc -Wall -Wextra -O2 fb.c -o fb`
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

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

void draw_circle(uint32_t *fb, uint16_t cx, uint16_t cy, uint8_t r, uint32_t color, bool filled)
{
	float t1 = (float)r/16;
	uint16_t x = r;
	uint16_t y = 0;

	while (x > y)
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

				else if(ev.code == KEY_DOWN)
				{
					draw_circle(framebuffer, RES_X/2, RES_Y/2, RES_Y/2-1, 0x00FFFFFF, false);
				}

				if (ev.code == KEY_ESC)
				{
					//black
					for (size_t i = 0; i < ssize / 4; i++)
					{
						framebuffer[i] = 0;
					}
				}
			}
		}
	}

	kbd_cleanup(kbd);					// clean keyboard access
	fb_cleanup(framebuffer, ssize, fb); // clean framebuffer access

	return 0;
}
