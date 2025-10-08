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
#include <sx1255.h>

#define RES_X 160
#define RES_Y 128

// keymap and states
#define LINHT_KEY_ESC 1
#define LINHT_KEY_1 2
#define LINHT_KEY_2 3
#define LINHT_KEY_3 4
#define LINHT_KEY_4 5
#define LINHT_KEY_5 6
#define LINHT_KEY_6 7
#define LINHT_KEY_7 8
#define LINHT_KEY_8 9
#define LINHT_KEY_9 10
#define LINHT_KEY_0 11
#define LINHT_KEY_ENTER 28
#define LINHT_KEY_HASH 43
#define LINHT_KEY_KPASTERISK 55
#define LINHT_KEY_F1 59
#define LINHT_KEY_F2 60
#define LINHT_KEY_UP 103
#define LINHT_KEY_DOWN 108

#define KEY_PRESS 0
#define KEY_RELEASE 1

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

int main(void)
{
	uint32_t freq_a = 433475000, freq_b = 439212500;
	
	// SX1255
	if (sx1255_init(spi_device, gpio_chip_path, rst_pin_offset) != 0)
    {
        fprintf(stderr, "Can not initialize device\nExiting\n");
        return -1;
    }

	sx1255_reset();
	sx1255_set_rate(SX1255_RATE_500K);
	sx1255_set_rx_freq(freq_a);
	//sx1255_set_tx_freq(freq_a);
	sx1255_set_lna_gain(20);
	sx1255_set_pga_gain(24);
	sx1255_enable_rx(true);

	// keyboard
	if ((rval = kbd_init(&kbd, kbd_path)) != 0)
	{
		return rval;
	}

	Image img;
	Texture2D texture[6] = {0};
	
    // Initialize Raylib window with DRM backend (no X11)
    // When compiled with PLATFORM_DRM, this will automatically use the framebuffer
    InitWindow(RES_X, RES_Y, "Mockup");

	Font customFont = LoadFontEx("/usr/share/linht/fonts/Ubuntu-Regular.ttf", 28, 0, 250);
	Font customFont10 = LoadFontEx("/usr/share/linht/fonts/UbuntuCondensed-Regular.ttf", 10, 0, 250);
	Font customFont12 = LoadFontEx("/usr/share/linht/fonts/UbuntuCondensed-Regular.ttf", 12, 0, 250);
    
	//load the wallpaper
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
		fcntl(kbd, F_SETFL, fcntl(kbd, F_GETFL, 0) | O_NONBLOCK); //non-blocking access
		ssize_t n = read(kbd, &ev, sizeof(ev));
		if (n == (ssize_t)sizeof(ev))
		{
			if (ev.value == KEY_PRESS)
			{
				if (ev.code == LINHT_KEY_UP)
				{
					freq_a += 12500;
					sx1255_set_rx_freq(freq_a);
				}
				else if (ev.code == LINHT_KEY_DOWN)
				{
					freq_a -= 12500;
					sx1255_set_rx_freq(freq_a);
				}
				else if (ev.code == LINHT_KEY_ESC)
				{
					break;
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
            
            //draw the wallpaper
            DrawTexture(texture[0], 0, 0, WHITE);

			//draw header
			DrawRectangle(0, 0, RES_X, 17, (Color){0x20, 0x20, 0x20, 0xFF});
			DrawLine(0, 17, RES_X-1, 17, (Color){0x60, 0x60, 0x60, 0xFF});

			//time
			time_t t = time(NULL);
			struct tm *time_info = localtime(&t);
			char time_s[10];
			sprintf(time_s, "%02d:%02d", time_info->tm_hour, time_info->tm_min);
			DrawTextEx(customFont, time_s, (Vector2){ 2.0f, 2.0f }, 14.0f, 0, WHITE);

			//'gnss' icon
			DrawTexture(texture[2], RES_X-40, 1, WHITE);

			//battery
			DrawTexture(texture[3], RES_X-24, 2, WHITE);

			//VFO A
			DrawTexture(texture[4], 2, 23, WHITE);
			DrawTextEx(customFont, "A", (Vector2){ 4.5f, 22.0f }, 16.0f, 0, WHITE);
			DrawTextEx(customFont10, "VFO", (Vector2){ 3.0f, 38.0f }, 10.0f, 1, WHITE);
			char freq_a_str[10];
			sprintf(freq_a_str, "%d.%03d", freq_a/1000000, (freq_a-(freq_a/1000000)*1000000)/1000);
			DrawTextEx(customFont, freq_a_str, (Vector2){ 21.0f, 18.0f }, (float)customFont.baseSize, 0, WHITE);
			sprintf(freq_a_str, "%02d", (freq_a%1000)/10);
			DrawTextEx(customFont, freq_a_str, (Vector2){ 113.0f, 28.0f }, 16.0f, 0, WHITE);
			DrawTextEx(customFont12, "12.5k", (Vector2){ 21.0f, 44.0f }, 12.0f, 1, BLUE);
			DrawTextEx(customFont12, "127.3", (Vector2){ 50.0f, 44.0f }, 12.0f, 1, BLUE);
			DrawTextEx(customFont12, "-7.6M", (Vector2){ 79.0f, 44.0f }, 12.0f, 1, BLUE);
			DrawTextEx(customFont12, "FM", (Vector2){ 21.0f, 56.0f }, 12.0f, 1, GREEN);
			//DrawTexture(texture[1], 140, 28, WHITE); //'vfo a mute' icon

			//horizontal separating bar
			DrawLine(0, 74, RES_X-1, 74, (Color){0x60, 0x60, 0x60, 0xFF});

			//VFO B
			DrawTexture(texture[4], 2, 79, WHITE);
			DrawTextEx(customFont, "B", (Vector2){ 4.5f, 78.0f }, 16.0f, 0, WHITE);
			DrawTextEx(customFont10, "VFO", (Vector2){ 3.0f, 94.0f }, 10.0f, 1, WHITE);
			char freq_b_str[10];
			sprintf(freq_b_str, "%d.%03d", freq_b/1000000, (freq_b-(freq_b/1000000)*1000000)/1000);
			DrawTextEx(customFont, freq_b_str, (Vector2){ 21.0f, 74.0f }, (float)customFont.baseSize, 0, WHITE);
			sprintf(freq_b_str, "%02d", (freq_b%1000)/10);
			DrawTextEx(customFont, freq_b_str, (Vector2){ 113.0f, 84.0f }, 16.0f, 0, WHITE);
			DrawTextEx(customFont12, "12.5k", (Vector2){ 21.0f, 100.0f }, 12.0f, 1, BLUE);
			DrawTextEx(customFont12, "", (Vector2){ 50.0f, 100.0f }, 12.0f, 1, BLUE);
			DrawTextEx(customFont12, "-7.6M", (Vector2){ 79.0f, 100.0f }, 12.0f, 1, BLUE);
			DrawTextEx(customFont12, "M17", (Vector2){ 21.0f, 112.0f }, 12.0f, 1, GREEN);
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
    CloseWindow();
    
    return 0;
}
