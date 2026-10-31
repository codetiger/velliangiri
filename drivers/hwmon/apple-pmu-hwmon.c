// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dialog D1815 PMU temperature sensors
 *
 * The D1815 has a 12-bit ADC behind a channel multiplexer. A conversion is
 * started by writing the channel and bit 4 to 0x30 (with 0x10 in 0x33);
 * the result appears in 0x31 (bits 3:0) and 0x32 (bits 11:4) and the PMU
 * raises event 13 when it is ready.
 *
 * Channels 11..14 measure thermistors through a 50 uA source with a 2.5 V
 * full scale, so R = raw * 50000 / 4096 ohm; they are 10 kOhm NTCs and the
 * temperature comes from the resistance table below. Channel 8 is the
 * PMU's own junction temperature: T = 26 C + 0.32 C * (cal - raw), with
 * the calibration value in 0x43 (bits 11:4) and 0x44 (bits 3:0).
 *
 * The board names its thermistors in the "apple,thermistor-labels"
 * property; temp1..temp4 are the thermistors and temp5 the junction.
 */

#include <linux/apple-pmu-i2c.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define D1815_ADC_CTRL		0x30
#define  D1815_ADC_START	BIT(4)
#define  D1815_ADC_BIAS		BIT(6)	/* set by the stock code for thermistors */
#define D1815_ADC_RESULT	0x31	/* 0x31 bits 3:0, 0x32 bits 11:4 */
#define D1815_ADC_MUX		0x33
#define  D1815_ADC_MUX_NORMAL	0x10
#define D1815_ADC_CAL		0x43	/* junction calibration, 0x43..0x44 */

#define D1815_CH_TJUNC		8
#define D1815_CH_TDEV1		11
#define D1815_NTC		4
#define D1815_TJ_CAL_DEFAULT	0x425

/* 10 kOhm NTC: resistance in ohm from -40 C in steps of 5 C */
static const u32 apple_pmu_ntc[] = {
	188500, 144290, 111330, 86560, 67790, 53460, 42450, 33930, 27280,
	22070, 17960, 14700, 12090, 10000, 8310, 6940, 5830, 4910, 4160,
	3540, 3020, 2590, 2230, 1920, 1670, 1450, 1270, 1110, 980, 860,
	760, 670, 600, 530,
};
#define NTC_T0		(-40000)
#define NTC_STEP	5000

struct apple_pmu_hwmon {
	struct regmap *regmap;
	struct mutex lock;		/* one conversion at a time */
	struct completion done;
	bool irq;
	u16 tj_cal;
	const char *label[D1815_NTC + 1];
};

static irqreturn_t apple_pmu_hwmon_done(int irq, void *data)
{
	struct apple_pmu_hwmon *h = data;

	complete(&h->done);
	return IRQ_HANDLED;
}

static int apple_pmu_hwmon_convert(struct apple_pmu_hwmon *h, unsigned int ch,
				   unsigned int *raw)
{
	unsigned int ctrl = ch;
	u8 res[2];
	int ret;

	if (ch >= 10)
		ctrl |= D1815_ADC_BIAS;

	mutex_lock(&h->lock);
	reinit_completion(&h->done);
	ret = regmap_write(h->regmap, D1815_ADC_MUX, D1815_ADC_MUX_NORMAL);
	if (!ret)
		ret = regmap_write(h->regmap, D1815_ADC_CTRL, ctrl | D1815_ADC_START);
	if (ret)
		goto out;

	if (h->irq) {
		if (!wait_for_completion_timeout(&h->done, msecs_to_jiffies(20)))
			ret = -ETIMEDOUT;
	} else {
		usleep_range(3000, 4000);
	}
	if (!ret)
		ret = regmap_bulk_read(h->regmap, D1815_ADC_RESULT, res, sizeof(res));
	if (!ret)
		*raw = (res[0] & 0xf) | res[1] << 4;
out:
	regmap_write(h->regmap, D1815_ADC_CTRL, ch);
	mutex_unlock(&h->lock);
	return ret;
}

