// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dialog D1815 PMU real-time clock
 *
 * The D1815 counts seconds in a 32-bit register at 0x4c..0x4f from the
 * moment the PMU is powered. The count survives reboots and standby but
 * restarts from zero when the board loses power (S5L8947X boards such as
 * j33iap have no backup battery), and it cannot be written.
 *
 * Wall time is kept as an offset from that count in PMU scratch registers:
 * whole seconds at 0x84..0x87 and the fraction of a second in units of
 * 1/32768 s at 0x95..0x96. Linux keeps the fraction at zero. An offset of
 * zero means the time has never been set since power-up, and the clock
 * reports itself invalid until it is set.
 *
 * The alarm compares the count with 0x46..0x49; bit 6 of 0x4a arms it and
 * it raises PMU event 32 when it fires.
 */

#include <linux/apple-pmu-i2c.h>
#include <linux/bits.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/unaligned.h>

#define D1815_RTC_ALARM		0x46	/* 32-bit LE, compared with the count */
#define D1815_RTC_CTRL		0x4a
#define  D1815_RTC_CTRL_ON	BIT(0)
#define  D1815_RTC_CTRL_ALARM	BIT(6)
#define D1815_RTC_COUNT		0x4c	/* 32-bit LE seconds since power-up */
#define D1815_RTC_OFFSET	0x84	/* 32-bit LE whole seconds */
#define D1815_RTC_OFFSET_FRAC	0x95	/* 15 bits, 1/32768 s */

struct apple_pmu_rtc {
	struct regmap *regmap;
	struct rtc_device *rtc;
};

static int apple_pmu_rtc_read_u32(struct regmap *map, unsigned int reg, u32 *val)
{
	u8 buf[4];
	int ret;

	ret = regmap_bulk_read(map, reg, buf, sizeof(buf));
	if (!ret)
		*val = get_unaligned_le32(buf);
	return ret;
}

static int apple_pmu_rtc_write_u32(struct regmap *map, unsigned int reg, u32 val)
{
	u8 buf[4];

	put_unaligned_le32(val, buf);
	return regmap_bulk_write(map, reg, buf, sizeof(buf));
}

/* The count can carry between bytes during a read: read until two agree. */
static int apple_pmu_rtc_count(struct apple_pmu_rtc *r, u32 *count)
{
	u32 a, b;
	int i, ret;

	ret = apple_pmu_rtc_read_u32(r->regmap, D1815_RTC_COUNT, &a);
	for (i = 0; !ret && i < 5; i++) {
		ret = apple_pmu_rtc_read_u32(r->regmap, D1815_RTC_COUNT, &b);
		if (!ret && a == b) {
			*count = a;
			return 0;
		}
		a = b;
	}
	return ret ?: -EIO;
}

static int apple_pmu_rtc_offset(struct apple_pmu_rtc *r, u32 *offset)
{
	int ret;

	ret = apple_pmu_rtc_read_u32(r->regmap, D1815_RTC_OFFSET, offset);
	if (!ret && !*offset)
		return -EINVAL;		/* not set since power-up */
	return ret;
}

static int apple_pmu_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct apple_pmu_rtc *r = dev_get_drvdata(dev);
	u32 count, offset;
	int ret;

	ret = apple_pmu_rtc_offset(r, &offset);
	if (ret)
		return ret;
	ret = apple_pmu_rtc_count(r, &count);
	if (ret)
		return ret;
	rtc_time64_to_tm((time64_t)count + offset, tm);
	return 0;
}

static int apple_pmu_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct apple_pmu_rtc *r = dev_get_drvdata(dev);
	time64_t t = rtc_tm_to_time64(tm);
	u8 frac[2] = { 0, 0 };
	u32 count;
	int ret;

	ret = apple_pmu_rtc_count(r, &count);
	if (ret)
		return ret;
	if (t <= count || t - count > U32_MAX)
		return -ERANGE;
	ret = regmap_bulk_write(r->regmap, D1815_RTC_OFFSET_FRAC, frac, sizeof(frac));
	if (ret)
		return ret;
	return apple_pmu_rtc_write_u32(r->regmap, D1815_RTC_OFFSET, t - count);
}

