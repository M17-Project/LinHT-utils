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
#include <sys/prctl.h>
#include <sys/file.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/fb.h>
#include <time.h>
#include <m17.h>
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
#define KEY_PRESS 1
#define KEY_RELEASE 0

#define arrlen(x) (sizeof(x) / sizeof(*x))

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

Font customFont, customFont10, customFont12, customFont38, customFont14;

// settings
config_t *conf;
const char *settings_file = "/usr/share/linht/settings.yaml";
char offs_str[24], can_str[4]; // for M17

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
const uint16_t key_seq_1[] = {KEY_UP, KEY_UP, KEY_DOWN, KEY_DOWN,
							  KEY_LEFT, KEY_RIGHT, KEY_LEFT, KEY_RIGHT,
							  KEY_ENTER, KEY_ESC, KEY_2};

// ZeroMQ and PMT
void *zmq_ctx;
void *zmq_ptt_pub;
void *zmq_fg_pub;
void *zmq_sub;

const char *ptt_ipc = "ipc:///tmp/ptt_msg";
const char *aux_fg_in_ipc = "ipc:///tmp/fg_aux_data_in";
const char *aux_fg_out_ipc = "ipc:///tmp/fg_aux_data_out";

uint8_t sot_pmt[10];
uint8_t eot_pmt[10];
uint8_t pmt_len; // "SOT" and "EOT" PMTs are the same length - single variable is fine

// states
bool vfo_a_tx = false;
bool vfo_b_tx = false;
time_t esc_start;
bool esc_pressed = false;

// spawning flowgraphs (python)
char *fg_path;
pid_t fg_pid;

// messaging
const char db_path[128] = "/var/lib/linht/messages.db";
sqlite3 *msg_db;
sqlite3_stmt *insert_msg_stmt;
sqlite3_stmt *count_msg_stmt;
sqlite3_stmt *unread_msg_count_stmt;

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

// M17
typedef struct
{
	char src[10];
	char dst[10];
	uint16_t type;
	uint8_t meta[14];
} m17_str_t;
m17_str_t last_str;

// battery voltage, read from the BQ25792 charger (Rev C has no battery
// divider on the SoM ADC)
const char chg_i2c_path[] = "/dev/i2c-2";
#define BQ25792_ADDR		0x6b
#define BQ25792_REG_ADC_CTRL	0x2E
#define BQ25792_REG_VBAT_ADC	0x3B
#define BQ25792_REG_PART_INFO	0x48
int chg_fd = -1;

// files
const char knob_volt[] = "/sys/bus/iio/devices/iio:device0/in_voltage0_raw";
int knob_fd;
uint8_t knob_pos;

// misc
uint8_t redraw_req = 1;
const Color bkg_color = BLACK;
const Color top_bar_color = {0x20, 0x20, 0x20, 0xFF};
const Color line_color = {0x60, 0x60, 0x60, 0xFF};
time_t t;
struct tm time_info;
char time_s[10], time_s_last[10];
uint8_t gnss_display, gnss_display_last;
uint8_t disp_vfo_b, disp_aux_data = 1;
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
	pmt[0] = 2;						 // pmt type - zmq message
	uint16_t l = htons(strlen(msg)); // length
	memcpy(&pmt[1], &l, sizeof(l));
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
	char src_line[32], dst_line[32];

	snprintf(src_line, sizeof(src_line), "From: %s\n", src);
	(void)dst;
	(void)dst_line;

	// clear drawing area
	DrawRectangle(0, 18, RES_X - 1, RES_Y - 1, bkg_color);

	DrawTextEx(customFont, src_line, (Vector2){2.0f, 20.0f}, 14.0f, 0, ORANGE);
	DrawTextBoxed(customFont, msg, (Rectangle){2, 40, RES_X - 1 - 2, RES_Y - 1 - 2}, 14.0f, 0, true, WHITE);
}

// one BQ25792 register access: write 'reg', then read 'len' bytes into 'rx'
// (repeated start), or write 'val' to 'reg' when rx is NULL
static int bq_xfer(uint8_t reg, uint8_t *rx, uint16_t len, uint8_t val)
{
	uint8_t tx[2] = {reg, val};
	struct i2c_msg msgs[2] = {
		{.addr = BQ25792_ADDR, .flags = 0, .len = (uint16_t)(rx ? 1 : 2), .buf = tx},
		{.addr = BQ25792_ADDR, .flags = I2C_M_RD, .len = len, .buf = rx},
	};
	struct i2c_rdwr_ioctl_data xfer = {.msgs = msgs, .nmsgs = rx ? 2 : 1};

	return (ioctl(chg_fd, I2C_RDWR, &xfer) == (int)xfer.nmsgs) ? 0 : -1;
}

