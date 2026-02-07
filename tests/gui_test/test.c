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
#include <sys/wait.h>
#include <linux/fb.h>
#include <time.h>
#include <raylib.h>
#include <linux/input.h>
#include <zmq.h>
#include <arpa/inet.h>
#include <sx1255.h>
#include <liblinht-ctrl.h>
#include <cyaml/cyaml.h>
#include <sqlite3.h>
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
	IMG_COUNT
};

Texture2D texture[IMG_COUNT];

enum
{
	DISP_VFO,
	DISP_MSG
};

uint8_t disp_state = DISP_VFO;

Font customFont, customFont10, customFont12;

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
uint16_t last_pressed[12];
const uint16_t key_seq_1[11] = {KEY_UP, KEY_UP, KEY_DOWN, KEY_DOWN,
								KEY_LEFT, KEY_RIGHT, KEY_LEFT, KEY_RIGHT,
								KEY_ENTER, KEY_ESC, KEY_2};

// ZeroMQ and PMT
const char *zmq_ipc = "ipc:///tmp/ptt_msg";
const char *aux_ipc = "ipc:///tmp/fg_aux_data_out";

uint8_t sot_pmt[10];
uint8_t eot_pmt[10];
uint8_t pmt_len; // "SOT" and "EOT" PMTs are the same length - single variable is fine

// states
bool vfo_a_tx = false;
bool vfo_b_tx = false;
time_t esc_start;
bool esc_pressed = false;

// spawning flowgraphs (python)
pid_t fg_pid;

// messaging
const char db_path[128] = "/var/lib/linht/messages.db";

typedef struct
{
	char src[10];
	char dst[10];
	uint16_t type;
	uint8_t meta[14];
	char text[825 - 4];
} m17_msg_t;
m17_msg_t last_msg;

uint16_t last_id;
typedef struct message
{
	uint16_t id;
	uint32_t timestamp;
	char protocol[32];
	char src[64];
	char dst[64];
	uint8_t meta[14];
	char message[1024];
	bool read;
} message_t;
message_t msg;

// misc
uint8_t redraw_req = 1;
Color bkg_color = BLACK;
Color top_bar_color = {0x20, 0x20, 0x20, 0xFF};
Color line_color = {0x60, 0x60, 0x60, 0xFF};
time_t t;
struct tm *time_info;
char time_s[10], time_s_last[10];
uint8_t gnss_display = 1, gnss_display_last;
char bv[8], bv_last[8];
Color bv_col = WHITE;

