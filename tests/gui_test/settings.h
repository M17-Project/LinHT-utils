#ifndef LINHT_SETTINGS
#define LINHT_SETTINGS

typedef struct
{
    bool backlight;
    uint8_t brightness;
    uint8_t timeout;
} ui_t;

typedef struct
{
    float freq_corr;
    bool calibrated;
    float i_dc, q_dc, iq_bal, iq_crosstalk;
    char *dpd_type;
    float dpd_0, dpd_1, dpd_2;
    bool bias_t;
} rf_settings_t;

typedef struct
{
    float timezone;
    ui_t keyboard;
    ui_t display;
    rf_settings_t rf;
} hw_settings_t;

typedef struct
{
    float rf_power_out_sp;
    uint8_t rf_switch;
    float atten_0, atten_1, lna_gain, pga_gain, dac_gain, mix_gain;
    bool tx_enabled, rx_enabled;
} frontend_t;

typedef struct
{
    char *mode;
    char *submode;
    float squelch_level;
    float ctcss_tone;
    bool ctcss_tx;
    bool ctcss_rx;
    char *src;
    char *dst;
    uint8_t can;
    bool encrypted;
    char *type;
    char *encr_key;
    bool signed_flag;
    char *sign_key;
    char *meta;
} channel_extra_t;

typedef struct
{
    bool active;
    char *fg;
    uint32_t tx_freq;
    uint32_t rx_freq;
    float bw;
    channel_extra_t extra;
} channel_t;

typedef struct
{
    frontend_t frontend;
    hw_settings_t settings;
    struct
    {
        channel_t vfo_0;
        channel_t vfo_1;
    } channels;
} config_t;

#endif
