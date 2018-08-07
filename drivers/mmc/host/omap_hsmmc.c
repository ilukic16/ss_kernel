/*
 * drivers/mmc/host/omap_hsmmc.c
 *
 * Driver for OMAP2430/3430 MMC controller.
 *
 * Copyright (C) 2007 Texas Instruments.
 *
 * Authors:
 *	Syed Mohammed Khasim	<x0khasim@ti.com>
 *	Madhusudhan		<madhu.cr@ti.com>
 *	Mohit Jalori		<mjalori@ti.com>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/dmaengine.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/omap-dma.h>
#include <linux/mmc/host.h>
#include <linux/mmc/core.h>
#include <linux/mmc/mmc.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/platform_data/mmc-omap.h>

/* OMAP HSMMC Host Controller Registers */
#define OMAP_HSMMC_SYSSTATUS	0x0014
#define OMAP_HSMMC_CON		0x002C
#define OMAP_HSMMC_SDMASA	0x0100
#define OMAP_HSMMC_BLK		0x0104
#define OMAP_HSMMC_ARG		0x0108
#define OMAP_HSMMC_CMD		0x010C
#define OMAP_HSMMC_RSP10	0x0110
#define OMAP_HSMMC_RSP32	0x0114
#define OMAP_HSMMC_RSP54	0x0118
#define OMAP_HSMMC_RSP76	0x011C
#define OMAP_HSMMC_DATA		0x0120
#define OMAP_HSMMC_PSTATE	0x0124
#define OMAP_HSMMC_HCTL		0x0128
#define OMAP_HSMMC_SYSCTL	0x012C
#define OMAP_HSMMC_STAT		0x0130
#define OMAP_HSMMC_IE		0x0134
#define OMAP_HSMMC_ISE		0x0138
#define OMAP_HSMMC_AC12		0x013C
#define OMAP_HSMMC_CAPA		0x0140
#define OMAP_HSMMC_REV		0x01FC

#define VS18			(1 << 26)
#define VS30			(1 << 25)
#define HSS			(1 << 21)
#define SDVS18			(0x5 << 9)
#define SDVS30			(0x6 << 9)
#define SDVS33			(0x7 << 9)
#define SDVS_MASK		0x00000E00
#define SDVSCLR			0xFFFFF1FF
#define SDVSDET			0x00000400
#define AUTOIDLE		0x1
#define SDBP			(1 << 8)
#define DTO			0xe
#define ICE			0x1
#define ICS			0x2
#define CEN			(1 << 2)
#define CLKD_MAX		0x3FF		/* max clock divisor: 1023 */
#define CLKD_MASK		0x0000FFC0
#define CLKD_SHIFT		6
#define DTO_MASK		0x000F0000
#define DTO_SHIFT		16
#define INIT_STREAM		(1 << 1)
#define ACEN_ACMD12		(1 << 2)
#define ACEN_ACMD23		(2 << 2)
#define DP_SELECT		(1 << 21)
#define DDIR			(1 << 4)
#define DMAE			0x1
#define MSBS			(1 << 5)
#define BCE			(1 << 1)
#define FOUR_BIT		(1 << 1)
#define HSPE			(1 << 2)
#define DDR			(1 << 19)
#define DW8			(1 << 5)
#define CLKEXTFREE		(1 << 16)
#define PADEN			(1 << 15)
#define CLEV			(1 << 24)
#define DLEV			(0xF << 20)
#define OD			0x1
#define STAT_CLEAR		0xFFFFFFFF
#define INIT_STREAM_CMD		0x00000000
#define DUAL_VOLT_OCR_BIT	7
#define SRC			(1 << 25)
#define SRD			(1 << 26)
#define SOFTRESET		(1 << 1)
#define RESETDONE		(1 << 0)

/* Interrupt masks for IE and ISE register */
#define CC_EN			(1 << 0)
#define TC_EN			(1 << 1)
#define BWR_EN			(1 << 4)
#define BRR_EN			(1 << 5)
#define ERR_EN			(1 << 15)
#define CTO_EN			(1 << 16)
#define CCRC_EN			(1 << 17)
#define CEB_EN			(1 << 18)
#define CIE_EN			(1 << 19)
#define DTO_EN			(1 << 20)
#define DCRC_EN			(1 << 21)
#define DEB_EN			(1 << 22)
#define ACE_EN			(1 << 24)
#define CERR_EN			(1 << 28)
#define BADA_EN			(1 << 29)

#define INT_EN_MASK		(BADA_EN | CERR_EN | DEB_EN | DCRC_EN |\
		DTO_EN | CIE_EN | CEB_EN | CCRC_EN | CTO_EN | \
		BRR_EN | BWR_EN | TC_EN | CC_EN)

#define CNI			(1 << 7)
#define ACIE			(1 << 4)
#define ACEB			(1 << 3)
#define ACCE			(1 << 2)
#define ACTO			(1 << 1)
#define ACNE			(1 << 0)

#define SIGEN_V1V8	(1 << 19)

#define MMC_AUTOSUSPEND_DELAY	100
#define MMC_TIMEOUT_MS		20
#define MMC_TIMEOUT_US		20000
#define OMAP_MMC_MIN_CLOCK	400000
#define OMAP_MMC_MAX_CLOCK	52000000
#define DRIVER_NAME		"omap_hsmmc"

#define AUTO_CMD12		(1 << 0)	/* Auto CMD12 support */
#define AUTO_CMD23		(1 << 1)	/* Auto CMD23 support */

#define OMAP_HSMMC_REV_SHIFT	24
/* HSMMC controller revision on OMAP5, DRA7 */
#define OMAP_HSMMC_REV_33	0x33

#define VDD_1V8			1800000		/* 180000 uV */
#define VDD_3V0			3000000		/* 300000 uV */
#define VDD_165_195		(ffs(MMC_VDD_165_195) - 1)

/*
 * One controller can have multiple slots, like on some omap boards using
 * omap.c controller driver. Luckily this is not currently done on any known
 * omap_hsmmc.c device.
 */
#define mmc_slot(host)		(host->pdata->slots[host->slot_id])

/*
 * MMC Host controller read/write API's
 */
