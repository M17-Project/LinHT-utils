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

//this is problematic
//#include <linux/input.h>
struct input_event
{
    struct timeval time;
    __u16 type;
    __u16 code;
    __s32 value;
};

#include <raylib.h>
#include <sx1255.h>

#define RES_X 160
#define RES_Y 128

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

static void DrawTextBoxedSelectable(Font font, const char *text, Rectangle rec, float fontSize, float spacing, bool wordWrap, Color tint, int selectStart, int selectLength, Color selectTint, Color selectBackTint)
{
    int length = TextLength(text);  // Total length in bytes of the text, scanned by codepoints in loop

    float textOffsetY = 0;          // Offset between lines (on line break '\n')
    float textOffsetX = 0.0f;       // Offset X to next character to draw

    float scaleFactor = fontSize/(float)font.baseSize;     // Character rectangle scaling factor

    // Word/character wrapping mechanism variables
    enum { MEASURE_STATE = 0, DRAW_STATE = 1 };
    int state = wordWrap? MEASURE_STATE : DRAW_STATE;

    int startLine = -1;         // Index where to begin drawing (where a line begins)
    int endLine = -1;           // Index where to stop drawing (where a line ends)
    int lastk = -1;             // Holds last value of the character position

    for (int i = 0, k = 0; i < length; i++, k++)
    {
        // Get next codepoint from byte string and glyph index in font
        int codepointByteCount = 0;
        int codepoint = GetCodepoint(&text[i], &codepointByteCount);
        int index = GetGlyphIndex(font, codepoint);

        // NOTE: Normally we exit the decoding sequence as soon as a bad byte is found (and return 0x3f)
        // but we need to draw all of the bad bytes using the '?' symbol moving one byte
        if (codepoint == 0x3f) codepointByteCount = 1;
        i += (codepointByteCount - 1);

        float glyphWidth = 0;
        if (codepoint != '\n')
        {
            glyphWidth = (font.glyphs[index].advanceX == 0) ? font.recs[index].width*scaleFactor : font.glyphs[index].advanceX*scaleFactor;

            if (i + 1 < length) glyphWidth = glyphWidth + spacing;
        }

        // NOTE: When wordWrap is ON we first measure how much of the text we can draw before going outside of the rec container
        // We store this info in startLine and endLine, then we change states, draw the text between those two variables
        // and change states again and again recursively until the end of the text (or until we get outside of the container)
        // When wordWrap is OFF we don't need the measure state so we go to the drawing state immediately
        // and begin drawing on the next line before we can get outside the container
        if (state == MEASURE_STATE)
        {
            // TODO: There are multiple types of spaces in UNICODE, maybe it's a good idea to add support for more
            // Ref: http://jkorpela.fi/chars/spaces.html
            if ((codepoint == ' ') || (codepoint == '\t') || (codepoint == '\n')) endLine = i;

            if ((textOffsetX + glyphWidth) > rec.width)
            {
                endLine = (endLine < 1)? i : endLine;
                if (i == endLine) endLine -= codepointByteCount;
                if ((startLine + codepointByteCount) == endLine) endLine = (i - codepointByteCount);

                state = !state;
            }
            else if ((i + 1) == length)
            {
                endLine = i;
                state = !state;
            }
            else if (codepoint == '\n') state = !state;

            if (state == DRAW_STATE)
            {
                textOffsetX = 0;
                i = startLine;
                glyphWidth = 0;

                // Save character position when we switch states
                int tmp = lastk;
                lastk = k - 1;
                k = tmp;
            }
        }
        else
        {
            if (codepoint == '\n')
            {
                if (!wordWrap)
                {
                    textOffsetY += (font.baseSize + font.baseSize/2)*scaleFactor/1.5;
                    textOffsetX = 0;
                }
            }
            else
            {
                if (!wordWrap && ((textOffsetX + glyphWidth) > rec.width))
                {
                    textOffsetY += (font.baseSize + font.baseSize/2)*scaleFactor/1.5;
                    textOffsetX = 0;
                }

                // When text overflows rectangle height limit, just stop drawing
                if ((textOffsetY + font.baseSize*scaleFactor) > rec.height) break;

                // Draw selection background
                bool isGlyphSelected = false;
                if ((selectStart >= 0) && (k >= selectStart) && (k < (selectStart + selectLength)))
                {
                    DrawRectangleRec((Rectangle){ rec.x + textOffsetX - 1, rec.y + textOffsetY, glyphWidth, (float)font.baseSize*scaleFactor }, selectBackTint);
                    isGlyphSelected = true;
                }

                // Draw current character glyph
                if ((codepoint != ' ') && (codepoint != '\t'))
                {
                    DrawTextCodepoint(font, codepoint, (Vector2){ rec.x + textOffsetX, rec.y + textOffsetY }, fontSize, isGlyphSelected? selectTint : tint);
                }
            }

            if (wordWrap && (i == endLine))
            {
                textOffsetY += (font.baseSize + font.baseSize/2)*scaleFactor/1.5;
                textOffsetX = 0;
                startLine = endLine;
                endLine = -1;
                glyphWidth = 0;
                selectStart += lastk - k;
                k = lastk;

                state = !state;
            }
        }

        if ((textOffsetX != 0) || (codepoint != ' ')) textOffsetX += glyphWidth;  // avoid leading spaces
    }
}

static void DrawTextBoxed(Font font, const char *text, Rectangle rec, float fontSize, float spacing, bool wordWrap, Color tint)
{
    DrawTextBoxedSelectable(font, text, rec, fontSize, spacing, wordWrap, tint, 0, 0, WHITE, WHITE);
}

int main(void)
{
	uint32_t freq_a = 433475000/*, freq_b = 439212500*/;
	
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
				if (ev.code == KEY_UP)
				{
					freq_a += 12500;
					sx1255_set_rx_freq(freq_a);
				}
				else if (ev.code == KEY_DOWN)
				{
					freq_a -= 12500;
					sx1255_set_rx_freq(freq_a);
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

			//Text
			const char message[1024] = "Open-source hardware, Linux-based, SDR handheld transceiver. OpenHT successor with greatly simplified hardware - no FPGAs involved.";
			DrawTextEx(customFont, "From: UR/SP5WWP\n", (Vector2){2.0f, 20.0f}, 14.0f, 0, ORANGE);
			DrawTextBoxed(customFont, message, (Rectangle){ 2, 40, RES_X-1-2, RES_Y-1-2 }, 14.0f, 0, true, WHITE);
            
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