static long apple_pmu_ntc_temp(unsigned int raw)
{
	u32 r = raw * 50000 / 4096;
	int i;

	if (r >= apple_pmu_ntc[0])
		return NTC_T0;
	for (i = 1; i < ARRAY_SIZE(apple_pmu_ntc); i++) {
		u32 r1 = apple_pmu_ntc[i - 1], r2 = apple_pmu_ntc[i];

		if (r >= r2)
			return NTC_T0 + (i - 1) * NTC_STEP +
			       (long)(r1 - r) * NTC_STEP / (r1 - r2);
	}
	return NTC_T0 + (ARRAY_SIZE(apple_pmu_ntc) - 1) * NTC_STEP;
}

static int apple_pmu_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
				u32 attr, int channel, long *val)
{
	struct apple_pmu_hwmon *h = dev_get_drvdata(dev);
	unsigned int raw;
	int ret;

	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;

	if (channel < D1815_NTC) {
		ret = apple_pmu_hwmon_convert(h, D1815_CH_TDEV1 + channel, &raw);
		if (ret)
			return ret;
		*val = apple_pmu_ntc_temp(raw);
	} else {
		ret = apple_pmu_hwmon_convert(h, D1815_CH_TJUNC, &raw);
		if (ret)
			return ret;
		*val = 26000 + 320 * ((long)h->tj_cal - (long)raw);
	}
	return 0;
}

static int apple_pmu_hwmon_read_string(struct device *dev,
				       enum hwmon_sensor_types type, u32 attr,
				       int channel, const char **str)
{
	struct apple_pmu_hwmon *h = dev_get_drvdata(dev);

	*str = h->label[channel];
	return 0;
}

static umode_t apple_pmu_hwmon_is_visible(const void *data,
					  enum hwmon_sensor_types type,
					  u32 attr, int channel)
{
	const struct apple_pmu_hwmon *h = data;

	if (attr == hwmon_temp_label)
		return h->label[channel] ? 0444 : 0;
	return 0444;
}

static const struct hwmon_channel_info * const apple_pmu_hwmon_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_ops apple_pmu_hwmon_ops = {
	.is_visible	= apple_pmu_hwmon_is_visible,
	.read		= apple_pmu_hwmon_read,
	.read_string	= apple_pmu_hwmon_read_string,
};

static const struct hwmon_chip_info apple_pmu_hwmon_chip = {
	.ops	= &apple_pmu_hwmon_ops,
	.info	= apple_pmu_hwmon_info,
};

static int apple_pmu_hwmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_pmu *pmu = dev_get_drvdata(dev->parent);
	struct apple_pmu_hwmon *h;
	struct device *hwmon;
	u8 cal[2];
	int irq, ret;

	if (!pmu || !pmu->regmap)
		return -EPROBE_DEFER;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->regmap = pmu->regmap;
	ret = devm_mutex_init(dev, &h->lock);
	if (ret)
		return ret;
	init_completion(&h->done);

	ret = regmap_bulk_read(h->regmap, D1815_ADC_CAL, cal, sizeof(cal));
	if (ret)
		return ret;
	h->tj_cal = cal[0] << 4 | (cal[1] & 0xf);
	if (!(h->tj_cal & 0xfff0))
		h->tj_cal = D1815_TJ_CAL_DEFAULT;

	of_property_read_string_array(dev->of_node, "apple,thermistor-labels",
				      h->label, D1815_NTC);
	h->label[D1815_NTC] = "PMU junction";

	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		ret = devm_request_threaded_irq(dev, irq, NULL, apple_pmu_hwmon_done,
						IRQF_ONESHOT, "pmu-adc", h);
		if (ret)
			return dev_err_probe(dev, ret, "cannot request ADC interrupt\n");
		h->irq = true;
	}

	hwmon = devm_hwmon_device_register_with_info(dev, "apple_pmu", h,
						     &apple_pmu_hwmon_chip, NULL);
	return PTR_ERR_OR_ZERO(hwmon);
}

static const struct of_device_id apple_pmu_hwmon_of_match[] = {
	{ .compatible = "apple,pmu-d1815-adc" },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_pmu_hwmon_of_match);

static struct platform_driver apple_pmu_hwmon_driver = {
	.driver = {
		.name		= "apple-pmu-hwmon",
		.of_match_table	= apple_pmu_hwmon_of_match,
	},
	.probe = apple_pmu_hwmon_probe,
};
module_platform_driver(apple_pmu_hwmon_driver);

MODULE_DESCRIPTION("Dialog D1815 PMU temperature sensors");
MODULE_LICENSE("GPL");