#define OMAP_HSMMC_READ(base, reg)	\
	__raw_readl((base) + OMAP_HSMMC_##reg)

#define OMAP_HSMMC_WRITE(base, reg, val) \
	__raw_writel((val), (base) + OMAP_HSMMC_##reg)

struct omap_hsmmc_next {
	unsigned int	dma_len;
	s32		cookie;
};

struct omap_hsmmc_host {
	struct	device		*dev;
	struct	mmc_host	*mmc;
	struct	mmc_request	*mrq;
	struct	mmc_command	*cmd;
	struct	mmc_data	*data;
	struct	clk		*fclk;
	struct	clk		*dbclk;
	/*
	 * vcc == configured supply
	 * vcc_aux == optional
	 *   -	MMC1, supply for DAT4..DAT7
	 *   -	MMC2/MMC2, external level shifter voltage supply, for
	 *	chip (SDIO, eMMC, etc) or transceiver (MMC2 only)
	 */
	struct	regulator	*vcc;
	struct	regulator	*vcc_aux;
	struct	regulator	*pbias;
	bool			pbias_enabled;
	void	__iomem		*base;
	resource_size_t		mapbase;
	spinlock_t		irq_lock; /* Prevent races with irq handler */
	unsigned int		dma_len;
	unsigned int		dma_sg_idx;
	unsigned char		bus_mode;
	unsigned char		power_mode;
	int			suspended;
	u32			con;
	u32			hctl;
	u32			sysctl;
	u32			capa;
	int			irq;
	int			use_dma, dma_ch;
	struct dma_chan		*tx_chan;
	struct dma_chan		*rx_chan;
	int			slot_id;
	int			response_busy;
	int			context_loss;
	int			protect_card;
	int			reqs_blocked;
	int			req_in_progress;
	int			regulator_enabled;
	unsigned long		clk_rate;
	unsigned int		flags;
	struct omap_hsmmc_next	next_data;
	struct	omap_mmc_platform_data	*pdata;
	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pinctrl_muxpu, *pinctrl_muxpd;
	int needs_vmmc:1;
	int needs_vmmc_aux:1;
};

static int
omap_hsmmc_prepare_data(struct omap_hsmmc_host *host, struct mmc_request *req);

static void set_data_timeout(struct omap_hsmmc_host *host,
			     unsigned int timeout_ns,
			     unsigned int timeout_clks);

static int omap_hsmmc_card_detect(struct device *dev, int slot)
{
	struct omap_hsmmc_host *host = dev_get_drvdata(dev);
	struct omap_mmc_platform_data *mmc = host->pdata;

	/* NOTE: assumes card detect signal is active-low */
	return !gpio_get_value_cansleep(mmc->slots[0].switch_pin);
}

static int omap_hsmmc_get_wp(struct device *dev, int slot)
{
	struct omap_hsmmc_host *host = dev_get_drvdata(dev);
	struct omap_mmc_platform_data *mmc = host->pdata;

	/* NOTE: assumes write protect signal is active-high */
	return gpio_get_value_cansleep(mmc->slots[0].gpio_wp);
}

static int omap_hsmmc_get_cover_state(struct device *dev, int slot)
{
	struct omap_hsmmc_host *host = dev_get_drvdata(dev);
	struct omap_mmc_platform_data *mmc = host->pdata;

	/* NOTE: assumes card detect signal is active-low */
	return !gpio_get_value_cansleep(mmc->slots[0].switch_pin);
}

#ifdef CONFIG_PM

static int omap_hsmmc_suspend_cdirq(struct device *dev, int slot)
{
	struct omap_hsmmc_host *host = dev_get_drvdata(dev);
	struct omap_mmc_platform_data *mmc = host->pdata;

	disable_irq(mmc->slots[0].card_detect_irq);
	return 0;
}

static int omap_hsmmc_resume_cdirq(struct device *dev, int slot)
{
	struct omap_hsmmc_host *host = dev_get_drvdata(dev);
	struct omap_mmc_platform_data *mmc = host->pdata;

	enable_irq(mmc->slots[0].card_detect_irq);
	return 0;
}

#else

#define omap_hsmmc_suspend_cdirq	NULL
#define omap_hsmmc_resume_cdirq		NULL

#endif

#ifdef CONFIG_REGULATOR
static int omap_hsmmc_set_power(struct device *dev, int slot, int power_on,
				   int vdd_iopower)
{
	struct omap_hsmmc_host *host =
		platform_get_drvdata(to_platform_device(dev));
	struct mmc_ios *ios = &host->mmc->ios;
	int ret = 0;
	u32 ac12 = 0;

	/*
	 * If we don't see a Vcc regulator, assume it's a fixed
	 * voltage always-on regulator.
	 */
	if (!host->vcc)
		return 0;

	if (mmc_slot(host).before_set_reg)
		mmc_slot(host).before_set_reg(dev, slot, power_on, vdd_iopower);

	if (host->pbias) {
		if (host->pbias_enabled == 1) {
			ret = regulator_disable(host->pbias);
			if (!ret)
				host->pbias_enabled = 0;
		}
		regulator_set_voltage(host->pbias, VDD_3V0, VDD_3V0);
	}

	/*
	 * Assume Vcc regulator is used only to power the card ... OMAP
	 * VDDS is used to power the pins, optionally with a transceiver to
	 * support cards using voltages other than VDDS (1.8V nominal).  When a
	 * transceiver is used, DAT3..7 are muxed as transceiver control pins.
	 *
	 * In some cases this regulator won't support enable/disable;
	 * e.g. it's a fixed rail for a WLAN chip.
	 *
	 * In other cases vcc_aux switches interface power.  Example, for
	 * eMMC cards it represents VccQ.  Sometimes transceivers or SDIO
	 * chips/cards need an interface voltage rail too.
	 */
	if (power_on) {
		if (host->vcc)
			ret = mmc_regulator_set_ocr(host->mmc, host->vcc,
								ios->vdd);
		if (ret < 0)
			return ret;
		/* Enable interface voltage rail, if needed */
		if (host->vcc_aux) {
			ac12 = OMAP_HSMMC_READ(host->base, AC12);
			if (vdd_iopower == VDD_165_195) {
				if (host->regulator_enabled) {
					ret = regulator_disable(host->vcc_aux);
					if (ret < 0)
						goto error_set_power;
					host->regulator_enabled = 0;
				}
				ret = regulator_set_voltage(host->vcc_aux,
				VDD_1V8, VDD_1V8);
				if (ret < 0)
					goto error_set_power;
				ac12 |= SIGEN_V1V8;
			} else {
				ret = regulator_set_voltage(host->vcc_aux,
							VDD_3V0, VDD_3V0);
				if (ret < 0)
					goto error_set_power;
				ac12 &= ~SIGEN_V1V8;
			}
			OMAP_HSMMC_WRITE(host->base, AC12, ac12);
		}
		if (host->vcc_aux && !host->regulator_enabled) {
			ret = regulator_enable(host->vcc_aux);
			if (!ret) {
				host->regulator_enabled = 1;
			} else {
				mmc_regulator_set_ocr(host->mmc, host->vcc, 0);
				goto error_set_power;
			}
		}
	} else {
		if (host->vcc_aux && host->regulator_enabled) {
			ret = regulator_set_voltage(host->vcc_aux, VDD_1V8,
								VDD_1V8);
			if (!ret)
				ac12 |= SIGEN_V1V8;
			ret = regulator_disable(host->vcc_aux);
			if (!ret)
				host->regulator_enabled = 0;
		}
		if (host->vcc)
			ret = mmc_regulator_set_ocr(host->mmc, host->vcc, 0);
	}

	if (ret < 0)
		goto error_set_power;

	if (host->pbias) {
		if (vdd_iopower == VDD_165_195)
			ret = regulator_set_voltage(host->pbias, VDD_1V8,
								VDD_1V8);
		else
			ret = regulator_set_voltage(host->pbias, VDD_3V0,
								VDD_3V0);
		if (ret < 0)
			goto error_set_power;

		if (host->pbias_enabled == 0) {
			ret = regulator_enable(host->pbias);
			if (!ret) {
				host->pbias_enabled = 1;
				goto error_set_power;
			}
		}
	}

	if (mmc_slot(host).after_set_reg)
		mmc_slot(host).after_set_reg(dev, slot, power_on, vdd_iopower);

error_set_power:
	return ret;
}

static int omap_hsmmc_reg_get(struct omap_hsmmc_host *host)
{
	struct regulator *reg;
	int ocr_value = 0;

	reg = regulator_get(host->dev, "vmmc");
	if (IS_ERR(reg) && host->needs_vmmc) {
		dev_err(host->dev, "unable to get vmmc regulator %ld\n",
			PTR_ERR(reg));
		return PTR_ERR(reg);
	}
	if (!IS_ERR(reg)) {
		host->vcc = reg;
		ocr_value = mmc_regulator_get_ocrmask(reg);
		if (!mmc_slot(host).ocr_mask) {
			mmc_slot(host).ocr_mask = ocr_value;
		} else {
			if (!(mmc_slot(host).ocr_mask & ocr_value)) {
				dev_err(host->dev, "ocrmask %x is not supported\n",
					mmc_slot(host).ocr_mask);
				mmc_slot(host).ocr_mask = 0;
				return -EINVAL;
			}
		}
	}
	mmc_slot(host).set_power = omap_hsmmc_set_power;

	/* Allow an aux regulator */
	reg = regulator_get(host->dev, "vmmc_aux");
	host->vcc_aux = IS_ERR(reg) ? NULL : reg;
	if (IS_ERR(reg) && host->needs_vmmc_aux) {
		dev_err(host->dev, "unable to get vmmc_aux regulator %ld\n",
			PTR_ERR(reg));
		return PTR_ERR(reg);
	}

	reg = regulator_get(host->dev, "pbias");
	host->pbias = IS_ERR(reg) ? NULL : reg;

	/* For eMMC do not power off when not in sleep state */
	if (mmc_slot(host).no_regulator_off_init)
		return 0;
	/*
	 * To disable boot_on regulator, enable regulator
	 * to increase usecount and then disable it.
	 */
	if ((host->vcc && regulator_is_enabled(host->vcc) > 0) ||
	    (host->vcc_aux && regulator_is_enabled(host->vcc_aux))) {
		int vdd = ffs(mmc_slot(host).ocr_mask) - 1;

		mmc_slot(host).set_power(host->dev, host->slot_id, 1, vdd);
		mmc_slot(host).set_power(host->dev, host->slot_id, 0, 0);
	}

	return 0;
}

static void omap_hsmmc_reg_put(struct omap_hsmmc_host *host)
{
	regulator_put(host->vcc);
	regulator_put(host->vcc_aux);
	regulator_put(host->pbias);
	mmc_slot(host).set_power = NULL;
}

static inline int omap_hsmmc_have_reg(void)
{
	return 1;
}

#else

static inline int omap_hsmmc_reg_get(struct omap_hsmmc_host *host)
{
	return -EINVAL;
}

static inline void omap_hsmmc_reg_put(struct omap_hsmmc_host *host)
{
}

static inline int omap_hsmmc_have_reg(void)
{
	return 0;
}

#endif

static int omap_hsmmc_gpio_init(struct omap_mmc_platform_data *pdata)
{
	int ret;

	if (gpio_is_valid(pdata->slots[0].switch_pin)) {
		if (pdata->slots[0].cover)
			pdata->slots[0].get_cover_state =
					omap_hsmmc_get_cover_state;
		else
			pdata->slots[0].card_detect = omap_hsmmc_card_detect;
		pdata->slots[0].card_detect_irq =
				gpio_to_irq(pdata->slots[0].switch_pin);
		ret = gpio_request(pdata->slots[0].switch_pin, "mmc_cd");
		if (ret)
			return ret;
		ret = gpio_direction_input(pdata->slots[0].switch_pin);
		if (ret)
			goto err_free_sp;
		ret = gpio_set_debounce(pdata->slots[0].switch_pin, 50000);
		if (ret)
			goto err_free_sp;
	} else
		pdata->slots[0].switch_pin = -EINVAL;

	if (gpio_is_valid(pdata->slots[0].gpio_wp)) {
		pdata->slots[0].get_ro = omap_hsmmc_get_wp;
		ret = gpio_request(pdata->slots[0].gpio_wp, "mmc_wp");
		if (ret)
			goto err_free_cd;
		ret = gpio_direction_input(pdata->slots[0].gpio_wp);
		if (ret)
			goto err_free_wp;
	} else
		pdata->slots[0].gpio_wp = -EINVAL;

	return 0;

err_free_wp:
	gpio_free(pdata->slots[0].gpio_wp);
err_free_cd:
	if (gpio_is_valid(pdata->slots[0].switch_pin))
err_free_sp:
		gpio_free(pdata->slots[0].switch_pin);
	return ret;
}

static void omap_hsmmc_gpio_free(struct omap_mmc_platform_data *pdata)
{
	if (gpio_is_valid(pdata->slots[0].gpio_wp))
		gpio_free(pdata->slots[0].gpio_wp);
	if (gpio_is_valid(pdata->slots[0].switch_pin))
		gpio_free(pdata->slots[0].switch_pin);
}

/*
 * Start clock to the card
 */
static void omap_hsmmc_start_clock(struct omap_hsmmc_host *host)
{
	OMAP_HSMMC_WRITE(host->base, SYSCTL,
		OMAP_HSMMC_READ(host->base, SYSCTL) | CEN);
}

/*
 * Stop clock to the card
 */
static void omap_hsmmc_stop_clock(struct omap_hsmmc_host *host)
{
	OMAP_HSMMC_WRITE(host->base, SYSCTL,
		OMAP_HSMMC_READ(host->base, SYSCTL) & ~CEN);
	if ((OMAP_HSMMC_READ(host->base, SYSCTL) & CEN) != 0x0)
		dev_dbg(mmc_dev(host->mmc), "MMC Clock is not stopped\n");
}

static void omap_hsmmc_enable_irq(struct omap_hsmmc_host *host,
				  struct mmc_command *cmd)
{
	unsigned int irq_mask;

	if (host->use_dma)
		irq_mask = INT_EN_MASK & ~(BRR_EN | BWR_EN);
	else
		irq_mask = INT_EN_MASK;

	/* Disable timeout for erases */
	if (cmd->opcode == MMC_ERASE)
		irq_mask &= ~DTO_EN;

	OMAP_HSMMC_WRITE(host->base, STAT, STAT_CLEAR);
	OMAP_HSMMC_WRITE(host->base, ISE, irq_mask);
	OMAP_HSMMC_WRITE(host->base, IE, irq_mask);
}

static void omap_hsmmc_disable_irq(struct omap_hsmmc_host *host)
{
	OMAP_HSMMC_WRITE(host->base, ISE, 0);
	OMAP_HSMMC_WRITE(host->base, IE, 0);
	OMAP_HSMMC_WRITE(host->base, STAT, STAT_CLEAR);
}

/* Calculate divisor for the given clock frequency */
static u16 calc_divisor(struct omap_hsmmc_host *host, struct mmc_ios *ios)
{
	u16 dsor = 0;

	if (ios->clock) {
		dsor = DIV_ROUND_UP(clk_get_rate(host->fclk), ios->clock);
		if (dsor > CLKD_MAX)
			dsor = CLKD_MAX;
	}

	return dsor;
}

static void omap_hsmmc_set_clock(struct omap_hsmmc_host *host)
{
	struct mmc_ios *ios = &host->mmc->ios;
	unsigned long regval;
	unsigned long timeout;
	unsigned long clkdiv;

	dev_vdbg(mmc_dev(host->mmc), "Set clock to %uHz\n", ios->clock);

	omap_hsmmc_stop_clock(host);

	regval = OMAP_HSMMC_READ(host->base, SYSCTL);
	regval = regval & ~(CLKD_MASK | DTO_MASK);
	clkdiv = calc_divisor(host, ios);
	regval = regval | (clkdiv << 6) | (DTO << 16);
	OMAP_HSMMC_WRITE(host->base, SYSCTL, regval);
	OMAP_HSMMC_WRITE(host->base, SYSCTL,
		OMAP_HSMMC_READ(host->base, SYSCTL) | ICE);

	/* Wait till the ICS bit is set */
	timeout = jiffies + msecs_to_jiffies(MMC_TIMEOUT_MS);
	while ((OMAP_HSMMC_READ(host->base, SYSCTL) & ICS) != ICS
		&& time_before(jiffies, timeout))
		cpu_relax();

	/*
	 * Enable High-Speed Support
	 * Pre-Requisites
	 *	- Controller should support High-Speed-Enable Bit
	 *	- Controller should not be using DDR Mode
	 *	- Controller should advertise that it supports High Speed
	 *	  in capabilities register
	 *	- MMC/SD clock coming out of controller > 25MHz
	 */
	if ((mmc_slot(host).features & HSMMC_HAS_HSPE_SUPPORT) &&
	    (ios->timing != MMC_TIMING_UHS_DDR50) &&
	    ((OMAP_HSMMC_READ(host->base, CAPA) & HSS) == HSS)) {
		regval = OMAP_HSMMC_READ(host->base, HCTL);
		if (clkdiv && (clk_get_rate(host->fclk)/clkdiv) > 25000000)
			regval |= HSPE;
		else
			regval &= ~HSPE;

		OMAP_HSMMC_WRITE(host->base, HCTL, regval);
	}

	omap_hsmmc_start_clock(host);
}

static void omap_hsmmc_set_bus_width(struct omap_hsmmc_host *host)
{
	struct mmc_ios *ios = &host->mmc->ios;
	u32 con;

	con = OMAP_HSMMC_READ(host->base, CON);
	if (ios->timing == MMC_TIMING_UHS_DDR50)
		con |= DDR;	/* configure in DDR mode */
	else
		con &= ~DDR;
	switch (ios->bus_width) {
	case MMC_BUS_WIDTH_8:
		OMAP_HSMMC_WRITE(host->base, CON, con | DW8);
		break;
	case MMC_BUS_WIDTH_4:
		OMAP_HSMMC_WRITE(host->base, CON, con & ~DW8);
		OMAP_HSMMC_WRITE(host->base, HCTL,
			OMAP_HSMMC_READ(host->base, HCTL) | FOUR_BIT);
		break;
	case MMC_BUS_WIDTH_1:
		OMAP_HSMMC_WRITE(host->base, CON, con & ~DW8);
		OMAP_HSMMC_WRITE(host->base, HCTL,
			OMAP_HSMMC_READ(host->base, HCTL) & ~FOUR_BIT);
		break;
	}
}

static void omap_hsmmc_set_bus_mode(struct omap_hsmmc_host *host)
{
	struct mmc_ios *ios = &host->mmc->ios;
	u32 con;

	con = OMAP_HSMMC_READ(host->base, CON);
	if (ios->bus_mode == MMC_BUSMODE_OPENDRAIN)
		OMAP_HSMMC_WRITE(host->base, CON, con | OD);
	else
		OMAP_HSMMC_WRITE(host->base, CON, con & ~OD);
}

#ifdef CONFIG_PM

/*
 * Restore the MMC host context, if it was lost as result of a
 * power state change.
 */
static int omap_hsmmc_context_restore(struct omap_hsmmc_host *host)
{
	struct mmc_ios *ios = &host->mmc->ios;
	u32 hctl, capa;
	u32 value;
	unsigned long timeout;

	if (!OMAP_HSMMC_READ(host->base, SYSSTATUS) & RESETDONE)
		return 1;

	if (host->con == OMAP_HSMMC_READ(host->base, CON) &&
	    host->hctl == OMAP_HSMMC_READ(host->base, HCTL) &&
	    host->sysctl == OMAP_HSMMC_READ(host->base, SYSCTL) &&
	    host->capa == OMAP_HSMMC_READ(host->base, CAPA))
		return 0;

	host->context_loss++;

	if (host->pdata->controller_flags & OMAP_HSMMC_SUPPORTS_DUAL_VOLT) {
		if (host->power_mode != MMC_POWER_OFF &&
		    ((1 << ios->vdd) <= MMC_VDD_23_24 ||
		    (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180)))
			hctl = SDVS18;
		else
			hctl = SDVS30;
		capa = VS30 | VS18;
	} else {
		hctl = SDVS18;
		capa = VS18;
	}

	OMAP_HSMMC_WRITE(host->base, HCTL,
			OMAP_HSMMC_READ(host->base, HCTL) | hctl);

	OMAP_HSMMC_WRITE(host->base, CAPA,
			OMAP_HSMMC_READ(host->base, CAPA) | capa);

	OMAP_HSMMC_WRITE(host->base, HCTL,
			OMAP_HSMMC_READ(host->base, HCTL) | SDBP);

	timeout = jiffies + msecs_to_jiffies(MMC_TIMEOUT_MS);
	while ((OMAP_HSMMC_READ(host->base, HCTL) & SDBP) != SDBP
		&& time_before(jiffies, timeout))
		;

	omap_hsmmc_disable_irq(host);

	/* Do not initialize card-specific things if the power is off */
	if (host->power_mode == MMC_POWER_OFF)
		goto out;

	omap_hsmmc_set_bus_width(host);

	omap_hsmmc_set_clock(host);

	omap_hsmmc_set_bus_mode(host);

	if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180) {
		value = OMAP_HSMMC_READ(host->base, HCTL);
		value &= ~SDVS_MASK;
		value |= SDVS18;
		OMAP_HSMMC_WRITE(host->base, HCTL, value);

		value = OMAP_HSMMC_READ(host->base, AC12);
		value |= SIGEN_V1V8;
		OMAP_HSMMC_WRITE(host->base, AC12, value);
	}

out:
	dev_dbg(mmc_dev(host->mmc), "context is restored: restore count %d\n",
		host->context_loss);
	return 0;
}

/*
 * Save the MMC host context (store the number of power state changes so far).
 */
static void omap_hsmmc_context_save(struct omap_hsmmc_host *host)
{
	host->con =  OMAP_HSMMC_READ(host->base, CON);
	host->hctl = OMAP_HSMMC_READ(host->base, HCTL);
	host->sysctl =  OMAP_HSMMC_READ(host->base, SYSCTL);
	host->capa = OMAP_HSMMC_READ(host->base, CAPA);
}

#else

static int omap_hsmmc_context_restore(struct omap_hsmmc_host *host)
{
	return 0;
}

static void omap_hsmmc_context_save(struct omap_hsmmc_host *host)
{
}

#endif

/*
 * Send init stream sequence to card
 * before sending IDLE command
 */
static void send_init_stream(struct omap_hsmmc_host *host)
{
	int reg = 0;
	unsigned long timeout;

	if (host->protect_card)
		return;

	disable_irq(host->irq);

	OMAP_HSMMC_WRITE(host->base, IE, INT_EN_MASK);
	OMAP_HSMMC_WRITE(host->base, CON,
		OMAP_HSMMC_READ(host->base, CON) | INIT_STREAM);
	OMAP_HSMMC_WRITE(host->base, CMD, INIT_STREAM_CMD);

	timeout = jiffies + msecs_to_jiffies(MMC_TIMEOUT_MS);
	while ((reg != CC_EN) && time_before(jiffies, timeout))
		reg = OMAP_HSMMC_READ(host->base, STAT) & CC_EN;

	OMAP_HSMMC_WRITE(host->base, CON,
		OMAP_HSMMC_READ(host->base, CON) & ~INIT_STREAM);

	OMAP_HSMMC_WRITE(host->base, STAT, STAT_CLEAR);
	OMAP_HSMMC_READ(host->base, STAT);

	enable_irq(host->irq);
}

static inline
int omap_hsmmc_cover_is_closed(struct omap_hsmmc_host *host)
{
	int r = 1;

	if (mmc_slot(host).get_cover_state)
		r = mmc_slot(host).get_cover_state(host->dev, host->slot_id);
	return r;
}

static ssize_t
omap_hsmmc_show_cover_switch(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct mmc_host *mmc = container_of(dev, struct mmc_host, class_dev);
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	return sprintf(buf, "%s\n",
			omap_hsmmc_cover_is_closed(host) ? "closed" : "open");
}

static DEVICE_ATTR(cover_switch, S_IRUGO, omap_hsmmc_show_cover_switch, NULL);

static ssize_t
omap_hsmmc_show_slot_name(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct mmc_host *mmc = container_of(dev, struct mmc_host, class_dev);
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	return sprintf(buf, "%s\n", mmc_slot(host).name);
}

static DEVICE_ATTR(slot_name, S_IRUGO, omap_hsmmc_show_slot_name, NULL);

/*
 * Configure the response type and send the cmd.
 */
static void
omap_hsmmc_start_command(struct omap_hsmmc_host *host, struct mmc_command *cmd,
	struct mmc_data *data, bool autocmd12)
{
	int cmdreg = 0, resptype = 0, cmdtype = 0;

	dev_vdbg(mmc_dev(host->mmc), "%s: CMD%d, argument 0x%08x\n",
		mmc_hostname(host->mmc), cmd->opcode, cmd->arg);
	host->cmd = cmd;

	omap_hsmmc_enable_irq(host, cmd);

	host->response_busy = 0;
	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136)
			resptype = 1;
		else if (cmd->flags & MMC_RSP_BUSY) {
			resptype = 3;
			host->response_busy = 1;
		} else
			resptype = 2;
	}

	/*
	 * Unlike OMAP1 controller, the cmdtype does not seem to be based on
	 * ac, bc, adtc, bcr. Only commands ending an open ended transfer need
	 * a val of 0x3, rest 0x0.
	 */
	if (cmd == host->mrq->stop)
		cmdtype = 0x3;

	cmdreg = (cmd->opcode << 24) | (resptype << 16) | (cmdtype << 22);
	if ((host->flags & AUTO_CMD23) && mmc_op_multi(cmd->opcode) &&
	    host->mrq->sbc) {
		cmdreg |= ACEN_ACMD23;
		OMAP_HSMMC_WRITE(host->base, SDMASA, host->mrq->sbc->arg);
	} else if ((host->flags & AUTO_CMD12) && mmc_op_multi(cmd->opcode) &&
								autocmd12)
		cmdreg |= ACEN_ACMD12;

	if (data) {
		cmdreg |= DP_SELECT | MSBS | BCE;
		if (data->flags & MMC_DATA_READ)
			cmdreg |= DDIR;
		else
			cmdreg &= ~(DDIR);
	}

	if (host->use_dma)
		cmdreg |= DMAE;

	host->req_in_progress = 1;

	OMAP_HSMMC_WRITE(host->base, ARG, cmd->arg);
	OMAP_HSMMC_WRITE(host->base, CMD, cmdreg);
}