// open the charger and check that it really is a BQ25792
static int batt_init(void)
{
	uint8_t part;

	chg_fd = open(chg_i2c_path, O_RDWR | O_CLOEXEC);
	if (chg_fd < 0)
		return -1;

	if (bq_xfer(BQ25792_REG_PART_INFO, &part, 1, 0) != 0 || ((part >> 3) & 7) != 1)
	{
		close(chg_fd);
		chg_fd = -1;
		return -1;
	}

	return 0;
}

// Battery voltage in mV, -1 on error, -2 when no new value is available
// (bus locked by another tool, or the ADC was just enabled).
// The ADC runs continuously once enabled. It is checked on every read, as
// another tool or a charger reset may have turned it off.
// The flock() on the bus device is shared with bq25792_status.py.
static int batt_read_mv(void)
{
	uint8_t ctrl, v[2];
	int ret = -1;

	if (chg_fd < 0)
		return -1;

	if (flock(chg_fd, LOCK_EX | LOCK_NB) != 0)
		return -2;

	if (bq_xfer(BQ25792_REG_ADC_CTRL, &ctrl, 1, 0) == 0)
	{
		if (!(ctrl & 0x80) || (ctrl & 0x40)) // off, or set to one-shot
		{
			// ADC_EN, continuous, 15-bit, no averaging, keep the reserved bits
			bq_xfer(BQ25792_REG_ADC_CTRL, NULL, 0, (uint8_t)((ctrl & 0x03) | 0x80));
			ret = -2;
		}
		else if (bq_xfer(BQ25792_REG_VBAT_ADC, v, 2, 0) == 0)
		{
			ret = ((int)v[0] << 8) | v[1]; // 1 mV per LSB
		}
	}

	flock(chg_fd, LOCK_UN);
	return ret;
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

// serialized PMT tags (GNU Radio pmt_serial_tags)
#define PST_TRUE		0x00
#define PST_FALSE		0x01
#define PST_SYMBOL		0x02
#define PST_INT32		0x03
#define PST_DOUBLE		0x04
#define PST_COMPLEX		0x05
#define PST_NULL		0x06
#define PST_PAIR		0x07
#define PST_DICT		0x09
#define PST_UNIFORM_VECTOR	0x0A
#define PST_UINT64		0x0B
#define PST_INT64		0x0D

// item size of a serialized uniform vector, 0 for an unknown item type
static size_t uvec_item_size(uint8_t utype)
{
	static const uint8_t sizes[] = {1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 8, 16}; // u8 s8 u16 s16 u32 s32 u64 s64 f32 f64 c32 c64
	return (utype < sizeof(sizes)) ? sizes[utype] : 0;
}

// copy a length-delimited string into a NUL-terminated buffer of 'size' bytes
static void copy_field(char *dst, size_t size, const uint8_t *src, size_t len)
{
	if (dst == NULL || size == 0)
		return;
	if (len > size - 1)
		len = size - 1;
	memcpy(dst, src, len);
	dst[len] = 0;
}

// Extract message data (SRC, DST, TYPE, META, SMS) from a serialized PMT
// dict, as published by the gr-m17 decoder. Every read is bounds checked.
// Unknown keys are skipped by their value type, parsing stops at the end of
// the dict or at anything it can not interpret.
void getMsgData(char *src, size_t src_size, char *dst, size_t dst_size, uint16_t *type,
				uint8_t meta[14], char *msg, size_t msg_size, const uint8_t *buf, size_t len)
{
	size_t idx = 0;

	if (src != NULL && src_size > 0)
		src[0] = 0;
	if (dst != NULL && dst_size > 0)
		dst[0] = 0;
	if (msg != NULL && msg_size > 0)
		msg[0] = 0;
	if (meta != NULL)
		memset(meta, 0, 14);
	if (type != NULL)
		*type = 0xFFFF;

	while (idx < len)
	{
		// dict structure: DICT, PAIR, key, value, ... NULL
		if (buf[idx] == PST_DICT || buf[idx] == PST_PAIR)
		{
			idx++;
			continue;
		}
		if (buf[idx] != PST_SYMBOL) // end of the dict (PST_NULL) or not understood
			break;

		// key
		if (idx + 3 > len)
			break;
		size_t key_len = rd_u16(&buf[idx + 1]);
		idx += 3;
		if (idx + key_len > len)
			break;
		const uint8_t *key = &buf[idx];
		idx += key_len;

		if (idx >= len)
			break;
		uint8_t tag = buf[idx++];

		if (tag == PST_SYMBOL) // string value
		{
			if (idx + 2 > len)
				break;
			size_t vlen = rd_u16(&buf[idx]);
			idx += 2;
			if (idx + vlen > len)
				break;

			if (key_len == 3 && memcmp(key, "src", 3) == 0)
				copy_field(src, src_size, &buf[idx], vlen);
			else if (key_len == 3 && memcmp(key, "dst", 3) == 0)
				copy_field(dst, dst_size, &buf[idx], vlen);
			else if (key_len == 3 && memcmp(key, "sms", 3) == 0)
				copy_field(msg, msg_size, &buf[idx], vlen);

			idx += vlen;
		}
		else if (tag == PST_UNIFORM_VECTOR) // item type, count, padding, items
		{
			if (idx + 6 > len)
				break;
			size_t isize = uvec_item_size(buf[idx]);
			size_t count = rd_u32(&buf[idx + 1]);
			size_t npad = buf[idx + 5];
			idx += 6 + npad;
			if (isize == 0 || count > len || idx > len || idx + count * isize > len)
				break;

			if (isize == 1 && key_len == 4 && memcmp(key, "type", 4) == 0)
			{
				if (type != NULL && count >= 2)
					*type = ((uint16_t)buf[idx] << 8) | buf[idx + 1];
			}
			else if (isize == 1 && key_len == 4 && memcmp(key, "meta", 4) == 0)
			{
				if (meta != NULL)
					memcpy(meta, &buf[idx], (count < 14) ? count : 14);
			}

			idx += count * isize;
		}
		else if (tag == PST_TRUE || tag == PST_FALSE || tag == PST_NULL)
			;
		else if (tag == PST_INT32)
			idx += 4;
		else if (tag == PST_DOUBLE || tag == PST_UINT64 || tag == PST_INT64)
			idx += 8;
		else if (tag == PST_COMPLEX)
			idx += 16;
		else // value type not understood, the rest can not be parsed
			break;
	}

	if (src != NULL && src_size > 0 && src[0] == 0)
		snprintf(src, src_size, "<unknown>");
	if (dst != NULL && dst_size > 0 && dst[0] == 0)
		snprintf(dst, dst_size, "<unknown>");
}

// message database handlers
// releases everything db_init() set up, safe to call more than once
void db_cleanup(void)
{
	sqlite3_finalize(insert_msg_stmt);
	sqlite3_finalize(count_msg_stmt);
	sqlite3_finalize(unread_msg_count_stmt);
	sqlite3_close(msg_db);
	insert_msg_stmt = NULL;
	count_msg_stmt = NULL;
	unread_msg_count_stmt = NULL;
	msg_db = NULL;
}

// opens the database into msg_db (db must point to it)
int db_init(sqlite3 **db, const char *db_path)
{
	char *err_msg = NULL;

	if (sqlite3_open(db_path, db) != SQLITE_OK)
	{
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(*db));
		db_cleanup();
		return 1;
	}

	sqlite3_busy_timeout(*db, 1000);
	sqlite3_exec(*db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	sqlite3_exec(*db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

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

	if (sqlite3_exec(*db, create_sql, NULL, NULL, &err_msg) != SQLITE_OK)
	{
		fprintf(stderr, "Failed to ensure table exists: %s\n", err_msg);
		sqlite3_free(err_msg);
		db_cleanup();
		return 1;
	}

	// prepare SQL with placeholders
	const char *sql = "INSERT INTO messages (timestamp, protocol, source, destination, meta, message, read) "
					  "VALUES (?, ?, ?, ?, ?, ?, ?);";

	if (sqlite3_prepare_v2(*db, sql, -1, &insert_msg_stmt, 0) != SQLITE_OK)
	{
		fprintf(stderr, "Failed to prepare placeholder statement: %s\n", sqlite3_errmsg(*db));
		db_cleanup();
		return 1;
	}

	// total message count
	const char *sql2 = "SELECT COUNT(*) FROM messages;";

	if (sqlite3_prepare_v2(*db, sql2, -1, &count_msg_stmt, 0) != SQLITE_OK)
	{
		fprintf(stderr, "Failed to prepare count statement: %s\n", sqlite3_errmsg(*db));
		db_cleanup();
		return 1;
	}

	// unread message count
	const char *sql3 = "SELECT COUNT(*) FROM messages WHERE read = 0;";

	if (sqlite3_prepare_v2(*db, sql3, -1, &unread_msg_count_stmt, 0) != SQLITE_OK)
	{
		fprintf(stderr, "Failed to prepare unread count statement: %s\n", sqlite3_errmsg(*db));
		db_cleanup();
		return 1;
	}

	return 0;
}


int push_message(message_t *msg)
{
	if (insert_msg_stmt == NULL) // no database
		return 1;

	sqlite3_clear_bindings(insert_msg_stmt);

	sqlite3_bind_int64(insert_msg_stmt, 1, (sqlite3_int64)msg->timestamp);				   // timestamp (seconds since epoch)
	sqlite3_bind_text(insert_msg_stmt, 2, msg->protocol, -1, SQLITE_TRANSIENT);			   // protocol
	sqlite3_bind_text(insert_msg_stmt, 3, msg->src, -1, SQLITE_TRANSIENT);				   // source
	sqlite3_bind_text(insert_msg_stmt, 4, msg->dst, -1, SQLITE_TRANSIENT);				   // destination
	sqlite3_bind_blob(insert_msg_stmt, 5, msg->meta, sizeof(msg->meta), SQLITE_TRANSIENT); // meta
	sqlite3_bind_text(insert_msg_stmt, 6, msg->message, -1, SQLITE_TRANSIENT);			   // message
	sqlite3_bind_int(insert_msg_stmt, 7, msg->read ? 1 : 0);							   // read flag (0 = unread)

	int retval = sqlite3_step(insert_msg_stmt);
	if (retval != SQLITE_DONE)
	{
		fprintf(stderr, "Insert failed (retval=%d): %s\n", retval, sqlite3_errmsg(msg_db));
		sqlite3_reset(insert_msg_stmt);
		return 1;
	}

	sqlite3_reset(insert_msg_stmt);
	return 0;
}

int get_message_count(void)
{
	int count = -1;

	if (count_msg_stmt == NULL) // no database
		return count;

	sqlite3_reset(count_msg_stmt);

	int retval = sqlite3_step(count_msg_stmt);
	if (retval == SQLITE_ROW)
		count = sqlite3_column_int(count_msg_stmt, 0);
	else
		fprintf(stderr, "Count query failed: %s\n", sqlite3_errmsg(msg_db));

	sqlite3_reset(count_msg_stmt);
	return count;
}

int get_unread_message_count(void)
{
	int count = -1;

	if (unread_msg_count_stmt == NULL) // no database
		return count;

	sqlite3_reset(unread_msg_count_stmt);

	int retval = sqlite3_step(unread_msg_count_stmt);
	if (retval == SQLITE_ROW)
		count = sqlite3_column_int(unread_msg_count_stmt, 0);
	else
		fprintf(stderr, "Unread count query failed: %s\n", sqlite3_errmsg(msg_db));

	sqlite3_reset(unread_msg_count_stmt);
	return count;
}

// messaging
void m17_send_sms(const char *msg)
{
	sx1255_enable_rx(false);
	sx1255_pa_enable(true);
	linht_ctrl_tx_rx_switch_set(true);
	linht_ctrl_red_led_set(true);

	// transmission start
	zmq_send(zmq_ptt_pub, sot_pmt, pmt_len, 0); // notify the ZMQ proxy
	usleep(50 * 1000); // let the proxy switch
	
	// "SMS":"msg" PMT pair, the text is limited to what one M17 packet holds:
	// 33 frames of 25 bytes = 825, minus the type byte, the NUL and the CRC
	size_t msg_len = strlen(msg);
	if (msg_len > 821)
		msg_len = 821;

	uint8_t pmt[10 + 821] = {0x07, 0x02, 0x00, 0x03, 0x53, 0x4D, 0x53, 0x02};
	pmt[8] = (uint8_t)(msg_len >> 8); // length, big endian
	pmt[9] = (uint8_t)(msg_len & 0xFF);
	memcpy(&pmt[10], msg, msg_len);
	zmq_send(zmq_fg_pub, pmt, 10 + msg_len, 0); // trigger baseband generation

	usleep(((3 + (1 + msg_len + 1 + 2) / 25) * 40 + 20) * 1000); // 20ms extra

	// transmission end
	zmq_send(zmq_ptt_pub, eot_pmt, pmt_len, 0); // notify the ZMQ proxy

	sx1255_enable_rx(true);
	sx1255_pa_enable(false);
	linht_ctrl_tx_rx_switch_set(false);
	linht_ctrl_red_led_set(false);
}

void fg_check(void)
{
	if (fg_pid <= 0)
		return;

	int status;
	pid_t r = waitpid(fg_pid, &status, WNOHANG);

	if (r == 0)
		return; // still running

	if (r == -1)
		return; // error or already reaped

	// if it died - resurrect it
	// re-set RF hardware to RX mode
	sx1255_pa_enable(false);
	sx1255_enable_rx(true);
	linht_ctrl_tx_rx_switch_set(false);
	linht_ctrl_red_led_set(false);
	vfo_a_tx = false;

	fprintf(stderr, "Flowgraph stopped");

	if (WIFSIGNALED(status))
		fprintf(stderr, " (signal %d)", WTERMSIG(status));

	if (WIFEXITED(status))
		fprintf(stderr, " (exit %d)", WEXITSTATUS(status));

	fprintf(stderr, "\nRestarting...\n");

	usleep(100 * 1000); // prevent aggressive restart loops

	// restart
	fg_pid = fork();
	if (fg_pid < 0)
	{
		perror("fork");
		fg_pid = 0; // never kill(-1, ...) at exit
		return;
	}
	if (fg_pid == 0)
	{
		int devnull = open("/dev/null", O_WRONLY);
		dup2(devnull, STDOUT_FILENO);
		dup2(devnull, STDERR_FILENO);
		close(devnull);

		prctl(PR_SET_PDEATHSIG, SIGTERM);

		execlp("python", "python",
			   conf->channels.vfo_0.fg,
			   "-o", offs_str,
			   "-S", conf->channels.vfo_0.extra.src,
			   "-D", conf->channels.vfo_0.extra.dst,
			   "-C", can_str,
			   (char *)NULL);

		fprintf(stderr, "Failed to execute GNU Radio flowgraph\n");
		exit(EXIT_FAILURE);
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
	float atten_0 = conf->frontend.atten_0;
	float atten_1 = conf->frontend.atten_1;

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
		fprintf(stderr, "    RX atten. 0  %.1f dB\n", atten_0);
		fprintf(stderr, "    RX atten. 1  %.1f dB\n", atten_1);
		fprintf(stderr, " VFO A TX sust.  %d ms\n", vfo_a_tx_sust);
		fprintf(stderr, " VFO B TX sust.  %d ms\n", vfo_b_tx_sust);
		fprintf(stderr, "-----------------------------\n");
	}

	// LEDs
	linht_ctrl_green_led_set(false);
	linht_ctrl_red_led_set(false);

	// battery voltage
	if (batt_init() != 0)
		fprintf(stderr, "Can not access the BQ25792 charger on %s, no battery voltage readout.\n", chg_i2c_path);

	// volume knob position
	knob_fd = open(knob_volt, O_RDONLY | O_CLOEXEC);
	if (knob_fd < 0)
	{
		fprintf(stderr, "Unable to perform volume knob voltage readout.\nExiting.\n");
		return -1;
	}

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

	// RX attenuators, values from the settings file
	if (linht_ctrl_atten_init() != 0)
	{
		fprintf(stderr, "Can not initialize RX attenuators.\n");
	}
	else
	{
		if (linht_ctrl_atten_set(1, atten_0) != 0)
			fprintf(stderr, "Invalid RX attenuator 0 setting: %.1f dB (allowed 0..31.5 dB)\n", atten_0);
		if (linht_ctrl_atten_set(2, atten_1) != 0)
			fprintf(stderr, "Invalid RX attenuator 1 setting: %.1f dB (allowed 0..31.5 dB)\n", atten_1);
	}

	// Disable PA on startup
	linht_ctrl_pa_enable_set(false);

	// Set to RF Switch to RX
	linht_ctrl_tx_rx_switch_set(false);

	fprintf(stderr, "SX1255 setup finished\n");

	// initialize text message database
	if (db_init(&msg_db, db_path) != 0)
	{
		fprintf(stderr, "Database initialization failed. Check permissions or path.\n");
	}

	// keyboard
	if ((rval = kbd_init(&kbd, kbd_path)) != 0)
	{
		return rval;
	}

	// ZeroMQ and PMT
	zmq_ctx = zmq_ctx_new();

	// PTT control (SOT/EOT for the ZMQ proxy)
	zmq_ptt_pub = zmq_socket(zmq_ctx, ZMQ_PUB);

	if (zmq_bind(zmq_ptt_pub, ptt_ipc) != 0)
	{
		fprintf(stderr, "ZeroMQ: Error binding to Unix socket %s.\nExiting.\n", ptt_ipc);
		return -1;
	}

	// audx data _to_ GR flowgraph (SMS for the M17 Encoder etc.)
	zmq_fg_pub = zmq_socket(zmq_ctx, ZMQ_PUB);
	if (!zmq_fg_pub)
	{
		fprintf(stderr, "ZeroMQ: Error spawning a PUB socket.\nExiting.\n");
		return -1;
	}

	if (zmq_connect(zmq_fg_pub, aux_fg_in_ipc) != 0)
	{
		fprintf(stderr, "ZeroMQ: Cannot connect to %s.\nExiting.\n", aux_fg_in_ipc);
		return -1;
	}

	// aux data _from_ GR flowgraph
	zmq_sub = zmq_socket(zmq_ctx, ZMQ_SUB);
	if (!zmq_sub)
	{
		fprintf(stderr, "ZeroMQ: Error spawning a SUB socket.\nExiting.\n");
		return -1;
	}

	zmq_setsockopt(zmq_sub, ZMQ_SUBSCRIBE, "", 0);

	if (zmq_connect(zmq_sub, aux_fg_out_ipc) != 0)
	{
		fprintf(stderr, "ZeroMQ: Cannot connect to %s.\nExiting.\n", aux_fg_out_ipc);
		return -1;
	}

	pmt_len = string_to_pmt(sot_pmt, "SOT");
	string_to_pmt(eot_pmt, "EOT");

	// init Raylib
	fprintf(stderr, "Initializing Raylib...\n");
	if (!raylib_debug)
		SetTraceLogLevel(LOG_NONE);
	InitWindow(RES_X, RES_Y, "GUI test");
	SetWindowState(FLAG_VSYNC_HINT);

	customFont = LoadFontEx("/usr/share/linht/fonts/Ubuntu-Regular.ttf", 28, 0, 250);
	customFont10 = LoadFontEx("/usr/share/linht/fonts/UbuntuCondensed-Regular.ttf", 10, 0, 250);
	customFont12 = LoadFontEx("/usr/share/linht/fonts/UbuntuCondensed-Regular.ttf", 12, 0, 250);
	customFont14 = LoadFontEx("/usr/share/linht/fonts/Ubuntu-Regular.ttf", 14, 0, 250);
	customFont38 = LoadFontEx("/usr/share/linht/fonts/Ubuntu-Regular.ttf", 38, 0, 250);

	// load images
	load_gfx();

	// set FPS
	SetTargetFPS(15);

	// execute FG, TODO: the parameters are only OK for M17 FG
	fg_path = conf->channels.vfo_0.fg;
	fprintf(stderr, "Executing GNU Radio flowgraph (%s)\n", fg_path);

	snprintf(offs_str, sizeof(offs_str), "%.4f%+.4fj", conf->settings.rf.i_dc, conf->settings.rf.q_dc);
	snprintf(can_str, sizeof(can_str), "%d", conf->channels.vfo_0.extra.can);

	fg_pid = fork();
	if (fg_pid < 0)
	{
		perror("fork");
		fg_pid = 0; // never kill(-1, ...) at exit
	}
	else if (fg_pid == 0)
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

		prctl(PR_SET_PDEATHSIG, SIGTERM);
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
		// check if the flowgraph is still running
		fg_check();

		// poll for terminal events even if no redrawing is required
		PollInputEvents();

		// poll ZMQ (M17 messages etc.)
		zmq_pollitem_t items[] = {{zmq_sub, 0, ZMQ_POLLIN, 0}};

		int rc = zmq_poll(items, 1, 0); // timeout 0 = non-blocking
		if (rc > 0 && (items[0].revents & ZMQ_POLLIN))
		{
			// a full 821-byte SMS plus the other fields is about 900 bytes
			uint8_t buf[2048];
			int len = zmq_recv(zmq_sub, buf, sizeof(buf), 0);
			if (len > (int)sizeof(buf))
			{
				// zmq_recv() returns the full size but stored only sizeof(buf)
				fprintf(stderr, "Dropped a %d-byte message from the flowgraph (too long)\n", len);
			}
			else if (len > 0)
			{
				uint16_t type;
				getMsgData(msg.src, sizeof(msg.src), msg.dst, sizeof(msg.dst), &type,
						   msg.meta, msg.message, sizeof(msg.message), buf, (size_t)len);

				// save to the database and display the contents only if it's a packet
				if ((type & 1) == 0 && strlen(msg.message))
				{
					msg.timestamp = time(NULL);
					sprintf(msg.protocol, "M17");
					msg.read = 1;

					// dump to database
					fprintf(stderr, "Message from %s: %s\n", msg.src, msg.message);
					push_message(&msg);

					// prepare for display
					snprintf(last_msg.src, sizeof(last_msg.src), "%s", msg.src);
					snprintf(last_msg.dst, sizeof(last_msg.dst), "%s", msg.dst);
					snprintf(last_msg.text, sizeof(last_msg.text), "%s", msg.message);

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

				// if it's a stream
				else if (type & 1)
				{
					// extract data
					snprintf(last_str.src, sizeof(last_str.src), "%s", msg.src);
					snprintf(last_str.dst, sizeof(last_str.dst), "%s", msg.dst);
					memcpy(&last_str.type, &type, sizeof(type));
					memcpy(last_str.meta, &msg.meta, sizeof(msg.meta));
					redraw_req = 1;
				}
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
						linht_ctrl_tx_rx_switch_set(true); // TX
						linht_ctrl_red_led_set(true);
						zmq_send(zmq_ptt_pub, sot_pmt, pmt_len, 0); // notify the ZMQ proxy
						zmq_send(zmq_fg_pub, sot_pmt, pmt_len, 0);	// notify the GR flowgraph
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
				else if (ev.code == KEY_0)
				{
					m17_send_sms("Test message!");
				}
				else
				{
					;
				}

				// remember last pressed keys
				for (uint8_t i = 0; i < arrlen(last_pressed) - 1; i++)
					last_pressed[i] = last_pressed[i + 1];
				last_pressed[arrlen(last_pressed) - 1] = ev.code;

				// check against a secret combination :)
				if (memcmp(&last_pressed[arrlen(last_pressed) - arrlen(key_seq_1)], key_seq_1, sizeof(key_seq_1)) == 0)
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
						zmq_send(zmq_fg_pub, eot_pmt, pmt_len, 0); // notify the GR flowgraph
						// we assume VFO A is being used
						// this is blocking - might be bad :)
						usleep(vfo_a_tx_sust * 1000);
						zmq_send(zmq_ptt_pub, eot_pmt, pmt_len, 0); // notify the ZMQ proxy

						sx1255_enable_rx(true);
						sx1255_pa_enable(false);
						linht_ctrl_tx_rx_switch_set(false); // RX
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

		// less critical checks
		char buf[16] = {0};
		if (cnt % 20 == 0)
		{
			// volume knob position
			lseek(knob_fd, 0, SEEK_SET);
			n = read(knob_fd, buf, sizeof(buf) - 1);

			if (n > 0)
			{
				buf[n] = 0;
				int value = atoi(buf);
				uint8_t tmp = (uint8_t)(255.0f * value / 3000.0f); // 3000 is slightly more than maximum
				if (abs(knob_pos - tmp) > 5)					   // reduce wiggle
				{
					knob_pos = tmp;
					redraw_req = 1;
				}
			}
		}

		// occasional checks - current time and battery voltage
		if (cnt % 200 == 0)
		{
			// battery voltage display (-2: no new value, keep the last one)
			int batt_mv = batt_read_mv();

			if (batt_mv >= 0)
			{
				snprintf(bv, sizeof(bv), "%d.%dV", batt_mv / 1000, (batt_mv % 1000) / 100);

				if (batt_mv >= 7400)
					bv_col = GREEN;
				else if (batt_mv >= 7000)
					bv_col = ORANGE;
				else
					bv_col = RED;
			}
			else if (batt_mv == -1)
			{
				snprintf(bv, sizeof(bv), "?.?V");
				bv_col = RED;
			}

			if (strcmp(bv, bv_last))
			{
				strcpy(bv_last, bv);
				redraw_req = 1;
			}

			// time display
			t = time(NULL);
			localtime_r(&t, &time_info);
			snprintf(time_s, sizeof(time_s), "%02d:%02d", time_info.tm_hour, time_info.tm_min);
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
			DrawTextEx(customFont14, time_s, (Vector2){2, 2}, 14, 0, RAYWHITE);

			// GNSS
			if (gnss_display)
				DrawTexture(texture[IMG_GNSS], RES_X - 40, 1, WHITE);

			// battery voltage - icon or text (right-aligned)
			// DrawTexture(texture[IMG_BATT_100], RES_X - 24, 2, WHITE);
			Vector2 size = MeasureTextEx(customFont, bv, 14.0f, 0);
			DrawTextEx(customFont14, bv, (Vector2){RES_X - size.x - 2.0f, 2.0f}, 14.0f, 0, bv_col);

			if (disp_state == DISP_VFO)
			{
				// VFO A
				// DrawTexture(texture[IMG_VFO_ACT], 2, 23, WHITE);
				// DrawTextEx(customFont, "A", (Vector2){4.5f, 22.0f}, 16.0f, 0, WHITE);
				// DrawTextEx(customFont10, "VFO", (Vector2){3.0f, 38.0f}, 10.0f, 1, WHITE);

				float width = MeasureTextEx(customFont14, conf->channels.vfo_0.extra.mode, 14.0f, 1).x;
				DrawTextEx(customFont14, conf->channels.vfo_0.extra.mode, (Vector2){(RES_X / 4 - width / 2), 20.0f}, 14.0f, 1, BLUE);
				char can_line[8];
				sprintf(can_line, "CAN %d", conf->channels.vfo_0.extra.can);
				width = MeasureTextEx(customFont14, can_line, 14.0f, 1).x;
				DrawTextEx(customFont14, can_line, (Vector2){RES_X / 4 - width / 2, 34.0f}, 14.0f, 1, BLUE);

				width = MeasureTextEx(customFont14, conf->channels.vfo_0.extra.src, 14.0f, 1).x;
				DrawTextEx(customFont14, conf->channels.vfo_0.extra.src, (Vector2){(3 * RES_X / 4 - width / 2), 20.0f}, 14.0f, 1, BLUE);
				width = MeasureTextEx(customFont14, conf->channels.vfo_0.extra.dst, 14.0f, 1).x;
				DrawTextEx(customFont14, conf->channels.vfo_0.extra.dst, (Vector2){3 * RES_X / 4 - width / 2, 34.0f}, 14.0f, 1, BLUE);
				// DrawTexture(texture[IMG_MUTE], 140, 28, WHITE); //'vfo a mute' icon

				char freq_a_str_1[10];
				char freq_a_str_2[3];
				uint32_t freq_a = vfo_a_tx ? vfo_a_tx_f : vfo_a_rx_f;
				snprintf(freq_a_str_1, sizeof(freq_a_str_1), "%d.%03d", freq_a / 1000000, (freq_a % 1000000) / 1000);
				snprintf(freq_a_str_2, sizeof(freq_a_str_2), "%02d", (freq_a % 1000) / 10);
				width = MeasureTextEx(customFont38, freq_a_str_1, 38.0f, 0).x + 2 + MeasureTextEx(customFont, freq_a_str_2, 28.0f, 0).x;
				DrawTextEx(customFont38, freq_a_str_1, (Vector2){(RES_X - width) / 2.0f, 45.0f}, 38.0f, 0, vfo_a_tx ? RED : (Color){230, 230, 230, 255});
				DrawTextEx(customFont, freq_a_str_2, (Vector2){(RES_X - width) / 2.0f + MeasureTextEx(customFont38, freq_a_str_1, 38.0f, 0).x + 2, 53.0f}, 28.0f, 0, vfo_a_tx ? RED : (Color){230, 230, 230, 255});

				/*if (disp_vfo_b)
				{
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
				}*/

				if (disp_aux_data)
				{
					char line[64] = {0};
					Vector2 size;

					// if SRC and DST fields are valid - display them
					if (last_str.src[0] != 0 && last_str.dst[0] != 0)
					{
						sprintf(line, "%s", last_str.src);
						size = MeasureTextEx(customFont14, line, 14.0f, 0);
						DrawTextEx(customFont14, line, (Vector2){(RES_X / 4 - size.x / 2), 81.0f}, 14.0f, 0, MAGENTA);
						sprintf(line, "%s", last_str.dst);
						size = MeasureTextEx(customFont14, line, 14.0f, 0);
						DrawTextEx(customFont14, line, (Vector2){(RES_X / 4 - size.x / 2), 95.0f}, 14.0f, 0, MAGENTA);
					}

					// if ECD present, display it
					if (((last_str.type >> 5) & 3) == 2)
					{
						char ext_src[10] = "", ext_dst[10] = "";
						decode_callsign_bytes(ext_src, &last_str.meta[0]);
						decode_callsign_bytes(ext_dst, &last_str.meta[6]);

						sprintf(line, "%s", ext_src);
						size = MeasureTextEx(customFont14, line, 14.0f, 0);
						DrawTextEx(customFont14, line, (Vector2){3 * RES_X / 4 - size.x / 2, 81.0f}, 14.0f, 0, MAGENTA);
						sprintf(line, "%s", ext_dst);
						size = MeasureTextEx(customFont14, line, 14.0f, 0);
						DrawTextEx(customFont14, line, (Vector2){3 * RES_X / 4 - size.x / 2, 95.0f}, 14.0f, 0, MAGENTA);
					}
				}

				DrawRectangle(2, RES_Y - 2 - 2, 2 + knob_pos / 255.0f * (RES_X - 4 - 2), 2, GRAY);
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
	UnloadFont(customFont38);
	UnloadFont(customFont14);
	kbd_cleanup(kbd);
	if (chg_fd >= 0)
		close(chg_fd);
	sx1255_cleanup();

	linht_ctrl_atten_cleanup(); // Cleanup attenuator control
	linht_ctrl_pa_enable_set(false); // Disable PA
	linht_ctrl_tx_rx_switch_set(false); // RX
	
	if (fg_pid > 0)
	{
		kill(fg_pid, SIGTERM); // kill FG
		waitpid(fg_pid, NULL, 0);
	}

	// zmq_ctx_destroy() waits for every open socket, and for unsent
	// messages unless the linger time is 0
	int linger = 0;
	zmq_setsockopt(zmq_ptt_pub, ZMQ_LINGER, &linger, sizeof(linger));
	zmq_setsockopt(zmq_fg_pub, ZMQ_LINGER, &linger, sizeof(linger));
	zmq_setsockopt(zmq_sub, ZMQ_LINGER, &linger, sizeof(linger));
	zmq_unbind(zmq_ptt_pub, ptt_ipc);
	zmq_close(zmq_ptt_pub);
	zmq_close(zmq_fg_pub);
	zmq_close(zmq_sub);
	zmq_ctx_destroy(zmq_ctx);
	cyaml_free(&cfg, &config_schema, conf, 0);
	db_cleanup();
	CloseWindow();
	fprintf(stderr, "Cleanup done. Exiting.\n");

	return 0;
}
