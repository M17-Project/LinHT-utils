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
    float i_dc, q_dc, iq_bal, iq_theta;
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
	CYAML_FIELD_FLOAT("iq_theta", CYAML_FLAG_DEFAULT, rf_settings_t, iq_theta),
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

#endif