static int
omap_hsmmc_get_dma_dir(struct omap_hsmmc_host *host, struct mmc_data *data)
{
	if (data->flags & MMC_DATA_WRITE)
		return DMA_TO_DEVICE;
	else
		return DMA_FROM_DEVICE;
}

static struct dma_chan *omap_hsmmc_get_dma_chan(struct omap_hsmmc_host *host,
	struct mmc_data *data)
{
	return data->flags & MMC_DATA_WRITE ? host->tx_chan : host->rx_chan;
}

static void omap_hsmmc_request_done(struct omap_hsmmc_host *host, struct mmc_request *mrq)
{
	int dma_ch;
	unsigned long flags;

	spin_lock_irqsave(&host->irq_lock, flags);
	host->req_in_progress = 0;
	dma_ch = host->dma_ch;
	spin_unlock_irqrestore(&host->irq_lock, flags);

	omap_hsmmc_disable_irq(host);
	/* Do not complete the request if DMA is still in progress */
	if (mrq->data && host->use_dma && dma_ch != -1)
		return;
	host->mrq = NULL;
	mmc_request_done(host->mmc, mrq);
}

/*
 * Notify the transfer complete to MMC core
 */
static void
omap_hsmmc_xfer_done(struct omap_hsmmc_host *host, struct mmc_data *data)
{
	if (!data) {
		struct mmc_request *mrq = host->mrq;

		/* TC before CC from CMD6 - don't know why, but it happens */
		if (host->cmd && host->cmd->opcode == 6 &&
		    host->response_busy) {
			host->response_busy = 0;
			return;
		}

		omap_hsmmc_request_done(host, mrq);
		return;
	}

	host->data = NULL;

	if (!data->error)
		data->bytes_xfered += data->blocks * (data->blksz);
	else
		data->bytes_xfered = 0;

	if (data->stop && (data->error || (!(host->flags & AUTO_CMD12) &&
		!host->mrq->sbc))) {
		/*
		 * If there is any error or open-end read/write with autocmd12
		 * disabled
		 */
		omap_hsmmc_start_command(host, data->stop, NULL, 0);
	} else {
		/* status update for autocmd12 of open-end read/write */
		if (data->stop && !host->mrq->sbc)
			data->stop->resp[0] = OMAP_HSMMC_READ(host->base,
							RSP76);
		omap_hsmmc_request_done(host, data->mrq);
	}

	return;
}

