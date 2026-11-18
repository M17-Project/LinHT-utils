// linht-chargerd: TI BQ25792 register access (data sheet SLUSDG1B)

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "bq25792.h"

// --- Real I2C through /dev/i2c-N ---

static int i2c_read(void *ctx, uint8_t reg, uint8_t *buf, unsigned len)
{
    struct bq_io *io = ctx;
    struct i2c_msg msg[2] = {
        { .addr = (uint16_t)io->addr, .flags = 0, .len = 1, .buf = &reg },
        { .addr = (uint16_t)io->addr, .flags = I2C_M_RD, .len = (uint16_t)len, .buf = buf },
    };
    struct i2c_rdwr_ioctl_data x = { .msgs = msg, .nmsgs = 2 };

    // One transfer with a repeated start, so a multi byte read is atomic.
    return ioctl(io->fd, I2C_RDWR, &x) == 2 ? 0 : -errno;
}

static int i2c_write(void *ctx, uint8_t reg, uint8_t val)
{
    struct bq_io *io = ctx;
    uint8_t b[2] = { reg, val };
    struct i2c_msg msg = { .addr = (uint16_t)io->addr, .flags = 0, .len = 2, .buf = b };
    struct i2c_rdwr_ioctl_data x = { .msgs = &msg, .nmsgs = 1 };

    return ioctl(io->fd, I2C_RDWR, &x) == 1 ? 0 : -errno;
}

int bq_open(struct bq_io *io, const char *dev, int addr)
{
    memset(io, 0, sizeof(*io));
    io->fd = open(dev, O_RDWR | O_CLOEXEC);
    if (io->fd < 0)
        return -errno;

    int rc = 0;
    if (flock(io->fd, LOCK_EX | LOCK_NB) < 0)
        rc = -errno;
    // I2C_SLAVE (not FORCE) fails with EBUSY when a kernel driver owns 0x6B.
    else if (ioctl(io->fd, I2C_SLAVE, addr) < 0)
        rc = -errno;
    if (rc < 0)
    {
        close(io->fd);
        io->fd = -1;
        return rc;
    }
    io->addr = addr;
    io->read = i2c_read;
    io->write = i2c_write;
    io->ctx = io;
    return 0;
}

void bq_close(struct bq_io *io)
{
    if (io->fd >= 0)
        close(io->fd);
    io->fd = -1;
}

// --- Register logic ---

int bq_read_all(struct bq_io *io, uint8_t regs[BQ_NREGS])
{
    return io->read(io->ctx, 0x00, regs, BQ_NREGS);
}

static int s16(const uint8_t *r, int reg)
{
    return (int16_t)((r[reg] << 8) | r[reg + 1]);
}

void bq_decode(const uint8_t r[BQ_NREGS], struct bq_meas *m)
{
    m->ibus = s16(r, BQ_REG_IBUS) / 1000.0;
    m->ibat = s16(r, BQ_REG_IBAT) / 1000.0;
    m->vbus = s16(r, BQ_REG_VBUS) / 1000.0;
    m->vbat = s16(r, BQ_REG_VBAT) / 1000.0;
    m->vsys = s16(r, BQ_REG_VSYS) / 1000.0;
    m->tdie = s16(r, BQ_REG_TDIE) * 0.5;
    m->vreg = (((r[BQ_REG_VREG] & 0x07) << 8) | r[BQ_REG_VREG + 1]) * 0.01;

    uint8_t s0 = r[BQ_REG_STAT0];
    m->vbus_present = !!(s0 & BQ_VBUS_PRESENT);
    m->power_good = !!(s0 & BQ_PG_STAT);
    m->iindpm = !!(s0 & BQ_IINDPM_STAT);
    m->vindpm = !!(s0 & BQ_VINDPM_STAT);
    m->wd_expired = !!(s0 & BQ_WD_STAT);
    m->chg_stat = r[BQ_REG_STAT1] >> 5;
    m->cells = (r[BQ_REG_CTRL_A] >> 6) + 1;
    m->part = (r[BQ_REG_PART] >> 3) & 7;
    m->pwm_750k = !!(r[BQ_REG_CTRL4] & BQ_PWM_FREQ);
    m->fault0 = r[BQ_REG_FAULT0];
    m->fault1 = r[BQ_REG_FAULT1];
    m->fflag0 = r[BQ_REG_FFLAG0];
    m->fflag1 = r[BQ_REG_FFLAG1];
}