static int apple_pmu_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct apple_pmu_rtc *r = dev_get_drvdata(dev);
	unsigned int ctrl;
	u32 alarm, offset;
	int ret;

	ret = apple_pmu_rtc_offset(r, &offset);
	if (ret)
		return ret;
	ret = apple_pmu_rtc_read_u32(r->regmap, D1815_RTC_ALARM, &alarm);
	if (ret)
		return ret;
	ret = regmap_read(r->regmap, D1815_RTC_CTRL, &ctrl);
	if (ret)
		return ret;
	rtc_time64_to_tm((time64_t)alarm + offset, &alrm->time);
	alrm->enabled = !!(ctrl & D1815_RTC_CTRL_ALARM);
	return 0;
}

static int apple_pmu_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct apple_pmu_rtc *r = dev_get_drvdata(dev);

	if (enabled)
		return regmap_set_bits(r->regmap, D1815_RTC_CTRL,
				       D1815_RTC_CTRL_ALARM | D1815_RTC_CTRL_ON);
	return regmap_clear_bits(r->regmap, D1815_RTC_CTRL, D1815_RTC_CTRL_ALARM);
}

static int apple_pmu_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct apple_pmu_rtc *r = dev_get_drvdata(dev);
	time64_t t = rtc_tm_to_time64(&alrm->time);
	u32 offset;
	int ret;

	ret = apple_pmu_rtc_offset(r, &offset);
	if (ret)
		return ret;
	if (t < offset || t - offset > U32_MAX)
		return -ERANGE;
	ret = apple_pmu_rtc_alarm_irq_enable(dev, 0);
	if (ret)
		return ret;
	ret = apple_pmu_rtc_write_u32(r->regmap, D1815_RTC_ALARM, t - offset);
	if (ret || !alrm->enabled)
		return ret;
	return apple_pmu_rtc_alarm_irq_enable(dev, 1);
}

static const struct rtc_class_ops apple_pmu_rtc_ops = {
	.read_time		= apple_pmu_rtc_read_time,
	.set_time		= apple_pmu_rtc_set_time,
	.read_alarm		= apple_pmu_rtc_read_alarm,
	.set_alarm		= apple_pmu_rtc_set_alarm,
	.alarm_irq_enable	= apple_pmu_rtc_alarm_irq_enable,
};

static irqreturn_t apple_pmu_rtc_alarm_irq(int irq, void *data)
{
	struct apple_pmu_rtc *r = data;

	/* One shot: disarm until the next alarm is set */
	regmap_clear_bits(r->regmap, D1815_RTC_CTRL, D1815_RTC_CTRL_ALARM);
	rtc_update_irq(r->rtc, 1, RTC_IRQF | RTC_AF);
	return IRQ_HANDLED;
}

static int apple_pmu_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_pmu *pmu = dev_get_drvdata(dev->parent);
	struct apple_pmu_rtc *r;
	int irq, ret;

	if (!pmu || !pmu->regmap)
		return -EPROBE_DEFER;

	r = devm_kzalloc(dev, sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	r->regmap = pmu->regmap;
	platform_set_drvdata(pdev, r);

	r->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(r->rtc))
		return PTR_ERR(r->rtc);
	r->rtc->ops = &apple_pmu_rtc_ops;
	r->rtc->range_min = 0;
	r->rtc->range_max = U32_MAX;

	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		ret = devm_request_threaded_irq(dev, irq, NULL,
						apple_pmu_rtc_alarm_irq,
						IRQF_ONESHOT, "rtc-alarm", r);
		if (ret)
			return dev_err_probe(dev, ret, "cannot request alarm interrupt\n");
		device_init_wakeup(dev, true);
	} else {
		clear_bit(RTC_FEATURE_ALARM, r->rtc->features);
	}

	return devm_rtc_register_device(r->rtc);
}

static const struct of_device_id apple_pmu_rtc_of_match[] = {
	{ .compatible = "apple,pmu-d1815-rtc" },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_pmu_rtc_of_match);

static struct platform_driver apple_pmu_rtc_driver = {
	.driver = {
		.name		= "apple-pmu-rtc",
		.of_match_table	= apple_pmu_rtc_of_match,
	},
	.probe = apple_pmu_rtc_probe,
};
module_platform_driver(apple_pmu_rtc_driver);

MODULE_DESCRIPTION("Dialog D1815 PMU real-time clock");
MODULE_LICENSE("GPL");