static void omap_hsmmc_start_dma_transfer(struct omap_hsmmc_host *host)
{
	struct mmc_request *req;
	struct dma_chan *chan;
	req = host->mrq;

	if (!req->data)
		return;
	OMAP_HSMMC_WRITE(host->base, BLK, (req->data->blksz)
				| (req->data->blocks << 16));
	set_data_timeout(host, req->data->timeout_ns,
				req->data->timeout_clks);
	chan = omap_hsmmc_get_dma_chan(host, req->data);
	dma_async_issue_pending(chan);
}

/*
 * Notify the core about command completion
 */
static void
omap_hsmmc_cmd_done(struct omap_hsmmc_host *host, struct mmc_command *cmd)
{
	struct mmc_request *req;
	req = host->mrq;

	if (host->mrq->sbc && (host->cmd == host->mrq->sbc) &&
	    !host->mrq->sbc->error && !(host->flags & AUTO_CMD23)) {
		host->cmd = NULL;
		omap_hsmmc_start_dma_transfer(host);
		omap_hsmmc_start_command(host, host->mrq->cmd,
						host->mrq->data, 0);
		return;
	}

	host->cmd = NULL;

	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136) {
			/* response type 2 */
			cmd->resp[3] = OMAP_HSMMC_READ(host->base, RSP10);
			cmd->resp[2] = OMAP_HSMMC_READ(host->base, RSP32);
			cmd->resp[1] = OMAP_HSMMC_READ(host->base, RSP54);
			cmd->resp[0] = OMAP_HSMMC_READ(host->base, RSP76);
		} else {
			/* response types 1, 1b, 3, 4, 5, 6 */
			cmd->resp[0] = OMAP_HSMMC_READ(host->base, RSP10);
		}
	}
	if ((host->data == NULL && !host->response_busy) || cmd->error)
		omap_hsmmc_request_done(host, cmd->mrq);
}

/*
 * DMA clean up for command errors
 */
static void omap_hsmmc_dma_cleanup(struct omap_hsmmc_host *host, int errno)
{
	int dma_ch;
	unsigned long flags;

	host->data->error = errno;

	spin_lock_irqsave(&host->irq_lock, flags);
	dma_ch = host->dma_ch;
	host->dma_ch = -1;
	spin_unlock_irqrestore(&host->irq_lock, flags);

	if (host->use_dma && dma_ch != -1) {
		struct dma_chan *chan = omap_hsmmc_get_dma_chan(host, host->data);

		dmaengine_terminate_all(chan);
		dma_unmap_sg(chan->device->dev,
			host->data->sg, host->data->sg_len,
			omap_hsmmc_get_dma_dir(host, host->data));

		host->data->host_cookie = 0;
	}
	host->data = NULL;
}

/*
 * Readable error output
 */
#ifdef CONFIG_MMC_DEBUG
static void omap_hsmmc_dbg_report_irq(struct omap_hsmmc_host *host, u32 status)
{
	/* --- means reserved bit without definition at documentation */
	static const char *omap_hsmmc_status_bits[] = {
		"CC"  , "TC"  , "BGE", "---", "BWR" , "BRR" , "---" , "---" ,
		"CIRQ",	"OBI" , "---", "---", "---" , "---" , "---" , "ERRI",
		"CTO" , "CCRC", "CEB", "CIE", "DTO" , "DCRC", "DEB" , "---" ,
		"ACE" , "---" , "---", "---", "CERR", "BADA", "---" , "---"
	};
	char res[256];
	char *buf = res;
	int len, i;

	len = sprintf(buf, "MMC IRQ 0x%x :", status);
	buf += len;

	for (i = 0; i < ARRAY_SIZE(omap_hsmmc_status_bits); i++)
		if (status & (1 << i)) {
			len = sprintf(buf, " %s", omap_hsmmc_status_bits[i]);
			buf += len;
		}

	dev_vdbg(mmc_dev(host->mmc), "%s\n", res);
}
#else
static inline void omap_hsmmc_dbg_report_irq(struct omap_hsmmc_host *host,
					     u32 status)
{
}
#endif  /* CONFIG_MMC_DEBUG */

/*
 * MMC controller internal state machines reset
 *
 * Used to reset command or data internal state machines, using respectively
 *  SRC or SRD bit of SYSCTL register
 * Can be called from interrupt context
 */
static inline void omap_hsmmc_reset_controller_fsm(struct omap_hsmmc_host *host,
						   unsigned long bit)
{
	unsigned long i = 0;
	unsigned long limit = MMC_TIMEOUT_US;

	OMAP_HSMMC_WRITE(host->base, SYSCTL,
			 OMAP_HSMMC_READ(host->base, SYSCTL) | bit);

	/*
	 * OMAP4 ES2 and greater has an updated reset logic.
	 * Monitor a 0->1 transition first
	 */
	if (mmc_slot(host).features & HSMMC_HAS_UPDATED_RESET) {
		while ((!(OMAP_HSMMC_READ(host->base, SYSCTL) & bit))
					&& (i++ < limit))
			udelay(1);
	}

	i = 0;

	while ((OMAP_HSMMC_READ(host->base, SYSCTL) & bit) &&
		(i++ < limit))
		udelay(1);

	if (OMAP_HSMMC_READ(host->base, SYSCTL) & bit)
		dev_err(mmc_dev(host->mmc),
			"Timeout waiting on controller reset in %s\n",
			__func__);
}

static void hsmmc_command_incomplete(struct omap_hsmmc_host *host,
					int err, int end_cmd)
{
	if (end_cmd) {
		omap_hsmmc_reset_controller_fsm(host, SRC);
		if (host->cmd)
			host->cmd->error = err;
	}

	if (host->data) {
		omap_hsmmc_reset_controller_fsm(host, SRD);
		omap_hsmmc_dma_cleanup(host, err);
	} else if (host->mrq && host->mrq->cmd)
		host->mrq->cmd->error = err;
}

static void omap_hsmmc_do_irq(struct omap_hsmmc_host *host, int status)
{
	struct mmc_data *data;
	int end_cmd = 0, end_trans = 0;
	int error = 0;

	data = host->data;
	dev_vdbg(mmc_dev(host->mmc), "IRQ Status is %x\n", status);

	if (status & ERR_EN) {
		omap_hsmmc_dbg_report_irq(host, status);

		if (status & (CTO_EN | CCRC_EN))
			end_cmd = 1;
		if (status & (CTO_EN | DTO_EN))
			hsmmc_command_incomplete(host, -ETIMEDOUT, end_cmd);
		else if (status & (CCRC_EN | DCRC_EN))
			hsmmc_command_incomplete(host, -EILSEQ, end_cmd);

		if (status & ACE_EN) {
			u32 ac12;
			ac12 = OMAP_HSMMC_READ(host->base, AC12);
			if (!(ac12 & ACNE) && host->mrq->sbc) {
				end_cmd = 1;
				if (ac12 & ACTO)
					error =  -ETIMEDOUT;
				else if (ac12 & (ACCE | ACEB | ACIE))
					error = -EILSEQ;
				host->mrq->sbc->error = error;
				hsmmc_command_incomplete(host, error, end_cmd);
			}
			if (!(ac12 & ACNE) && !host->mrq->sbc &&
			    host->mrq->data) {
				end_trans = 1;
				if (ac12 & ACTO)
					host->mrq->data->error = -ETIMEDOUT;
				else if (ac12 & (ACCE | ACEB | ACIE))
					host->mrq->data->error = -EILSEQ;
				omap_hsmmc_reset_controller_fsm(host, SRC);
			}
			dev_dbg(mmc_dev(host->mmc), "AC12 err: 0x%x\n", ac12);
		}
		if (host->data || host->response_busy) {
			end_trans = !end_cmd;
			host->response_busy = 0;
		}
	}

	OMAP_HSMMC_WRITE(host->base, STAT, status);
	if (end_cmd || ((status & CC_EN) && host->cmd))
		omap_hsmmc_cmd_done(host, host->cmd);
	if ((end_trans || (status & TC_EN)) && host->mrq)
		omap_hsmmc_xfer_done(host, data);
}

/*
 * MMC controller IRQ handler
 */
