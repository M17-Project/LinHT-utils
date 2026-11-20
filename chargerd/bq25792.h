// linht-chargerd: TI BQ25792 register access (data sheet SLUSDG1B)
//
// This is the only file that knows the register map. The I2C transfers go
// through struct bq_io, so the unit tests can put a fake chip behind it.

#ifndef CHARGERD_BQ25792_H
#define CHARGERD_BQ25792_H

#include <stdint.h>

#define BQ_NREGS 0x49           // REG00 to REG48

#define BQ_REG_VREG     0x01    // 16 bit, 10 mV per bit, bits 10:0 (REG01 2:0, REG02)
#define BQ_REG_CTRL_A   0x0a    // CELL in bits 7:6
#define BQ_REG_CTRL0    0x0f    // EN_CHG, EN_HIZ
#define BQ_REG_CTRL1    0x10    // WD_RST, WATCHDOG
#define BQ_REG_CTRL3    0x12    // PFM_FWD_DIS
#define BQ_REG_CTRL4    0x13    // PWM_FREQ, never written
#define BQ_REG_CTRL5    0x14    // EN_IBAT
#define BQ_REG_STAT0    0x1b
#define BQ_REG_STAT1    0x1c
#define BQ_REG_FAULT0   0x20
#define BQ_REG_FAULT1   0x21
#define BQ_REG_FLAG0    0x22    // REG22 to REG27 clear on read
#define BQ_REG_FFLAG0   0x26
#define BQ_REG_FFLAG1   0x27
#define BQ_REG_ADC      0x2e
#define BQ_REG_IBUS     0x31    // first ADC result
#define BQ_REG_IBAT     0x33
#define BQ_REG_VBUS     0x35
#define BQ_REG_VBAT     0x3b
#define BQ_REG_VSYS     0x3d
#define BQ_REG_TDIE     0x41    // last ADC result used
#define BQ_REG_PART     0x48

#define BQ_EN_CHG       0x20    // REG0F
#define BQ_EN_HIZ       0x04    // REG0F
#define BQ_WD_RST       0x08    // REG10
#define BQ_PFM_FWD_DIS  0x10    // REG12
#define BQ_PWM_FREQ     0x20    // REG13, 1 = 750 kHz (forbidden on LinHT)
#define BQ_EN_IBAT      0x20    // REG14
#define BQ_ADC_EN       0x80    // REG2E
#define BQ_ADC_RATE     0x40    // REG2E, 1 = one shot
#define BQ_ADC_SAMPLE   0x30    // REG2E, 00 = 15 bit

#define BQ_IINDPM_STAT  0x80    // REG1B
#define BQ_VINDPM_STAT  0x40
#define BQ_WD_STAT      0x20
#define BQ_PG_STAT      0x08
#define BQ_VBUS_PRESENT 0x01

#define BQ_CHG_DONE     7       // CHG_STAT

#define BQ_VBAT_OVP     0x20    // REG20 VBAT_OVP_STAT, REG26 VBAT_OVP_FLAG

// Stock values after a clean stop (data sheet reset values, REG10 and
// REG14 as read on the LinHT units in default mode).
#define BQ_STOCK_REG0F  0xa2
#define BQ_STOCK_REG10  0x85
#define BQ_STOCK_REG12  0x00
#define BQ_STOCK_REG14  0x16
#define BQ_STOCK_REG2E  0x30

struct bq_io
{
    // Both return 0 or a negative errno.
    int (*read)(void *ctx, uint8_t reg, uint8_t *buf, unsigned len);
    int (*write)(void *ctx, uint8_t reg, uint8_t val);
    // Two bytes from reg on in one transfer. The 16 bit registers (VREG)
    // need it: the BQ25792 ignores a one byte write to REG02.
    int (*write2)(void *ctx, uint8_t reg, uint8_t hi, uint8_t lo);
    void *ctx;
    int fd;                     // real I2C only
    int addr;
};

struct bq_meas
{
    double vbus, ibus, vbat, ibat, vsys, tdie;  // V, A, degrees C
    double vreg;                                // V, charge voltage limit
    int vbus_present, power_good;
    int iindpm, vindpm, wd_expired;
    int chg_stat;                               // 0 to 7
    int cells;                                  // from CELL
    int part;                                   // PN, 1 = BQ25792
    int pwm_750k;
    uint8_t fault0, fault1;                     // REG20, REG21
    uint8_t fflag0, fflag1;                     // REG26, REG27
};

// What the policy wants. bq_enforce() writes only the bits that differ.
struct bq_want
{
    int hiz;
    int chg;
    int pfm;
    int vreg_10mv;              // VREG in 10 mV steps, 0 leaves it as it is
};

// Bits of the bq_enforce() result: which registers were rewritten.
#define BQ_FIX_REG0F 1
#define BQ_FIX_REG12 2
#define BQ_FIX_REG14 4
#define BQ_FIX_REG2E 8
#define BQ_FIX_VREG  16

// Opens the I2C device, takes flock(LOCK_EX | LOCK_NB) on it and sets the
// slave address without I2C_SLAVE_FORCE (EBUSY if a kernel driver is bound).
int bq_open(struct bq_io *io, const char *dev, int addr);
void bq_close(struct bq_io *io);

int bq_read_all(struct bq_io *io, uint8_t regs[BQ_NREGS]);
void bq_decode(const uint8_t regs[BQ_NREGS], struct bq_meas *m);

// Read, modify, write of one register. Writes only when the value changes
// unless force is set. Returns 0 or a negative errno.
int bq_update(struct bq_io *io, uint8_t reg, uint8_t mask, uint8_t val, int force);

// Compares the snapshot with what is wanted and rewrites REG0F, REG12,
// REG14, REG2E and VREG (REG01, REG02) where they differ. The snapshot is updated with the
// written values. Returns the BQ_FIX_* bits or a negative errno.
int bq_enforce(struct bq_io *io, uint8_t regs[BQ_NREGS], const struct bq_want *w);

// WD_RST, the 40 s setting in REG10 stays as it is.
int bq_kick(struct bq_io *io);

// Writes stock values (not values saved at start), then reads back REG0F,
// REG10, REG12, REG14 and REG2E into rb[5].
int bq_restore(struct bq_io *io, uint8_t rb[5]);

// The part of bq_restore() after REG0F: PFM_FWD_DIS 0, EN_IBAT 0, REG2E
// 0x30 (ADC off, 12 bit) and WD_RST. REG0F (EN_HIZ, EN_CHG) is not touched.
// A daemon restart is this followed by the start-up writes, the chip
// recovery does the same. Returns 0 or a negative errno.
int bq_restore_keep0f(struct bq_io *io);

const char *bq_chg_stat_name(int stat);

// Names of the set fault bits of REG20 and REG21 (or the same bits of the
// flags REG26, REG27). IBAT_REG is a regulation state, not a fault, and is
// left out. Returns the number of names written to out.
int bq_fault_names(uint8_t f0, uint8_t f1, const char **out, int max);

#endif
