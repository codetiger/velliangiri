// SPDX-License-Identifier: GPL-2.0-only
/*
 * /dev/watchdog driver for S5L8940X / S5L8947X hardware watchdog timer.
 */

#define pr_fmt(fmt) "apple-wdt: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/watchdog.h>

#include "s5l8947x.h"

#define APPLE_WDT_HZ		24000000U
#define APPLE_WDT_MAX_SECS	170U
#define APPLE_WDT_DEFAULT_SECS	60U

static int apple_wdt_ping(struct watchdog_device *wdd)
{
	apple_s5l_wdt_rebase_and_arm(wdd->timeout * APPLE_WDT_HZ);
	return 0;
}

static int apple_wdt_start(struct watchdog_device *wdd)
{
	apple_wdt_ping(wdd);
	apple_s5l_wdt_reset_enable(true);
	return 0;
}

static int apple_wdt_stop(struct watchdog_device *wdd)
{
	apple_s5l_wdt_reset_enable(false);
	return 0;
}

static int apple_wdt_set_timeout(struct watchdog_device *wdd, unsigned int secs)
{
	wdd->timeout = secs;
	if (watchdog_active(wdd))
		apple_wdt_ping(wdd);
	return 0;
}

static const struct watchdog_info apple_wdt_info = {
	.identity	= "S5L8947X Watchdog",
	.options	= WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops apple_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= apple_wdt_start,
	.stop		= apple_wdt_stop,
	.ping		= apple_wdt_ping,
	.set_timeout	= apple_wdt_set_timeout,
};

static struct watchdog_device apple_wdd = {
	.info		= &apple_wdt_info,
	.ops		= &apple_wdt_ops,
	.min_timeout	= 1,
	.max_timeout	= APPLE_WDT_MAX_SECS,
	.timeout	= APPLE_WDT_DEFAULT_SECS,
};

static int __init apple_wdt_dev_init(void)
{
	int ret;

	if (!apple_s5l_wdt_available()) {
		pr_err("no counter mapping, skipping /dev/watchdog\n");
		return -ENODEV;
	}

	watchdog_set_nowayout(&apple_wdd, WATCHDOG_NOWAYOUT);
	watchdog_stop_on_reboot(&apple_wdd);

	/*
	 * The pre-kernel entry stub arms the hardware watchdog before
	 * jumping into Linux. Rebase to the default 60s timeout now and
	 * mark WDOG_HW_RUNNING so the watchdog core feeds it via its
	 * boot-enabled keepalive worker until userspace opens /dev/watchdog.
	 */
	apple_wdt_start(&apple_wdd);
	set_bit(WDOG_HW_RUNNING, &apple_wdd.status);

	ret = watchdog_register_device(&apple_wdd);
	if (ret) {
		pr_err("could not register /dev/watchdog: %d\n", ret);
		return ret;
	}

	pr_info("/dev/watchdog registered (timeout %u s, max %u s)\n",
		apple_wdd.timeout, apple_wdd.max_timeout);
	return 0;
}
late_initcall(apple_wdt_dev_init);