static irqreturn_t omap_hsmmc_irq(int irq, void *dev_id)
{
	struct omap_hsmmc_host *host = dev_id;
	int status;

	status = OMAP_HSMMC_READ(host->base, STAT);
	while (status & INT_EN_MASK && host->req_in_progress) {
		omap_hsmmc_do_irq(host, status);

		/* Flush posted write */
		status = OMAP_HSMMC_READ(host->base, STAT);
	}

	return IRQ_HANDLED;
}

static void set_sd_bus_power(struct omap_hsmmc_host *host)
{
	unsigned long i;

	OMAP_HSMMC_WRITE(host->base, HCTL,
			 OMAP_HSMMC_READ(host->base, HCTL) | SDBP);
	for (i = 0; i < loops_per_jiffy; i++) {
		if (OMAP_HSMMC_READ(host->base, HCTL) & SDBP)
			break;
		cpu_relax();
	}
}

/*
 * Switch MMC interface voltage ... only relevant for MMC1.
 *
 * MMC2 and MMC3 use fixed 1.8V levels, and maybe a transceiver.
 * The MMC2 transceiver controls are used instead of DAT4..DAT7.
 * Some chips, like eMMC ones, use internal transceivers.
 */
static int omap_hsmmc_switch_opcond(struct omap_hsmmc_host *host, int vdd)
{
	u32 reg_val = 0;
	int ret;

	/* Disable the clocks */
	pm_runtime_put_sync(host->dev);
	if (host->dbclk)
		clk_disable_unprepare(host->dbclk);

	/* Turn the power off */
	ret = mmc_slot(host).set_power(host->dev, host->slot_id, 0, 0);

	/* Turn the power ON with given VDD 1.8 or 3.0v */
	if (!ret)
		ret = mmc_slot(host).set_power(host->dev, host->slot_id, 1,
					       vdd);
	pm_runtime_get_sync(host->dev);
	if (host->dbclk)
		clk_prepare_enable(host->dbclk);

	if (ret != 0)
		goto err;

	OMAP_HSMMC_WRITE(host->base, HCTL,
		OMAP_HSMMC_READ(host->base, HCTL) & SDVSCLR);
	reg_val = OMAP_HSMMC_READ(host->base, HCTL);

	/*
	 * If a MMC dual voltage card is detected, the set_ios fn calls
	 * this fn with VDD bit set for 1.8V. Upon card removal from the
	 * slot, omap_hsmmc_set_ios sets the VDD back to 3V on MMC_POWER_OFF.
	 *
	 * Cope with a bit of slop in the range ... per data sheets:
	 *  - "1.8V" for vdds_mmc1/vdds_mmc1a can be up to 2.45V max,
	 *    but recommended values are 1.71V to 1.89V
	 *  - "3.0V" for vdds_mmc1/vdds_mmc1a can be up to 3.5V max,
	 *    but recommended values are 2.7V to 3.3V
	 *
	 * Board setup code shouldn't permit anything very out-of-range.
	 * TWL4030-family VMMC1 and VSIM regulators are fine (avoiding the
	 * middle range) but VSIM can't power DAT4..DAT7 at more than 3V.
	 */
	if ((1 << vdd) <= MMC_VDD_23_24)
		reg_val |= SDVS18;
	else
		reg_val |= SDVS30;

	OMAP_HSMMC_WRITE(host->base, HCTL, reg_val);
	set_sd_bus_power(host);

	return 0;
err:
	dev_err(mmc_dev(host->mmc), "Unable to switch operating voltage\n");
	return ret;
}

/* Protect the card while the cover is open */
static void omap_hsmmc_protect_card(struct omap_hsmmc_host *host)
{
	if (!mmc_slot(host).get_cover_state)
		return;

	host->reqs_blocked = 0;
	if (mmc_slot(host).get_cover_state(host->dev, host->slot_id)) {
		if (host->protect_card) {
			dev_info(host->dev, "%s: cover is closed, "
					 "card is now accessible\n",
					 mmc_hostname(host->mmc));
			host->protect_card = 0;
		}
	} else {
		if (!host->protect_card) {
			dev_info(host->dev, "%s: cover is open, "
					 "card is now inaccessible\n",
					 mmc_hostname(host->mmc));
			host->protect_card = 1;
		}
	}
}

/*
 * irq handler to notify the core about card insertion/removal
 */
static irqreturn_t omap_hsmmc_detect(int irq, void *dev_id)
{
	struct omap_hsmmc_host *host = dev_id;
	struct omap_mmc_slot_data *slot = &mmc_slot(host);
	int carddetect;

	if (host->suspended)
		return IRQ_HANDLED;

	sysfs_notify(&host->mmc->class_dev.kobj, NULL, "cover_switch");

	if (slot->card_detect)
		carddetect = slot->card_detect(host->dev, host->slot_id);
	else {
		omap_hsmmc_protect_card(host);
		carddetect = -ENOSYS;
	}

	if (carddetect)
		mmc_detect_change(host->mmc, (HZ * 200) / 1000);
	else
		mmc_detect_change(host->mmc, (HZ * 50) / 1000);
	return IRQ_HANDLED;
}

static void omap_hsmmc_dma_callback(void *param)
{
	struct omap_hsmmc_host *host = param;
	struct dma_chan *chan;
	struct mmc_data *data;
	int req_in_progress;

	spin_lock_irq(&host->irq_lock);
	if (host->dma_ch < 0) {
		spin_unlock_irq(&host->irq_lock);
		return;
	}

	data = host->mrq->data;
	chan = omap_hsmmc_get_dma_chan(host, data);
	if (!data->host_cookie)
		dma_unmap_sg(chan->device->dev,
			     data->sg, data->sg_len,
			     omap_hsmmc_get_dma_dir(host, data));

	req_in_progress = host->req_in_progress;
	host->dma_ch = -1;
	spin_unlock_irq(&host->irq_lock);

	/* If DMA has finished after TC, complete the request */
	if (!req_in_progress) {
		struct mmc_request *mrq = host->mrq;

		host->mrq = NULL;
		mmc_request_done(host->mmc, mrq);
	}
}

static int omap_hsmmc_pre_dma_transfer(struct omap_hsmmc_host *host,
				       struct mmc_data *data,
				       struct omap_hsmmc_next *next,
				       struct dma_chan *chan)
{
	int dma_len;

	if (!next && data->host_cookie &&
	    data->host_cookie != host->next_data.cookie) {
		dev_warn(host->dev, "[%s] invalid cookie: data->host_cookie %d"
		       " host->next_data.cookie %d\n",
		       __func__, data->host_cookie, host->next_data.cookie);
		data->host_cookie = 0;
	}

	/* Check if next job is already prepared */
	if (next ||
	    (!next && data->host_cookie != host->next_data.cookie)) {
		dma_len = dma_map_sg(chan->device->dev, data->sg, data->sg_len,
				     omap_hsmmc_get_dma_dir(host, data));

	} else {
		dma_len = host->next_data.dma_len;
		host->next_data.dma_len = 0;
	}


	if (dma_len == 0)
		return -EINVAL;

	if (next) {
		next->dma_len = dma_len;
		data->host_cookie = ++next->cookie < 0 ? 1 : next->cookie;
	} else
		host->dma_len = dma_len;

	return 0;
}

/*
 * Routine to configure and start DMA for the MMC card
 */
static int omap_hsmmc_setup_dma_transfer(struct omap_hsmmc_host *host,
					struct mmc_request *req)
{
	struct dma_slave_config cfg;
	struct dma_async_tx_descriptor *tx;
	int ret = 0, i;
	struct mmc_data *data = req->data;
	struct dma_chan *chan;

	/* Sanity check: all the SG entries must be aligned by block size. */
	for (i = 0; i < data->sg_len; i++) {
		struct scatterlist *sgl;

		sgl = data->sg + i;
		if (sgl->length % data->blksz)
			return -EINVAL;
	}
	if ((data->blksz % 4) != 0)
		/* REVISIT: The MMC buffer increments only when MSB is written.
		 * Return error for blksz which is non multiple of four.
		 */
		return -EINVAL;

	BUG_ON(host->dma_ch != -1);

	chan = omap_hsmmc_get_dma_chan(host, data);

	cfg.src_addr = host->mapbase + OMAP_HSMMC_DATA;
	cfg.dst_addr = host->mapbase + OMAP_HSMMC_DATA;
	cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.src_maxburst = data->blksz / 4;
	cfg.dst_maxburst = data->blksz / 4;

	ret = dmaengine_slave_config(chan, &cfg);
	if (ret)
		return ret;

	ret = omap_hsmmc_pre_dma_transfer(host, data, NULL, chan);
	if (ret)
		return ret;

	tx = dmaengine_prep_slave_sg(chan, data->sg, data->sg_len,
		data->flags & MMC_DATA_WRITE ? DMA_MEM_TO_DEV : DMA_DEV_TO_MEM,
		DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!tx) {
		dev_err(mmc_dev(host->mmc), "prep_slave_sg() failed\n");
		/* FIXME: cleanup */
		return -1;
	}

	tx->callback = omap_hsmmc_dma_callback;
	tx->callback_param = host;

	/* Does not fail */
	dmaengine_submit(tx);

	host->dma_ch = 1;

	return 0;
}

static void set_data_timeout(struct omap_hsmmc_host *host,
			     unsigned int timeout_ns,
			     unsigned int timeout_clks)
{
	unsigned int timeout, cycle_ns;
	uint32_t reg, clkd, dto = 0;

	reg = OMAP_HSMMC_READ(host->base, SYSCTL);
	clkd = (reg & CLKD_MASK) >> CLKD_SHIFT;
	if (clkd == 0)
		clkd = 1;

	cycle_ns = 1000000000 / (host->clk_rate / clkd);
	timeout = timeout_ns / cycle_ns;
	timeout += timeout_clks;
	if (timeout) {
		while ((timeout & 0x80000000) == 0) {
			dto += 1;
			timeout <<= 1;
		}
		dto = 31 - dto;
		timeout <<= 1;
		if (timeout && dto)
			dto += 1;
		if (dto >= 13)
			dto -= 13;
		else
			dto = 0;
		if (dto > 14)
			dto = 14;
	}

	reg &= ~DTO_MASK;
	reg |= dto << DTO_SHIFT;
	OMAP_HSMMC_WRITE(host->base, SYSCTL, reg);
}

/*
 * Configure block length for MMC/SD cards and initiate the transfer.
 */
static int
omap_hsmmc_prepare_data(struct omap_hsmmc_host *host, struct mmc_request *req)
{
	int ret;
	host->data = req->data;

	if (req->data == NULL) {
		OMAP_HSMMC_WRITE(host->base, BLK, 0);
		/*
		 * Set an arbitrary 100ms data timeout for commands with
		 * busy signal.
		 */
		if (req->cmd->flags & MMC_RSP_BUSY)
			set_data_timeout(host, 100000000U, 0);
		return 0;
	}

	if (host->use_dma) {
		ret = omap_hsmmc_setup_dma_transfer(host, req);
		if (ret != 0) {
			dev_err(mmc_dev(host->mmc), "MMC start dma failure\n");
			return ret;
		}
	}
	return 0;
}

