/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Dialog D1815 / D1974 / D2045 I2C PMU MFD shared definitions
 */

#ifndef __LINUX_APPLE_PMU_I2C_H__
#define __LINUX_APPLE_PMU_I2C_H__

#include <linux/regmap.h>

struct apple_pmu {
	struct device *dev;
	struct regmap *regmap;
};

#endif /* __LINUX_APPLE_PMU_I2C_H__ */