void load_gfx(void)
{
	Image img;

	img = LoadImage(IMG_PATH "/wallpaper.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_WALLPAPER] = LoadTextureFromImage(img);
	UnloadImage(img);

	img = LoadImage(IMG_PATH "/mute.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_MUTE] = LoadTextureFromImage(img);
	UnloadImage(img);

	img = LoadImage(IMG_PATH "/gnss.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_GNSS] = LoadTextureFromImage(img);
	UnloadImage(img);

	img = LoadImage(IMG_PATH "/batt_100.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_BATT_100] = LoadTextureFromImage(img);
	UnloadImage(img);

	img = LoadImage(IMG_PATH "/vfo_act.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_VFO_ACT] = LoadTextureFromImage(img);
	UnloadImage(img);

	img = LoadImage(IMG_PATH "/vfo_inact.png");
	ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
	texture[IMG_VFO_INACT] = LoadTextureFromImage(img);
	UnloadImage(img);
}

/*Texture2D RenderTextToTexture(const char *text, Font font, int fontSize, Color color)
{
	Vector2 size = MeasureTextEx(font, text, fontSize, 0);
	Image img = GenImageColor(size.x, size.y, BLANK);

	ImageDrawTextEx(
		&img,
		font,
		text,
		(Vector2){0, 0},
		fontSize,
		0,
		color
	);

	Texture2D tex = LoadTextureFromImage(img);
	UnloadImage(img);
	return tex;
}*/

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

// display misc stuff
static void DrawTextBoxedSelectable(Font font, const char *text, Rectangle rec, float fontSize, float spacing, bool wordWrap, Color tint, int selectStart, int selectLength, Color selectTint, Color selectBackTint)
{
	int length = TextLength(text); // Total length in bytes of the text, scanned by codepoints in loop

	float textOffsetY = 0;	  // Offset between lines (on line break '\n')
	float textOffsetX = 0.0f; // Offset X to next character to draw

	float scaleFactor = fontSize / (float)font.baseSize; // Character rectangle scaling factor

	// Word/character wrapping mechanism variables
	enum
	{
		MEASURE_STATE = 0,
		DRAW_STATE = 1
	};
	int state = wordWrap ? MEASURE_STATE : DRAW_STATE;

	int startLine = -1; // Index where to begin drawing (where a line begins)
	int endLine = -1;	// Index where to stop drawing (where a line ends)
	int lastk = -1;		// Holds last value of the character position

	for (int i = 0, k = 0; i < length; i++, k++)
	{
		// Get next codepoint from byte string and glyph index in font
		int codepointByteCount = 0;
		int codepoint = GetCodepoint(&text[i], &codepointByteCount);
		int index = GetGlyphIndex(font, codepoint);

		// NOTE: Normally we exit the decoding sequence as soon as a bad byte is found (and return 0x3f)
		// but we need to draw all of the bad bytes using the '?' symbol moving one byte
		if (codepoint == 0x3f)
			codepointByteCount = 1;
		i += (codepointByteCount - 1);

		float glyphWidth = 0;
		if (codepoint != '\n')
		{
			glyphWidth = (font.glyphs[index].advanceX == 0) ? font.recs[index].width * scaleFactor : font.glyphs[index].advanceX * scaleFactor;

			if (i + 1 < length)
				glyphWidth = glyphWidth + spacing;
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
			if ((codepoint == ' ') || (codepoint == '\t') || (codepoint == '\n'))
				endLine = i;

			if ((textOffsetX + glyphWidth) > rec.width)
			{
				endLine = (endLine < 1) ? i : endLine;
				if (i == endLine)
					endLine -= codepointByteCount;
				if ((startLine + codepointByteCount) == endLine)
					endLine = (i - codepointByteCount);

				state = !state;
			}
			else if ((i + 1) == length)
			{
				endLine = i;
				state = !state;
			}
			else if (codepoint == '\n')
				state = !state;

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
					textOffsetY += (font.baseSize + font.baseSize / 2) * scaleFactor / 1.5;
					textOffsetX = 0;
				}
			}
			else
			{
				if (!wordWrap && ((textOffsetX + glyphWidth) > rec.width))
				{
					textOffsetY += (font.baseSize + font.baseSize / 2) * scaleFactor / 1.5;
					textOffsetX = 0;
				}

				// When text overflows rectangle height limit, just stop drawing
				if ((textOffsetY + font.baseSize * scaleFactor) > rec.height)
					break;

				// Draw selection background
				bool isGlyphSelected = false;
				if ((selectStart >= 0) && (k >= selectStart) && (k < (selectStart + selectLength)))
				{
					DrawRectangleRec((Rectangle){rec.x + textOffsetX - 1, rec.y + textOffsetY, glyphWidth, (float)font.baseSize * scaleFactor}, selectBackTint);
					isGlyphSelected = true;
				}

				// Draw current character glyph
				if ((codepoint != ' ') && (codepoint != '\t'))
				{
					DrawTextCodepoint(font, codepoint, (Vector2){rec.x + textOffsetX, rec.y + textOffsetY}, fontSize, isGlyphSelected ? selectTint : tint);
				}
			}

			if (wordWrap && (i == endLine))
			{
				textOffsetY += (font.baseSize + font.baseSize / 2) * scaleFactor / 1.5;
				textOffsetX = 0;
				startLine = endLine;
				endLine = -1;
				glyphWidth = 0;
				selectStart += lastk - k;
				k = lastk;

				state = !state;
			}
		}

		if ((textOffsetX != 0) || (codepoint != ' '))
			textOffsetX += glyphWidth; // avoid leading spaces
	}
}

static void DrawTextBoxed(Font font, const char *text, Rectangle rec, float fontSize, float spacing, bool wordWrap, Color tint)
{
	DrawTextBoxedSelectable(font, text, rec, fontSize, spacing, wordWrap, tint, 0, 0, WHITE, WHITE);
}

void displayMessage(const char *src, const char *dst, const char *msg)
{
	char src_line[10], dst_line[10];

	sprintf(src_line, "From: %s\n", src);
	(void)dst;
	(void)dst_line;

	// clear drawing area
	DrawRectangle(0, 18, RES_X - 1, RES_Y - 1, bkg_color);

	DrawTextEx(customFont, src_line, (Vector2){2.0f, 20.0f}, 14.0f, 0, ORANGE);
	DrawTextBoxed(customFont, msg, (Rectangle){2, 40, RES_X - 1 - 2, RES_Y - 1 - 2}, 14.0f, 0, true, WHITE);
}

static inline uint16_t rd_u16(const uint8_t *p)
{
	uint16_t v = ((uint16_t)p[0] << 8) | p[1];
	return v;
}

static inline uint32_t rd_u32(const uint8_t *p)
{
	uint32_t v = ((uint32_t)p[0] << 24) |
				 ((uint32_t)p[1] << 16) |
				 ((uint32_t)p[2] << 8) |
				 p[3];
	return v;
}

// extract message data (SRC, DST, TYPE, META, SMS)
void getMsgData(char *src, char *dst, uint16_t *type, uint8_t meta[14], char *msg, const uint8_t *buf, size_t len)
{
	uint16_t idx = 0;
	uint32_t field_len = 0;
	char key[16] = {0};

	if (src != NULL)
		src[0] = 0;
	if (dst != NULL)
		dst[0] = 0;
	if (msg != NULL)
		msg[0] = 0;
	if (type != NULL)
		*type = 0xFFFF;

	// NOTE: the parser below is based on reverse-engineered PMT format...
	while (idx < len - 1)
	{
		if (buf[idx] == 0x09) // 0x09 - PMT_PAIR
		{
			idx++;
			// omit serialization depth marker byte
			idx++;
		}
		else if (buf[idx] == 0x02) // 0x02 - PMT_SYMBOL
		{
			idx++;
			field_len = rd_u16(&buf[idx]);
			idx += 2;
			memset(key, 0, field_len + 1); // +1 for the terminating null
			memcpy(key, &buf[idx], field_len);
			idx += field_len;
			if (memcmp(key, "src", 3) == 0)
			{
				idx++; // omit the 0x02 byte for the value field of this pair
				field_len = rd_u16(&buf[idx]);
				idx += 2;
				if (src != NULL)
				{
					memset(src, 0, field_len + 1); // +1 for the terminating null
					memcpy(src, &buf[idx], field_len);
				}
				idx += field_len;
			}
			else if (memcmp(key, "dst", 3) == 0)
			{
				idx++; // omit the 0x02 byte for the value field of this pair
				field_len = rd_u16(&buf[idx]);
				idx += 2;
				if (dst != NULL)
				{
					memset(dst, 0, field_len + 1); // +1 for the terminating null
					memcpy(dst, &buf[idx], field_len);
				}
				idx += field_len;
			}
			else if (memcmp(key, "sms", 3) == 0)
			{
				idx++; // omit the 0x02 byte for the value field of this pair
				field_len = rd_u16(&buf[idx]);
				idx += 2;
				if (msg != NULL)
				{
					memset(msg, 0, field_len + 1); // +1 for the terminating null
					memcpy(msg, &buf[idx], field_len);
				}
				idx += field_len;
			}
			else if (memcmp(key, "type", 4) == 0)
			{
				idx++; // omit the 0x0A byte for the value field of this pair
				idx++; // omit 1 byte
				field_len = rd_u32(&buf[idx]);
				idx += 4;
				idx += 2; // omit 2 bytes
				if (type != NULL)
					*type = ((uint16_t)buf[idx] << 8) | buf[idx + 1]; // expected 2 bytes
				idx += field_len;
			}
			else if (memcmp(key, "meta", 4) == 0)
			{
				idx++; // omit the 0x0A byte for the value field of this pair
				idx++; // omit 1 byte
				field_len = rd_u32(&buf[idx]);
				idx += 4;
				idx += 2; // omit 2 bytes
				if (meta != NULL)
					memcpy(meta, &buf[idx], field_len);
				idx += field_len;
			}
			else // unexpected key
			{
				; // TODO: handle this properly
			}
		}
	}

	if (src != NULL)
		if (src[0] == 0)
			strcpy(src, "<unknown>");
	if (dst != NULL)
		if (dst[0] == 0)
			strcpy(dst, "<unknown>");
	if (msg != NULL)
		if (msg[0] == 0)
			strcpy(msg, "<empty>");
}

// message database handlers
int db_init(const char *db_path)
{
	sqlite3 *db;
	char *err_msg = 0;
	int retval;

	retval = sqlite3_open(db_path, &db);
	if (retval != SQLITE_OK)
	{
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		return 1;
	}

	const char *create_sql =
		"CREATE TABLE IF NOT EXISTS messages ("
		"id INTEGER PRIMARY KEY,"
		"timestamp INTEGER,"
		"protocol TEXT,"
		"source TEXT,"
		"destination TEXT,"
		"meta BLOB,"
		"message TEXT,"
		"read INTEGER"
		");";

	retval = sqlite3_exec(db, create_sql, 0, 0, &err_msg);

	if (retval != SQLITE_OK)
	{
		fprintf(stderr, "Failed to ensure table exists: %s\n", err_msg);
		sqlite3_free(err_msg);
		sqlite3_close(db);
		return 1;
	}

	sqlite3_close(db);
	return 0;
}

int push_message(const char *db_path, message_t msg)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	int retval;

	retval = sqlite3_open(db_path, &db);
	if (retval != SQLITE_OK)
	{
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		return 1;
	}

	// prepare SQL with placeholders
	const char *sql = "INSERT INTO messages (timestamp, protocol, source, destination, meta, message, read) "
					  "VALUES (?, ?, ?, ?, ?, ?, ?);";

	retval = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
	if (retval != SQLITE_OK)
	{
		fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return 1;
	}

	// bind values
	sqlite3_bind_int64(stmt, 1, (sqlite3_int64)msg.timestamp);			   // timestamp (seconds since epoch)
	sqlite3_bind_text(stmt, 2, msg.protocol, -1, SQLITE_STATIC);		   // protocol
	sqlite3_bind_text(stmt, 3, msg.src, -1, SQLITE_STATIC);				   // source
	sqlite3_bind_text(stmt, 4, msg.dst, -1, SQLITE_STATIC);				   // destination
	sqlite3_bind_blob(stmt, 5, msg.meta, sizeof(msg.meta), SQLITE_STATIC); // meta
	sqlite3_bind_text(stmt, 6, msg.message, -1, SQLITE_STATIC);			   // message
	sqlite3_bind_int(stmt, 7, 0);										   // read flag (0 = unread)

	// execute
	retval = sqlite3_step(stmt);
	if (retval != SQLITE_DONE)
	{
		fprintf(stderr, "Insert failed: %s\n", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return 1;
	}
	else
	{
		// printf("New message inserted.\n");
	}

	sqlite3_finalize(stmt);
	sqlite3_close(db);

	return 0;
}

int get_message_count(const char *db_path)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	int retval, count = 0;

	retval = sqlite3_open(db_path, &db);
	if (retval != SQLITE_OK)
	{
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		return -1;
	}

	retval = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM messages;", -1, &stmt, 0);
	if (retval == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
	{
		count = sqlite3_column_int(stmt, 0);
	}
	else
	{
		fprintf(stderr, "Query failed: %s\n", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return -1;
	}

	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return count;
}

int get_unread_message_count(const char *db_path)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	int retval, count = 0;

	retval = sqlite3_open(db_path, &db);
	if (retval != SQLITE_OK)
	{
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		return -1;
	}

	retval = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM messages WHERE read = 0;", -1, &stmt, 0);
	if (retval == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
	{
		count = sqlite3_column_int(stmt, 0);
	}
	else
	{
		fprintf(stderr, "Query failed: %s\n", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return -1;
	}

	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return count;
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
	uint16_t vfo_a_tx_sust = conf->channels.vfo_0.tx_sust;
	uint16_t vfo_b_tx_sust = conf->channels.vfo_1.tx_sust;
	uint16_t rf_rate = conf->frontend.rf_sample_rate;
	float freq_corr = conf->settings.rf.freq_corr;
	uint8_t lna_gain = conf->frontend.lna_gain;
	uint8_t pga_gain = conf->frontend.pga_gain;
	int8_t dac_gain = conf->frontend.dac_gain;
	float mix_gain = conf->frontend.mix_gain;

	// settings printout
	if (1)
	{
		fprintf(stderr, "Loaded settings:\n");
		fprintf(stderr, "       VFO A RX  %d Hz\n", vfo_a_rx_f);
		fprintf(stderr, "             TX  %d Hz\n", vfo_a_tx_f);
		fprintf(stderr, "       VFO B RX  %d Hz\n", vfo_b_rx_f);
		fprintf(stderr, "             TX  %d Hz\n", vfo_b_tx_f);
		fprintf(stderr, "    Freq. corr.  %+.3f ppm\n", freq_corr);
		fprintf(stderr, "    I DC offset  %+.3f\n", conf->settings.rf.i_dc);
		fprintf(stderr, "    Q DC offset  %+.3f\n", conf->settings.rf.q_dc);
		fprintf(stderr, "     IQ balance  %+.4f\n", conf->settings.rf.iq_bal);
		fprintf(stderr, "       IQ angle  %+.1f deg.\n", conf->settings.rf.iq_theta);
		fprintf(stderr, " RF sample rate  %d kHz\n", rf_rate);
		fprintf(stderr, "       LNA gain  %d dB\n", lna_gain);
		fprintf(stderr, "       PGA gain  %d dB\n", pga_gain);
		fprintf(stderr, "       DAC gain  %d dB\n", dac_gain);
		fprintf(stderr, "     Mixer gain  %.1f dB\n", mix_gain);
		fprintf(stderr, " VFO A TX sust.  %d ms\n", vfo_a_tx_sust);
		fprintf(stderr, " VFO B TX sust.  %d ms\n", vfo_b_tx_sust);
		fprintf(stderr, "-----------------------------\n");
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
	if (rf_rate == 500)
		sx1255_set_rate(SX1255_RATE_500K);
	else if (rf_rate == 250)
		sx1255_set_rate(SX1255_RATE_250K);
	else if (rf_rate == 125)
		sx1255_set_rate(SX1255_RATE_125K);
	sx1255_set_rx_freq(vfo_a_rx_f * (1.0 + freq_corr * 1e-6));
	sx1255_set_tx_freq(vfo_a_tx_f * (1.0 + freq_corr * 1e-6));
	sx1255_set_lna_gain(lna_gain);
	sx1255_set_pga_gain(pga_gain);
	sx1255_set_dac_gain(dac_gain);
	sx1255_set_mixer_gain(mix_gain);
	sx1255_enable_rx(true);
	sx1255_enable_tx(true);
	sx1255_pa_enable(false);
	linht_ctrl_tx_rx_switch_set(true);
	fprintf(stderr, "SX1255 setup finished\n");

	// initialize text message database
	db_init(db_path); // make sure an appropriate table in the DB exists

	if (db_init(db_path) != 0)
	{
		fprintf(stderr, "Database initialization failed. Check permissions or path.\n");
	}

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

	void *zmq_sub = zmq_socket(zmq_ctx, ZMQ_SUB);
	if (!zmq_sub)
	{
		fprintf(stderr, "ZeroMQ: Error spawning a SUB socket.\nExiting.\n");
		return -1;
	}

	zmq_setsockopt(zmq_sub, ZMQ_SUBSCRIBE, "", 0);

	if (zmq_connect(zmq_sub, aux_ipc) != 0)
	{
		fprintf(stderr, "ZeroMQ: Cannot connect to %s.\nExiting.\n", aux_ipc);
		return -1;
	}

	pmt_len = string_to_pmt(sot_pmt, "SOT");
	string_to_pmt(eot_pmt, "EOT");

	// config TX sustain time
	// this should be done per VFO
	// but we assume we use only VFO A
	char sust_time[16] = {0};
	uint8_t sust_pmt[24] = {0};
	snprintf(sust_time, sizeof(sust_time), "SUST%d", vfo_a_tx_sust);
	uint8_t sust_pmt_len = string_to_pmt(sust_pmt, sust_time);
	zmq_send(zmq_pub, sust_pmt, sust_pmt_len, 0);

	// init Raylib
	fprintf(stderr, "Initializing Raylib...\n");
	if (!raylib_debug)
		SetTraceLogLevel(LOG_NONE);
	InitWindow(RES_X, RES_Y, "GUI test");
	SetWindowState(FLAG_VSYNC_HINT);

	customFont = LoadFontEx("/usr/share/linht/fonts/Ubuntu-Regular.ttf", 28, 0, 250);
	customFont10 = LoadFontEx("/usr/share/linht/fonts/UbuntuCondensed-Regular.ttf", 10, 0, 250);
	customFont12 = LoadFontEx("/usr/share/linht/fonts/UbuntuCondensed-Regular.ttf", 12, 0, 250);

	// load images
	load_gfx();

	// set FPS
	SetTargetFPS(15);

	// execute FG, TODO: the parameters are only OK for M17 FG
	char *fg_path = conf->channels.vfo_0.fg;
	fprintf(stderr, "Executing GNU Radio flowgraph (%s)\n", fg_path);

	char offs_str[24], can_str[4];
	snprintf(offs_str, sizeof(offs_str), "%.4f+%.4fj", conf->settings.rf.i_dc, conf->settings.rf.q_dc);
	snprintf(can_str, sizeof(can_str), "%d", conf->channels.vfo_0.extra.can);

	fg_pid = fork();
	if (fg_pid == 0)
	{
		// child process
		// open /dev/null
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull < 0)
			exit(EXIT_FAILURE);

		// redirect both stdout and stderr to /dev/null
		dup2(devnull, STDOUT_FILENO);
		dup2(devnull, STDERR_FILENO);

		close(devnull); // close /dev/null

		execlp("python", "python",
			   fg_path,
			   "-o", offs_str,
			   "-S", conf->channels.vfo_0.extra.src,
			   "-D", conf->channels.vfo_0.extra.dst,
			   "-C", can_str,
			   (char *)NULL);

		// if execp fails
		fprintf(stderr, "Failed to execute GNU Radio flowgraph\n");
		exit(EXIT_FAILURE);
	}

	// ready!
	fprintf(stderr, "Ready! Awaiting commands...\n");

	// get time
	esc_start = time(NULL);

	// main loop
	while (!WindowShouldClose())
	{
		// poll for terminal events even if no redrawing is required
		PollInputEvents();

		// poll ZMQ (M17 messages etc.)
		zmq_pollitem_t items[] = {{zmq_sub, 0, ZMQ_POLLIN, 0}};

		int rc = zmq_poll(items, 1, 0); // timeout 0 = non-blocking
		if (rc > 0 && (items[0].revents & ZMQ_POLLIN))
		{
			uint8_t buf[1024];
			int len = zmq_recv(zmq_sub, buf, sizeof(buf), 0);
			if (len > 0)
			{
				// TYPE field is discarded for now
				// TODO: add it later
				getMsgData(msg.src, msg.dst, NULL, msg.meta, msg.message, buf, len);

				msg.timestamp = time(NULL);
				sprintf(msg.protocol, "M17");
				msg.read = 0;

				// dump to database
				fprintf(stderr, "Message from %s: %s\n", msg.src, msg.message);
				push_message(db_path, msg);

				// prepare for display
				strcpy(last_msg.src, msg.src);
				strcpy(last_msg.dst, msg.dst);
				strcpy(last_msg.text, msg.message);

				// clear the struct for a new message
				memset((uint8_t *)&msg, 0, sizeof(message_t));

				// blink
				// TODO: this is bad - blocking, fix it
				/*linht_ctrl_green_led_set(true);
				usleep(100e3);
				linht_ctrl_green_led_set(false);*/

				disp_state = DISP_MSG;
				redraw_req = 1;
			}
		}

		// check keypad events
		struct input_event ev;
		ssize_t n = read(kbd, &ev, sizeof(ev)); // non-blocking
		if (n == (ssize_t)sizeof(ev) && ev.type == EV_KEY)
		{
			if (ev.value == KEY_PRESS)
			{
				if (ev.code == KEY_P)
				{
					if (disp_state == DISP_VFO)
					{
						sx1255_enable_rx(false);
						sx1255_pa_enable(true);
						linht_ctrl_tx_rx_switch_set(false);
						linht_ctrl_red_led_set(true);
						zmq_send(zmq_pub, sot_pmt, pmt_len, 0);
						vfo_a_tx = true;
						fprintf(stderr, "PTT pressed\n");
						redraw_req = 1;
					}
				}
				else if (ev.code == KEY_UP)
				{
					if (disp_state == DISP_VFO)
					{
						vfo_a_rx_f += 12500;
						vfo_a_tx_f += 12500;
						sx1255_set_rx_freq(vfo_a_rx_f * (1.0 + freq_corr * 1e-6));
						sx1255_set_tx_freq(vfo_a_tx_f * (1.0 + freq_corr * 1e-6));
						redraw_req = 1;
					}
				}
				else if (ev.code == KEY_DOWN)
				{
					if (disp_state == DISP_VFO)
					{
						vfo_a_rx_f -= 12500;
						vfo_a_tx_f -= 12500;
						sx1255_set_rx_freq(vfo_a_rx_f * (1.0 + freq_corr * 1e-6));
						sx1255_set_tx_freq(vfo_a_tx_f * (1.0 + freq_corr * 1e-6));
						redraw_req = 1;
					}
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

					if (disp_state == DISP_MSG)
					{
						disp_state = DISP_VFO;
						redraw_req = 1;
					}
				}
				else
				{
					;
				}

				// remember last pressed keys
				for (uint8_t i = 0; i < 11; i++)
					last_pressed[i] = last_pressed[i + 1];
				last_pressed[11] = ev.code;

				// check against a secret combination :)
				if (memcmp((uint8_t *)&last_pressed[12 - 11], (uint8_t *)key_seq_1, sizeof(key_seq_1)) == 0)
				{
					fprintf(stderr, "Shutting down now.\n");
					system("shutdown now");
					return 0;
				}
			}
			else // KEY_RELEASE
			{
				if (ev.code == KEY_P)
				{
					if (disp_state == DISP_VFO)
					{
						zmq_send(zmq_pub, eot_pmt, pmt_len, 0);
						// we assume VFO A is being used
						// this is blocking - might be bad :)
						usleep(vfo_a_tx_sust * 1000);

						sx1255_enable_rx(true);
						sx1255_pa_enable(false);
						linht_ctrl_tx_rx_switch_set(true);
						linht_ctrl_red_led_set(false);
						vfo_a_tx = false;
						fprintf(stderr, "PTT released\n");
						redraw_req = 1;
					}
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
		if (esc_pressed && time(NULL) - esc_start >= 5)
			break;

		//'gnss' icon
		if (gnss_display == 1 && gnss_display_last == 0)
		{
			gnss_display_last = 1;
			redraw_req = 1;
		}
		else if (gnss_display == 0 && gnss_display_last == 1)
		{
			gnss_display_last = 0;
			redraw_req = 1;
		}

		// not so frequent checks - current time and battery voltage
		if (cnt % 200 == 0)
		{
			// battery voltage display
			FILE *fp = fopen("/sys/bus/iio/devices/iio:device0/in_voltage1_raw", "r");
			int value;
			uint16_t batt_mv = 0;

			if (!fp)
			{
				fprintf(stderr, "Failed to open IIO sysfs entry\n");
			}
			else
			{
				if (fscanf(fp, "%d", &value) == 1)
				{
					batt_mv = (uint16_t)(value / 4096.0 * 1.8 * (39.0 + 10.0) / 10.0 * 1000.0);
					snprintf(bv, sizeof(bv), "%d.%d", batt_mv / 1000, (batt_mv % 1000) / 100);
				}
				else
				{
					snprintf(bv, sizeof(bv), "?.?");
					fprintf(stderr, "Failed to read battery voltage\n");
				}

				fclose(fp);
			}

			if (batt_mv >= 7400)
				bv_col = WHITE;
			else if (batt_mv >= 7000)
				bv_col = ORANGE;
			else
				bv_col = RED;

			if (strcmp(bv, bv_last))
			{
				strcpy(bv_last, bv);
				redraw_req = 1;
			}

			// time display
			t = time(NULL);
			time_info = localtime(&t);
			snprintf(time_s, sizeof(time_s), "%02d:%02d", time_info->tm_hour, time_info->tm_min);
			if (strcmp(time_s, time_s_last))
			{
				strcpy(time_s_last, time_s);
				redraw_req = 1;
			}
		}

		if (redraw_req)
		{
			BeginDrawing();

			// clear screen
			ClearBackground(bkg_color);

			// draw the wallpaper
			// DrawTexture(texture[IMG_WALLPAPER], 0, 0, WHITE);

			// draw header
			DrawRectangle(0, 0, RES_X - 1, 17, top_bar_color);
			DrawLine(0, 17, RES_X - 1, 17, line_color);

			// top to bottom, left to right (more or less)
			DrawTextEx(customFont, time_s, (Vector2){2, 2}, 14, 0, WHITE);

			// GNSS
			if (gnss_display)
				DrawTexture(texture[IMG_GNSS], RES_X - 40, 1, WHITE);

			// battery voltage - icon or text
			// DrawTexture(texture[IMG_BATT_100], RES_X - 24, 2, WHITE);
			DrawTextEx(customFont, bv, (Vector2){RES_X - 20.0f, 2.0f}, 14.0f, 0, bv_col);

			if (disp_state == DISP_VFO)
			{
				// VFO A
				DrawTexture(texture[IMG_VFO_ACT], 2, 23, WHITE);
				DrawTextEx(customFont, "A", (Vector2){4.5f, 22.0f}, 16.0f, 0, WHITE);
				DrawTextEx(customFont10, "VFO", (Vector2){3.0f, 38.0f}, 10.0f, 1, WHITE);
				char freq_a_str[10];
				uint32_t freq_a = vfo_a_tx ? vfo_a_tx_f : vfo_a_rx_f;
				snprintf(freq_a_str, sizeof(freq_a_str), "%d.%03d", freq_a / 1000000, (freq_a % 1000000) / 1000);
				DrawTextEx(customFont, freq_a_str, (Vector2){21.0f, 18.0f}, (float)customFont.baseSize, 0, vfo_a_tx ? RED : WHITE);
				snprintf(freq_a_str, sizeof(freq_a_str), "%02d", (freq_a % 1000) / 10);
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
				snprintf(freq_b_str, sizeof(freq_b_str), "%d.%03d", freq_b / 1000000, (freq_b % 1000000) / 1000);
				DrawTextEx(customFont, freq_b_str, (Vector2){21.0f, 74.0f}, (float)customFont.baseSize, 0, WHITE);
				snprintf(freq_b_str, sizeof(freq_b_str), "%02d", (freq_b % 1000) / 10);
				DrawTextEx(customFont, freq_b_str, (Vector2){113.0f, 84.0f}, 16.0f, 0, WHITE);
				DrawTextEx(customFont12, "12.5k", (Vector2){21.0f, 100.0f}, 12.0f, 1, BLUE);
				DrawTextEx(customFont12, "127.3", (Vector2){50.0f, 100.0f}, 12.0f, 1, BLUE);
				DrawTextEx(customFont12, "-7.6M", (Vector2){79.0f, 100.0f}, 12.0f, 1, BLUE);
				DrawTextEx(customFont12, "FM", (Vector2){21.0f, 112.0f}, 12.0f, 1, GREEN);
				DrawTexture(texture[IMG_MUTE], 140, 84, WHITE); //'vfo b mute' icon
			}
			else if (disp_state == DISP_MSG)
			{
				displayMessage(last_msg.src, last_msg.dst, last_msg.text);
			}

			EndDrawing();

			redraw_req = 0;
		}

		// frame counter
		cnt++;
		cnt %= 5 * 200;

		// reduce CPU congestion
		usleep(5e3);
	}

	// cleanup
	fprintf(stderr, "Exit code caught. Cleaning up...\n");
	for (uint8_t i = 0; i < IMG_COUNT; i++)
		UnloadTexture(texture[i]);
	UnloadFont(customFont);
	UnloadFont(customFont10);
	UnloadFont(customFont12);
	kbd_cleanup(kbd);
	sx1255_cleanup();
	kill(fg_pid, SIGTERM); // kill FG
	waitpid(fg_pid, NULL, 0);
	zmq_unbind(zmq_pub, zmq_ipc);
	zmq_close(zmq_pub);
	zmq_ctx_destroy(zmq_ctx);
	cyaml_free(&cfg, &config_schema, conf, 0);
	CloseWindow();
	fprintf(stderr, "Cleanup done. Exiting.\n");

	return 0;
}