static void omap_hsmmc_post_req(struct mmc_host *mmc, struct mmc_request *mrq,
				int err)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);
	struct mmc_data *data = mrq->data;

	if (host->use_dma && data->host_cookie) {
		struct dma_chan *c = omap_hsmmc_get_dma_chan(host, data);

		dma_unmap_sg(c->device->dev, data->sg, data->sg_len,
			     omap_hsmmc_get_dma_dir(host, data));
		data->host_cookie = 0;
	}
}

static void omap_hsmmc_pre_req(struct mmc_host *mmc, struct mmc_request *mrq,
			       bool is_first_req)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	if (mrq->data->host_cookie) {
		mrq->data->host_cookie = 0;
		return ;
	}

	if (host->use_dma) {
		struct dma_chan *c = omap_hsmmc_get_dma_chan(host, mrq->data);

		if (omap_hsmmc_pre_dma_transfer(host, mrq->data,
						&host->next_data, c))
			mrq->data->host_cookie = 0;
	}
}

/*
 * Request function. for read/write operation
 */
static void omap_hsmmc_request(struct mmc_host *mmc, struct mmc_request *req)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);
	int err;

	BUG_ON(host->req_in_progress);
	BUG_ON(host->dma_ch != -1);
	if (host->protect_card) {
		if (host->reqs_blocked < 3) {
			/*
			 * Ensure the controller is left in a consistent
			 * state by resetting the command and data state
			 * machines.
			 */
			omap_hsmmc_reset_controller_fsm(host, SRD);
			omap_hsmmc_reset_controller_fsm(host, SRC);
			host->reqs_blocked += 1;
		}
		req->cmd->error = -EBADF;
		if (req->data)
			req->data->error = -EBADF;
		req->cmd->retries = 0;
		mmc_request_done(mmc, req);
		return;
	} else if (host->reqs_blocked)
		host->reqs_blocked = 0;
	WARN_ON(host->mrq != NULL);
	host->mrq = req;
	host->clk_rate = clk_get_rate(host->fclk);
	err = omap_hsmmc_prepare_data(host, req);
	if (err) {
		req->cmd->error = err;
		if (req->data)
			req->data->error = err;
		host->mrq = NULL;
		mmc_request_done(mmc, req);
		return;
	}
	if (req->sbc && !(host->flags & AUTO_CMD23)) {
		omap_hsmmc_start_command(host, req->sbc, NULL, 0);
		return;
	}
	omap_hsmmc_start_dma_transfer(host);
	omap_hsmmc_start_command(host, req->cmd, req->data, 1);
}

/* Routine to configure clock values. Exposed API to core */
static void omap_hsmmc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);
	int do_send_init_stream = 0;

	pm_runtime_get_sync(host->dev);

	if (ios->power_mode != host->power_mode) {
		switch (ios->power_mode) {
		case MMC_POWER_OFF:
			mmc_slot(host).set_power(host->dev, host->slot_id,
						 0, 0);
			break;
		case MMC_POWER_UP:
			mmc_slot(host).set_power(host->dev, host->slot_id,
						 1, ios->vdd);
			break;
		case MMC_POWER_ON:
			do_send_init_stream = 1;
			break;
		}
		host->power_mode = ios->power_mode;
	}

	/* FIXME: set registers based only on changes to ios */

	omap_hsmmc_set_bus_width(host);

	if (host->pdata->controller_flags & OMAP_HSMMC_SUPPORTS_DUAL_VOLT) {
		/* Only MMC1 can interface at 3V without some flavor
		 * of external transceiver; but they all handle 1.8V.
		 */
		if ((OMAP_HSMMC_READ(host->base, HCTL) & SDVSDET) &&
			(ios->vdd == DUAL_VOLT_OCR_BIT)) {
				/*
				 * The mmc_select_voltage fn of the core does
				 * not seem to set the power_mode to
				 * MMC_POWER_UP upon recalculating the voltage.
				 * vdd 1.8v.
				 */
			if (omap_hsmmc_switch_opcond(host, ios->vdd) != 0)
				dev_dbg(mmc_dev(host->mmc),
						"Switch operation failed\n");
		}
	}

	if (!ios->clock)
		goto end_ios;
	omap_hsmmc_set_clock(host);

	if (do_send_init_stream)
		send_init_stream(host);

	omap_hsmmc_set_bus_mode(host);

end_ios:
	pm_runtime_put_autosuspend(host->dev);
}

static int omap_hsmmc_get_cd(struct mmc_host *mmc)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	if (!mmc_slot(host).card_detect)
		return -ENOSYS;
	return mmc_slot(host).card_detect(host->dev, host->slot_id);
}

static int omap_hsmmc_get_ro(struct mmc_host *mmc)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	if (!mmc_slot(host).get_ro)
		return -ENOSYS;
	return mmc_slot(host).get_ro(host->dev, 0);
}

static void omap_hsmmc_init_card(struct mmc_host *mmc, struct mmc_card *card)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	if (mmc_slot(host).init_card)
		mmc_slot(host).init_card(card);
}

static void omap_hsmmc_conf_bus_power(struct omap_hsmmc_host *host)
{
	u32 hctl, capa, value;

	/* Only MMC1 supports 3.0V */
	if (host->pdata->controller_flags & OMAP_HSMMC_SUPPORTS_DUAL_VOLT) {
		hctl = SDVS30;
		capa = VS30 | VS18;
	} else {
		hctl = SDVS18;
		capa = VS18;
	}

	value = OMAP_HSMMC_READ(host->base, HCTL) & ~SDVS_MASK;
	OMAP_HSMMC_WRITE(host->base, HCTL, value | hctl);

	value = OMAP_HSMMC_READ(host->base, CAPA);
	OMAP_HSMMC_WRITE(host->base, CAPA, value | capa);

	/* Set SD bus power bit */
	set_sd_bus_power(host);
}

static int omap_hsmmc_enable_fclk(struct mmc_host *mmc)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	pm_runtime_get_sync(host->dev);

	return 0;
}

static int omap_hsmmc_disable_fclk(struct mmc_host *mmc)
{
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	pm_runtime_mark_last_busy(host->dev);
	pm_runtime_put_autosuspend(host->dev);

	return 0;
}

static int omap_start_signal_voltage_switch(struct mmc_host *mmc,
			struct mmc_ios *ios)
{
	struct omap_hsmmc_host *host;
	u32 value = 0;
	int ret = 0;

	if (!(mmc->caps & (MMC_CAP_UHS_SDR12 |
			MMC_CAP_UHS_SDR25 | MMC_CAP_UHS_DDR50)))
		return 0;

	host  = mmc_priv(mmc);

	if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_330) {
		omap_hsmmc_conf_bus_power(host);

		value = OMAP_HSMMC_READ(host->base, AC12);
		value &= ~SIGEN_V1V8;
		OMAP_HSMMC_WRITE(host->base, AC12, value);
		dev_dbg(mmc_dev(host->mmc), " i/o voltage switch to 3V\n");
		return 0;
	}

	if (host->pinctrl_muxpd) {
		ret = pinctrl_select_state(host->pinctrl, host->pinctrl_muxpd);
		if (ret < 0) {
			dev_err(host->dev, "pinctrl pinctrl_muxpd select error\n");
			goto voltage_switch_error;
		}
	}

	if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180) {
		value = OMAP_HSMMC_READ(host->base, HCTL);
		value &= ~SDVS_MASK;
		value |= SDVS18;
		OMAP_HSMMC_WRITE(host->base, HCTL, value);
		value |= SDBP;
		OMAP_HSMMC_WRITE(host->base, HCTL, value);

		ret = mmc_slot(host).set_power(host->dev, host->slot_id,
						 1, VDD_165_195);
		if (ret < 0) {
			dev_dbg(mmc_dev(host->mmc), "failed to switch 1.8v\n");
			goto voltage_switch_error;
		}
	}

	if (host->pinctrl_muxpu) {
		ret = pinctrl_select_state(host->pinctrl, host->pinctrl_muxpu);
		if (ret < 0)
			dev_err(host->dev, "pinctrl pinctrl_muxpu select error\n");
	}

voltage_switch_error:
	return ret;
}

static int omap_hsmmc_card_busy_low(struct omap_hsmmc_host *host)
{
	u32 value = 0;
	unsigned long timeout;
	unsigned long notimeout = 0;
	int ret = 1;

	value = OMAP_HSMMC_READ(host->base, CON);
	value &= ~CLKEXTFREE;
	OMAP_HSMMC_WRITE(host->base, CON, (value | PADEN));

	notimeout = 0;
	value = OMAP_HSMMC_READ(host->base, PSTATE);
	timeout = jiffies + msecs_to_jiffies(1);
	do {
		if (!(value & (CLEV | DLEV))) {
			notimeout = 1;
			break;
		}
		usleep_range(100, 200);
		value = OMAP_HSMMC_READ(host->base, PSTATE);
	} while (!time_after(jiffies, timeout));
	if (!notimeout) {
		dev_dbg(mmc_dev(host->mmc), "timeout : i/o low 0x%x\n", value);
		ret = -EIO;
	}

	return ret;
}

static int omap_hsmmc_card_busy_high(struct omap_hsmmc_host *host)
{
	u32 value = 0;
	unsigned long timeout;
	unsigned long notimeout = 0;
	int ret = 0;

	value = OMAP_HSMMC_READ(host->base, CON);
	OMAP_HSMMC_WRITE(host->base, CON, (value | CLKEXTFREE));
	mdelay(1);
	value = OMAP_HSMMC_READ(host->base, PSTATE);

	omap_hsmmc_reset_controller_fsm(host, SRD);
	omap_hsmmc_reset_controller_fsm(host, SRC);
	notimeout = 0;
	timeout = jiffies + msecs_to_jiffies(1);
	do {
		if ((value & (CLEV | DLEV)) == (CLEV | DLEV)) {
			notimeout = 1;
			break;
		}
		usleep_range(100, 200);
		value = OMAP_HSMMC_READ(host->base, PSTATE);
	} while (!time_after(jiffies, timeout));
	if (!notimeout) {
		dev_dbg(mmc_dev(host->mmc), "timeout : i/o high 0x%x\n", value);
		ret = -EIO;
	}

	value = OMAP_HSMMC_READ(host->base, CON);
	OMAP_HSMMC_WRITE(host->base, CON, (value & ~(CLKEXTFREE | PADEN)));

	return ret;
}

static int omap_hsmmc_card_busy(struct mmc_host *mmc)
{
	struct omap_hsmmc_host *host;
	u32 value;
	int ret;

	host  = mmc_priv(mmc);
	value = OMAP_HSMMC_READ(host->base, AC12);

	if (value & SIGEN_V1V8)
		ret = omap_hsmmc_card_busy_high(host);
	else
		ret = omap_hsmmc_card_busy_low(host);

	return ret;
}