int bq_update(struct bq_io *io, uint8_t reg, uint8_t mask, uint8_t val, int force)
{
    uint8_t cur;
    int rc = io->read(io->ctx, reg, &cur, 1);
    if (rc < 0)
        return rc;
    uint8_t next = (uint8_t)((cur & ~mask) | (val & mask));
    if (next == cur && !force)
        return 0;
    return io->write(io->ctx, reg, next);
}

static int fix(struct bq_io *io, uint8_t *regs, uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t next = (uint8_t)((regs[reg] & ~mask) | (val & mask));
    if (next == regs[reg])
        return 0;
    int rc = io->write(io->ctx, reg, next);
    if (rc < 0)
        return rc;
    regs[reg] = next;
    return 1;
}

int bq_enforce(struct bq_io *io, uint8_t regs[BQ_NREGS], const struct bq_want *w)
{
    int done = 0, rc;

    rc = fix(io, regs, BQ_REG_CTRL0, BQ_EN_HIZ | BQ_EN_CHG,
             (w->hiz ? BQ_EN_HIZ : 0) | (w->chg ? BQ_EN_CHG : 0));
    if (rc < 0)
        return rc;
    done |= rc ? BQ_FIX_REG0F : 0;

    rc = fix(io, regs, BQ_REG_CTRL3, BQ_PFM_FWD_DIS, w->pfm ? 0 : BQ_PFM_FWD_DIS);
    if (rc < 0)
        return rc;
    done |= rc ? BQ_FIX_REG12 : 0;

    rc = fix(io, regs, BQ_REG_CTRL5, BQ_EN_IBAT, BQ_EN_IBAT);
    if (rc < 0)
        return rc;
    done |= rc ? BQ_FIX_REG14 : 0;

    // Continuous conversion, 15 bit. The averaging bits stay as they are.
    rc = fix(io, regs, BQ_REG_ADC, BQ_ADC_EN | BQ_ADC_RATE | BQ_ADC_SAMPLE, BQ_ADC_EN);
    if (rc < 0)
        return rc;
    done |= rc ? BQ_FIX_REG2E : 0;

    return done;
}

int bq_kick(struct bq_io *io)
{
    return bq_update(io, BQ_REG_CTRL1, BQ_WD_RST, BQ_WD_RST, 1);
}

int bq_restore(struct bq_io *io, uint8_t rb[5])
{
    static const uint8_t reg[5] = { BQ_REG_CTRL0, BQ_REG_CTRL1, BQ_REG_CTRL3, BQ_REG_CTRL5, BQ_REG_ADC };
    int rc, err = 0;

    // Each step is tried even when an earlier one failed.
    if ((rc = bq_update(io, BQ_REG_CTRL0, BQ_EN_HIZ | BQ_EN_CHG, BQ_EN_CHG, 0)) < 0)
        err = rc;
    if ((rc = bq_update(io, BQ_REG_CTRL3, BQ_PFM_FWD_DIS, 0, 0)) < 0)
        err = rc;
    if ((rc = bq_update(io, BQ_REG_CTRL5, BQ_EN_IBAT, 0, 0)) < 0)
        err = rc;
    if ((rc = bq_update(io, BQ_REG_ADC, 0xff, BQ_STOCK_REG2E, 0)) < 0)
        err = rc;
    if ((rc = bq_kick(io)) < 0)
        err = rc;
    for (int i = 0; i < 5; i++)
        if ((rc = io->read(io->ctx, reg[i], &rb[i], 1)) < 0)
            err = rc;
    return err;
}

const char *bq_chg_stat_name(int stat)
{
    static const char *const n[8] = { "not-charging", "trickle", "precharge", "fast",
                                      "taper", "reserved", "top-off", "done" };
    return n[stat & 7];
}

int bq_fault_names(uint8_t f0, uint8_t f1, const char **out, int max)
{
    static const char *const n0[8] = { "vac1-ovp", "vac2-ovp", "converter-ocp", "ibat-ocp",
                                       "ibus-ocp", "vbat-ovp", "vbus-ovp", NULL };
    static const char *const n1[8] = { NULL, NULL, "thermal-shutdown", NULL,
                                       "otg-uvp", "otg-ovp", "vsys-ovp", "vsys-short" };
    int k = 0;
    for (int i = 0; i < 8; i++)
    {
        if ((f0 & (1 << i)) && n0[i] && k < max)
            out[k++] = n0[i];
        if ((f1 & (1 << i)) && n1[i] && k < max)
            out[k++] = n1[i];
    }
    return k;
}
