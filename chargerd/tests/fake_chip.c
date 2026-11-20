// linht-chargerd tests: a fake BQ25792 behind struct bq_io

#include <errno.h>
#include <string.h>

#include "fake_chip.h"

void fake_set16(struct fake_chip *f, uint8_t reg, int value)
{
    f->r[reg] = (uint8_t)((value >> 8) & 0xff);
    f->r[reg + 1] = (uint8_t)(value & 0xff);
}

static void watchdog(struct fake_chip *f)
{
    if (!f->host || f->t - f->kick_t < 40.0)
        return;
    // Back to default mode: the bits marked "reset by WATCHDOG".
    f->host = 0;
    f->wd_expiries++;
    f->r[BQ_REG_CTRL0] |= BQ_EN_CHG;
    f->r[BQ_REG_CTRL5] &= (uint8_t)~BQ_EN_IBAT;
    f->r[BQ_REG_ADC] &= (uint8_t)~BQ_ADC_EN;
    f->r[BQ_REG_STAT0] |= BQ_WD_STAT;
}

static int rd(void *ctx, uint8_t reg, uint8_t *buf, unsigned len)
{
    struct fake_chip *f = ctx;
    if (f->fail)
        return -EIO;
    if (reg + len > BQ_NREGS)
        return -EINVAL;
    watchdog(f);
    // A running ADC gives new results: dither the VSYS LSB.
    if ((f->r[BQ_REG_ADC] & BQ_ADC_EN) && !f->adc_frozen)
        f->r[BQ_REG_VSYS + 1] = (uint8_t)(f->r[BQ_REG_VSYS + 1] ^ (++f->adc_tick & 1 ? 1 : 2));
    memcpy(buf, f->r + reg, len);
    for (unsigned i = reg; i < reg + len; i++)
        if (i >= BQ_REG_FLAG0 && i <= BQ_REG_FFLAG1)
            f->r[i] = 0;
    return 0;
}

static int wr(void *ctx, uint8_t reg, uint8_t val)
{
    struct fake_chip *f = ctx;
    if (f->fail)
        return -EIO;
    watchdog(f);
    f->writes[reg]++;
    if (!f->host)
    {
        f->host = 1;
        f->kick_t = f->t;
    }
    if (reg == BQ_REG_CTRL1 && (val & BQ_WD_RST))
    {
        f->kick_t = f->t;
        f->r[BQ_REG_STAT0] &= (uint8_t)~BQ_WD_STAT;
        val &= (uint8_t)~BQ_WD_RST;     // self clearing
    }
    if (reg == BQ_REG_CTRL0 && (val & BQ_EN_HIZ) && !(f->r[reg] & BQ_EN_HIZ))
        f->r[BQ_REG_ADC] &= (uint8_t)~BQ_ADC_EN;  // HIZ entry turns the ADC off
    // The 16 bit VREG takes only a two byte write (seen on Rev C).
    if (reg == BQ_REG_VREG || reg == BQ_REG_VREG + 1)
    {
        f->ignored_writes++;
        return 0;
    }
    f->r[reg] = val;
    return 0;
}

static int wr2(void *ctx, uint8_t reg, uint8_t hi, uint8_t lo)
{
    struct fake_chip *f = ctx;
    if (f->fail)
        return -EIO;
    if (reg + 2 > BQ_NREGS)
        return -EINVAL;
    watchdog(f);
    f->writes[reg]++;
    f->writes[reg + 1]++;
    f->r[reg] = hi;
    f->r[reg + 1] = lo;
    return 0;
}

void fake_init(struct fake_chip *f, struct bq_io *io)
{
    memset(f, 0, sizeof(*f));
    fake_set16(f, BQ_REG_VREG, 840);        // 8.40 V
    f->r[BQ_REG_CTRL_A] = 0x63;             // 2S
    f->r[BQ_REG_CTRL0] = 0xa2;
    f->r[BQ_REG_CTRL1] = 0x85;
    f->r[BQ_REG_CTRL4] = 0x01;              // 1.5 MHz
    f->r[BQ_REG_CTRL5] = 0x16;
    f->r[BQ_REG_ADC] = 0x30;
    f->r[BQ_REG_PART] = 0x09;               // PN 1, rev 1
    fake_usb(f, 1);
    f->r[BQ_REG_STAT1] = 3 << 5;            // fast charge
    fake_adc(f, 7.9, 0.5);
    memset(io, 0, sizeof(*io));
    io->read = rd;
    io->write = wr;
    io->write2 = wr2;
    io->ctx = f;
    io->fd = -1;
}

void fake_time(struct fake_chip *f, double t)
{
    f->t = t;
}

void fake_usb(struct fake_chip *f, int on)
{
    if (on)
    {
        f->r[BQ_REG_STAT0] |= BQ_VBUS_PRESENT | BQ_PG_STAT;
        f->r[BQ_REG_CTRL0] &= (uint8_t)~BQ_EN_HIZ;  // plug in clears EN_HIZ
        fake_set16(f, BQ_REG_VBUS, 5000);
    }
    else
    {
        f->r[BQ_REG_STAT0] &= (uint8_t)~(BQ_VBUS_PRESENT | BQ_PG_STAT);
        f->r[BQ_REG_STAT1] = 0;
        fake_set16(f, BQ_REG_VBUS, 0);
    }
}

void fake_adc(struct fake_chip *f, double vbat, double ibat)
{
    fake_set16(f, BQ_REG_VBAT, (int)(vbat * 1000 + 0.5));
    fake_set16(f, BQ_REG_IBAT, (int)(ibat * 1000 + (ibat < 0 ? -0.5 : 0.5)));
    fake_set16(f, BQ_REG_VSYS, (int)(vbat * 1000 + 100));
    fake_set16(f, BQ_REG_TDIE, 60);         // 30 C
}