static const struct mmc_host_ops omap_hsmmc_ops = {
	.enable = omap_hsmmc_enable_fclk,
	.disable = omap_hsmmc_disable_fclk,
	.post_req = omap_hsmmc_post_req,
	.pre_req = omap_hsmmc_pre_req,
	.request = omap_hsmmc_request,
	.set_ios = omap_hsmmc_set_ios,
	.get_cd = omap_hsmmc_get_cd,
	.get_ro = omap_hsmmc_get_ro,
	.init_card = omap_hsmmc_init_card,
	.start_signal_voltage_switch = omap_start_signal_voltage_switch,
	.card_busy = omap_hsmmc_card_busy,
	/* NYET -- enable_sdio_irq */
};

#ifdef CONFIG_DEBUG_FS

static int omap_hsmmc_regs_show(struct seq_file *s, void *data)
{
	struct mmc_host *mmc = s->private;
	struct omap_hsmmc_host *host = mmc_priv(mmc);

	seq_printf(s, "mmc%d:\n ctx_loss:\t%d\n\nregs:\n",
			mmc->index, host->context_loss);

	if (host->suspended) {
		seq_printf(s, "host suspended, can't read registers\n");
		return 0;
	}

	pm_runtime_get_sync(host->dev);

	seq_printf(s, "CON:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, CON));
	seq_printf(s, "HCTL:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, HCTL));
	seq_printf(s, "SYSCTL:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, SYSCTL));
	seq_printf(s, "IE:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, IE));
	seq_printf(s, "ISE:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, ISE));
	seq_printf(s, "CAPA:\t\t0x%08x\n",
			OMAP_HSMMC_READ(host->base, CAPA));

	pm_runtime_mark_last_busy(host->dev);
	pm_runtime_put_autosuspend(host->dev);

	return 0;
}

static int omap_hsmmc_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, omap_hsmmc_regs_show, inode->i_private);
}

static const struct file_operations mmc_regs_fops = {
	.open           = omap_hsmmc_regs_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static void omap_hsmmc_debugfs(struct mmc_host *mmc)
{
	if (mmc->debugfs_root)
		debugfs_create_file("regs", S_IRUSR, mmc->debugfs_root,
			mmc, &mmc_regs_fops);
}

#else

static void omap_hsmmc_debugfs(struct mmc_host *mmc)
{
}

#endif

#ifdef CONFIG_OF
static u16 omap4_reg_offset = 0x100;

static const struct of_device_id omap_mmc_of_match[] = {
	{
		.compatible = "ti,omap2-hsmmc",
	},
	{
		.compatible = "ti,omap3-hsmmc",
	},
	{
		.compatible = "ti,omap4-hsmmc",
		.data = &omap4_reg_offset,
	},
	{},
};
MODULE_DEVICE_TABLE(of, omap_mmc_of_match);

static struct omap_mmc_platform_data *of_get_hsmmc_pdata(struct device *dev)
{
	struct omap_mmc_platform_data *pdata;
	struct device_node *np = dev->of_node;
	u32 bus_width, max_freq;
	int cd_gpio, wp_gpio;

	cd_gpio = of_get_named_gpio(np, "cd-gpios", 0);
	wp_gpio = of_get_named_gpio(np, "wp-gpios", 0);
	if (cd_gpio == -EPROBE_DEFER || wp_gpio == -EPROBE_DEFER)
		return ERR_PTR(-EPROBE_DEFER);

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL; /* out of memory */

	if (of_find_property(np, "ti,dual-volt", NULL))
		pdata->controller_flags |= OMAP_HSMMC_SUPPORTS_DUAL_VOLT;

	/* This driver only supports 1 slot */
	pdata->nr_slots = 1;
	pdata->slots[0].switch_pin = cd_gpio;
	pdata->slots[0].gpio_wp = wp_gpio;

	if (of_find_property(np, "ti,non-removable", NULL)) {
		pdata->slots[0].nonremovable = true;
		pdata->slots[0].no_regulator_off_init = true;
	}
	of_property_read_u32(np, "bus-width", &bus_width);
	if (bus_width == 4)
		pdata->slots[0].caps |= MMC_CAP_4_BIT_DATA;
	else if (bus_width == 8)
		pdata->slots[0].caps |= MMC_CAP_8_BIT_DATA;

	if (of_find_property(np, "cap-power-off-card", NULL))
		pdata->slots[0].caps |= MMC_CAP_POWER_OFF_CARD;

	if (of_find_property(np, "cap-mmc-dual-data-rate", NULL))
		pdata->slots[0].caps |= MMC_CAP_1_8V_DDR;

	if (of_find_property(np, "keep-power-in-suspend", NULL))
		pdata->slots[0].pm_caps |= MMC_PM_KEEP_POWER;

	if (of_find_property(np, "ti,needs-special-reset", NULL))
		pdata->slots[0].features |= HSMMC_HAS_UPDATED_RESET;

	if (!of_property_read_u32(np, "max-frequency", &max_freq))
		pdata->max_freq = max_freq;

	if (of_find_property(np, "ti,needs-special-hs-handling", NULL))
		pdata->slots[0].features |= HSMMC_HAS_HSPE_SUPPORT;

	pdata->needs_vmmc = of_property_read_bool(np, "vmmc-supply");
	pdata->needs_vmmc_aux = of_property_read_bool(np, "vmmc_aux-supply");

	return pdata;
}
#else
static inline struct omap_mmc_platform_data
			*of_get_hsmmc_pdata(struct device *dev)
{
	return NULL;
}
#endif

static int omap_hsmmc_probe(struct platform_device *pdev)
{
	struct omap_mmc_platform_data *pdata = pdev->dev.platform_data;
	struct mmc_host *mmc;
	struct omap_hsmmc_host *host = NULL;
	struct resource *res;
	int ret, irq;
	const struct of_device_id *match;
	dma_cap_mask_t mask;
	unsigned tx_req, rx_req;
	u32 revision;

	match = of_match_device(of_match_ptr(omap_mmc_of_match), &pdev->dev);
	if (match) {
		pdata = of_get_hsmmc_pdata(&pdev->dev);

		if (IS_ERR(pdata))
			return PTR_ERR(pdata);

		if (match->data) {
			const u16 *offsetp = match->data;
			pdata->reg_offset = *offsetp;
		}
	}

	if (pdata == NULL) {
		dev_err(&pdev->dev, "Platform Data is missing\n");
		return -ENXIO;
	}

	if (pdata->nr_slots == 0) {
		dev_err(&pdev->dev, "No Slots\n");
		return -ENXIO;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	irq = platform_get_irq(pdev, 0);
	if (res == NULL || irq < 0)
		return -ENXIO;

	res = request_mem_region(res->start, resource_size(res), pdev->name);
	if (res == NULL)
		return -EBUSY;

	ret = omap_hsmmc_gpio_init(pdata);
	if (ret)
		goto err;

	mmc = mmc_alloc_host(sizeof(struct omap_hsmmc_host), &pdev->dev);
	if (!mmc) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	host		= mmc_priv(mmc);
	host->mmc	= mmc;
	host->pdata	= pdata;
	host->dev	= &pdev->dev;
	host->use_dma	= 1;
	host->dma_ch	= -1;
	host->irq	= irq;
	host->slot_id	= 0;
	host->mapbase	= res->start + pdata->reg_offset;
	host->base	= ioremap(host->mapbase, SZ_4K);
	host->power_mode = MMC_POWER_OFF;
	host->next_data.cookie = 1;
	host->regulator_enabled = 0;
	host->pbias_enabled = 0;
	host->needs_vmmc = pdata->needs_vmmc;
	host->needs_vmmc_aux = pdata->needs_vmmc_aux;

	platform_set_drvdata(pdev, host);

	mmc->ops	= &omap_hsmmc_ops;

	/*
	 * If regulator_disable can only put vcc_aux to sleep then there is
	 * no off state.
	 */
	if (mmc_slot(host).vcc_aux_disable_is_sleep)
		mmc_slot(host).no_off = 1;

	mmc->f_min = OMAP_MMC_MIN_CLOCK;

	if (pdata->max_freq > 0)
		mmc->f_max = pdata->max_freq;
	else
		mmc->f_max = OMAP_MMC_MAX_CLOCK;

	spin_lock_init(&host->irq_lock);

	host->fclk = clk_get(&pdev->dev, "fck");
	if (IS_ERR(host->fclk)) {
		ret = PTR_ERR(host->fclk);
		host->fclk = NULL;
		goto err1;
	}

	if (host->pdata->controller_flags & OMAP_HSMMC_BROKEN_MULTIBLOCK_READ) {
		dev_info(&pdev->dev, "multiblock reads disabled due to 35xx erratum 2.1.1.128; MMC read performance may suffer\n");
		mmc->caps2 |= MMC_CAP2_NO_MULTI_READ;
	}

	pm_runtime_enable(host->dev);
	pm_runtime_get_sync(host->dev);
	pm_runtime_set_autosuspend_delay(host->dev, MMC_AUTOSUSPEND_DELAY);
	pm_runtime_use_autosuspend(host->dev);

	omap_hsmmc_context_save(host);

	host->dbclk = clk_get(&pdev->dev, "mmchsdb_fck");
	/*
	 * MMC can still work without debounce clock.
	 */
	if (IS_ERR(host->dbclk)) {
		host->dbclk = NULL;
	} else if (clk_prepare_enable(host->dbclk) != 0) {
		dev_warn(mmc_dev(host->mmc), "Failed to enable debounce clk\n");
		clk_put(host->dbclk);
		host->dbclk = NULL;
	}

	/* Set this to a value that allows allocating an entire descriptor
	   list within a page (zero order allocation). */
	mmc->max_segs = 64;

	mmc->max_blk_size = 512;       /* Block Length at max can be 1024 */
	mmc->max_blk_count = 0xFFFF;    /* No. of Blocks is 16 bits */
	mmc->max_req_size = mmc->max_blk_size * mmc->max_blk_count;
	mmc->max_seg_size = mmc->max_req_size;

	mmc->caps |= MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED |
		     MMC_CAP_WAIT_WHILE_BUSY | MMC_CAP_ERASE;

	mmc->caps |= mmc_slot(host).caps;
	if (mmc->caps & MMC_CAP_8_BIT_DATA)
		mmc->caps |= MMC_CAP_4_BIT_DATA;

	if (mmc_slot(host).nonremovable)
		mmc->caps |= MMC_CAP_NONREMOVABLE;

	mmc->pm_caps = mmc_slot(host).pm_caps;

	omap_hsmmc_conf_bus_power(host);

	if (!pdev->dev.of_node) {
		res = platform_get_resource_byname(pdev, IORESOURCE_DMA, "tx");
		if (!res) {
			dev_err(mmc_dev(host->mmc), "cannot get DMA TX channel\n");
			ret = -ENXIO;
			goto err_irq;
		}
		tx_req = res->start;

		res = platform_get_resource_byname(pdev, IORESOURCE_DMA, "rx");
		if (!res) {
			dev_err(mmc_dev(host->mmc), "cannot get DMA RX channel\n");
			ret = -ENXIO;
			goto err_irq;
		}
		rx_req = res->start;
	}

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	host->rx_chan =
		dma_request_slave_channel_compat(mask, omap_dma_filter_fn,
						 &rx_req, &pdev->dev, "rx");

	if (!host->rx_chan) {
		dev_err(mmc_dev(host->mmc), "unable to obtain RX DMA engine channel %u\n", rx_req);
		ret = -ENXIO;
		goto err_irq;
	}

	host->tx_chan =
		dma_request_slave_channel_compat(mask, omap_dma_filter_fn,
						 &tx_req, &pdev->dev, "tx");

	if (!host->tx_chan) {
		dev_err(mmc_dev(host->mmc), "unable to obtain TX DMA engine channel %u\n", tx_req);
		ret = -ENXIO;
		goto err_irq;
	}

	/* Request IRQ for MMC operations */
	ret = request_irq(host->irq, omap_hsmmc_irq, 0,
			mmc_hostname(mmc), host);
	if (ret) {
		dev_err(mmc_dev(host->mmc), "Unable to grab HSMMC IRQ\n");
		goto err_irq;
	}

	if (pdata->init != NULL) {
		if (pdata->init(&pdev->dev) != 0) {
			dev_err(mmc_dev(host->mmc),
				"Unable to configure MMC IRQs\n");
			goto err_irq_cd_init;
		}
	}

	if (omap_hsmmc_have_reg() && !mmc_slot(host).set_power) {
		ret = omap_hsmmc_reg_get(host);
		if (ret)
			goto err_irq_cd;
	}

	revision = OMAP_HSMMC_READ(host->base, REV);
	if ((revision >> OMAP_HSMMC_REV_SHIFT) >= OMAP_HSMMC_REV_33) {
		mmc->caps |= MMC_CAP_CMD23;
		host->flags |= AUTO_CMD23;
		if (host->vcc_aux)
			mmc->caps |= MMC_CAP_UHS_SDR12 | MMC_CAP_UHS_SDR25
				| MMC_CAP_UHS_DDR50;
	}

	mmc->ocr_avail = mmc_slot(host).ocr_mask;

	/* Request IRQ for card detect */
	if ((mmc_slot(host).card_detect_irq)) {
		ret = request_threaded_irq(mmc_slot(host).card_detect_irq,
					   NULL,
					   omap_hsmmc_detect,
					   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					   mmc_hostname(mmc), host);
		if (ret) {
			dev_err(mmc_dev(host->mmc),
				"Unable to grab MMC CD IRQ\n");
			goto err_irq_cd;
		}
		pdata->suspend = omap_hsmmc_suspend_cdirq;
		pdata->resume = omap_hsmmc_resume_cdirq;
	}

	omap_hsmmc_disable_irq(host);

	host->pinctrl = devm_pinctrl_get(host->dev);
	if (IS_ERR(host->pinctrl)) {
		dev_warn(host->dev,
			 "pins are not configured from the driver\n");
		host->pinctrl = NULL;
	}
	host->pinctrl_muxpu = pinctrl_lookup_state(host->pinctrl, "muxpu");
	if (IS_ERR(host->pinctrl_muxpu)) {
		dev_vdbg(host->dev, "optional: pinctrl_muxpu lookup error");
		host->pinctrl_muxpu = NULL;
	}
	host->pinctrl_muxpd = pinctrl_lookup_state(host->pinctrl, "muxpd");
	if (IS_ERR(host->pinctrl_muxpd)) {
		dev_vdbg(host->dev, "optional: pinctrl_muxpd lookup error");
		host->pinctrl_muxpd = NULL;
	}

	omap_hsmmc_protect_card(host);

	mmc_add_host(mmc);

	if (mmc_slot(host).name != NULL) {
		ret = device_create_file(&mmc->class_dev, &dev_attr_slot_name);
		if (ret < 0)
			goto err_slot_name;
	}
	if (mmc_slot(host).card_detect_irq && mmc_slot(host).get_cover_state) {
		ret = device_create_file(&mmc->class_dev,
					&dev_attr_cover_switch);
		if (ret < 0)
			goto err_slot_name;
	}

	omap_hsmmc_debugfs(mmc);
	pm_runtime_mark_last_busy(host->dev);
	pm_runtime_put_autosuspend(host->dev);

	return 0;

err_slot_name:
	mmc_remove_host(mmc);
	free_irq(mmc_slot(host).card_detect_irq, host);
err_irq_cd:
	omap_hsmmc_reg_put(host);
	if (host->pdata->cleanup)
		host->pdata->cleanup(&pdev->dev);
err_irq_cd_init:
	free_irq(host->irq, host);
err_irq:
	if (host->tx_chan)
		dma_release_channel(host->tx_chan);
	if (host->rx_chan)
		dma_release_channel(host->rx_chan);
	pm_runtime_put_sync(host->dev);
	pm_runtime_disable(host->dev);
	clk_put(host->fclk);
	if (host->dbclk) {
		clk_disable_unprepare(host->dbclk);
		clk_put(host->dbclk);
	}
err1:
	iounmap(host->base);
	mmc_free_host(mmc);
err_alloc:
	omap_hsmmc_gpio_free(pdata);
err:
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res)
		release_mem_region(res->start, resource_size(res));
	return ret;
}

static int omap_hsmmc_remove(struct platform_device *pdev)
{
	struct omap_hsmmc_host *host = platform_get_drvdata(pdev);
	struct resource *res;

	pm_runtime_get_sync(host->dev);
	mmc_remove_host(host->mmc);
	omap_hsmmc_reg_put(host);
	if (host->pdata->cleanup)
		host->pdata->cleanup(&pdev->dev);
	free_irq(host->irq, host);
	if (mmc_slot(host).card_detect_irq)
		free_irq(mmc_slot(host).card_detect_irq, host);

	if (host->tx_chan)
		dma_release_channel(host->tx_chan);
	if (host->rx_chan)
		dma_release_channel(host->rx_chan);

	pm_runtime_put_sync(host->dev);
	pm_runtime_disable(host->dev);
	clk_put(host->fclk);
	if (host->dbclk) {
		clk_disable_unprepare(host->dbclk);
		clk_put(host->dbclk);
	}

	omap_hsmmc_gpio_free(host->pdata);
	iounmap(host->base);
	mmc_free_host(host->mmc);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res)
		release_mem_region(res->start, resource_size(res));

	return 0;
}

#ifdef CONFIG_PM
static int omap_hsmmc_prepare(struct device *dev)
{
	struct omap_hsmmc_host *host = dev_get_drvdata(dev);

	if (host->pdata->suspend)
		return host->pdata->suspend(dev, host->slot_id);

	return 0;
}

static void omap_hsmmc_complete(struct device *dev)
{
	struct omap_hsmmc_host *host = dev_get_drvdata(dev);

	if (host->pdata->resume)
		host->pdata->resume(dev, host->slot_id);

}

static int omap_hsmmc_suspend(struct device *dev)
{
	int ret = 0;
	struct omap_hsmmc_host *host = dev_get_drvdata(dev);

	if (!host)
		return 0;

	if (host && host->suspended)
		return 0;

	pm_runtime_get_sync(host->dev);
	host->suspended = 1;
	ret = mmc_suspend_host(host->mmc);

	if (ret) {
		host->suspended = 0;
		goto err;
	}

	if (!(host->mmc->pm_flags & MMC_PM_KEEP_POWER)) {
		omap_hsmmc_disable_irq(host);
		OMAP_HSMMC_WRITE(host->base, HCTL,
				OMAP_HSMMC_READ(host->base, HCTL) & ~SDBP);
	}

	if (host->dbclk)
		clk_disable_unprepare(host->dbclk);
err:
	pm_runtime_put_sync(host->dev);

	/* Select sleep pin state */
	pinctrl_pm_select_sleep_state(host->dev);

	return ret;
}

/* Routine to resume the MMC device */
static int omap_hsmmc_resume(struct device *dev)
{
	int ret = 0;
	struct omap_hsmmc_host *host = dev_get_drvdata(dev);

	if (!host)
		return 0;

	if (host && !host->suspended)
		return 0;

	/* Select default pin state */
	pinctrl_pm_select_default_state(host->dev);

	pm_runtime_get_sync(host->dev);

	if (host->dbclk)
		clk_prepare_enable(host->dbclk);

	if (!(host->mmc->pm_flags & MMC_PM_KEEP_POWER))
		omap_hsmmc_conf_bus_power(host);

	omap_hsmmc_protect_card(host);

	/* Notify the core to resume the host */
	ret = mmc_resume_host(host->mmc);
	if (ret == 0)
		host->suspended = 0;

	pm_runtime_mark_last_busy(host->dev);
	pm_runtime_put_autosuspend(host->dev);

	return ret;

}

#else
#define omap_hsmmc_prepare	NULL
#define omap_hsmmc_complete	NULL
#define omap_hsmmc_suspend	NULL
#define omap_hsmmc_resume	NULL
#endif

static int omap_hsmmc_runtime_suspend(struct device *dev)
{
	struct omap_hsmmc_host *host;

	host = platform_get_drvdata(to_platform_device(dev));
	omap_hsmmc_context_save(host);
	dev_dbg(dev, "disabled\n");

	/* Optionally let pins go into idle state */
	pinctrl_pm_select_idle_state(host->dev);

	return 0;
}

static int omap_hsmmc_runtime_resume(struct device *dev)
{
	struct omap_hsmmc_host *host;

	host = platform_get_drvdata(to_platform_device(dev));

	/* go to the default state */
	pinctrl_pm_select_default_state(host->dev);

	omap_hsmmc_context_restore(host);
	dev_dbg(dev, "enabled\n");

	return 0;
}

static struct dev_pm_ops omap_hsmmc_dev_pm_ops = {
	.suspend	= omap_hsmmc_suspend,
	.resume		= omap_hsmmc_resume,
	.prepare	= omap_hsmmc_prepare,
	.complete	= omap_hsmmc_complete,
	.runtime_suspend = omap_hsmmc_runtime_suspend,
	.runtime_resume = omap_hsmmc_runtime_resume,
};

static struct platform_driver omap_hsmmc_driver = {
	.probe		= omap_hsmmc_probe,
	.remove		= omap_hsmmc_remove,
	.driver		= {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.pm = &omap_hsmmc_dev_pm_ops,
		.of_match_table = of_match_ptr(omap_mmc_of_match),
	},
};

module_platform_driver(omap_hsmmc_driver);
MODULE_DESCRIPTION("OMAP High Speed Multimedia Card driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_AUTHOR("Texas Instruments Inc");
