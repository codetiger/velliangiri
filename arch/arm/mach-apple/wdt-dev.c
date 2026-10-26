// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X /dev/watchdog character device driver.
 *
 * Exposes standard Linux /dev/watchdog (misc minor 130) backed by the
 * hardware watchdog rebase helpers in wdt-clocksource.c.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>

#define S5L_WDT_DEFAULT_TIMEOUT	60U
#define S5L_WDT_MIN_TIMEOUT	5U
#define S5L_WDT_MAX_TIMEOUT	170U

extern void apple_s5l_wdt_takeover(u32 timeout_seconds);
extern void apple_s5l_wdt_feed(void);

static unsigned int wdt_timeout = S5L_WDT_DEFAULT_TIMEOUT;
static unsigned long wdt_is_open;

static int s5l_wdt_open(struct inode *inode, struct file *file)
{
	if (test_and_set_bit(0, &wdt_is_open))
		return -EBUSY;

	apple_s5l_wdt_takeover(wdt_timeout);
	pr_info("s5l8947x-wdt: /dev/watchdog opened, hardware bite set to %u s\n",
		wdt_timeout);
	return stream_open(inode, file);
}

static int s5l_wdt_release(struct inode *inode, struct file *file)
{
	/*
	 * Intentionally keep the hardware watchdog armed on close (nowayout)
	 * so a crashed watchdog daemon triggers a clean hardware reset.
	 */
	apple_s5l_wdt_feed();
	clear_bit(0, &wdt_is_open);
	pr_warn("s5l8947x-wdt: /dev/watchdog closed; hardware timer remains armed (%u s)\n",
		wdt_timeout);
	return 0;
}

static ssize_t s5l_wdt_write(struct file *file, const char __user *data,
			     size_t len, loff_t *ppos)
{
	if (len)
		apple_s5l_wdt_feed();
	return len;
}

static const struct watchdog_info s5l_wdt_ident = {
	.options		= WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING,
	.firmware_version	= 1,
	.identity		= "s5l8947x-wdt",
};

static long s5l_wdt_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int __user *p = argp;
	int new_timeout;

	switch (cmd) {
	case WDIOC_GETSUPPORT:
		return copy_to_user(argp, &s5l_wdt_ident,
				    sizeof(s5l_wdt_ident)) ? -EFAULT : 0;
	case WDIOC_GETSTATUS:
	case WDIOC_GETBOOTSTATUS:
		return put_user(0, p);
	case WDIOC_KEEPALIVE:
		apple_s5l_wdt_feed();
		return 0;
	case WDIOC_SETTIMEOUT:
		if (get_user(new_timeout, p))
			return -EFAULT;
		if (new_timeout < (int)S5L_WDT_MIN_TIMEOUT)
			new_timeout = S5L_WDT_MIN_TIMEOUT;
		if (new_timeout > (int)S5L_WDT_MAX_TIMEOUT)
			new_timeout = S5L_WDT_MAX_TIMEOUT;
		wdt_timeout = (unsigned int)new_timeout;
		apple_s5l_wdt_takeover(wdt_timeout);
		return put_user((int)wdt_timeout, p);
	case WDIOC_GETTIMEOUT:
		return put_user((int)wdt_timeout, p);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations s5l_wdt_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.write		= s5l_wdt_write,
	.unlocked_ioctl	= s5l_wdt_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.open		= s5l_wdt_open,
	.release	= s5l_wdt_release,
};

static struct miscdevice s5l_wdt_miscdev = {
	.minor	= WATCHDOG_MINOR,
	.name	= "watchdog",
	.fops	= &s5l_wdt_fops,
};

static int __init s5l_wdt_dev_init(void)
{
	int ret = misc_register(&s5l_wdt_miscdev);

	if (ret)
		pr_err("s5l8947x-wdt: failed to register /dev/watchdog: %d\n", ret);
	else
		pr_info("s5l8947x-wdt: /dev/watchdog registered (default %u s)\n",
			wdt_timeout);
	return ret;
}
device_initcall(s5l_wdt_dev_init);
