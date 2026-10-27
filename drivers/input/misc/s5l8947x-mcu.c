// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X On-Board IR / System Management MCU (UART1 @ 0x32600000) and
 * Broadcom BCM4334 Wireless Combo Core (UART2 @ 0x32700000 + HSIC) Driver
 *
 * 1. On-Board IR / System MCU (mcu0 @ 0x32600000, PMGR gate 61):
 *    - Initializes UART1 directly via MMIO for 250000 baud 8N1 (24 MHz / 16 / 6,
 *      UBRDIV = 5) with RX/TX FIFOs enabled.
 *    - Pulses UART1 UCON bit 4 (SBREAK) for 80 ms to synchronize the MCU.
 *    - Implements the 6-byte "St" packet framing with CRC-16/CCITT-FALSE
 *      (init = 0xffff, poly = 0x1021):
 *        'S' (0x53), 't' (0x74), cmd, 0x01, len_hi, len_lo, payload..., crc_hi, crc_lo
 *    - Queries MCU firmware version (cmd = 0x02) and sets system/LED active
 *      state (cmd = 0x04, mode = 0x02), polling RX FIFO for NEC IR events.
 *
 * 2. BCM4334 Wireless Combo Controller (wireless @ 0x32700000, PMGR gates 32/62/78):
 *    - Enables Dialog D1815 PMU wlan_reg_on (reg 0x57 bit 1) power switch.
 *    - Enables PMGR gates 32 (USB20-HOST), 78 (HSIC0PHY), and 62 (UART2).
 *    - Initializes UART2 (0x32700000) for 115200 baud 8N1 (UBRDIV = 12) and
 *      probes the BCM4334 Bluetooth 4.0 core with an H4 HCI_Reset (01 03 0c 00).
 */

#define pr_fmt(fmt) "s5l8947x-mcu: " fmt

#include <linux/delay.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define PMGR_PHYS_BASE		0x3f100000UL
#define PMGR_GATE_OFFSET(n)	(0x1008U + (n) * 4U)
#define PMGR_GATE_GPIO		28
#define PMGR_GATE_USB20_HOST	32
#define PMGR_GATE_UART1		61
#define PMGR_GATE_UART2		62
#define PMGR_GATE_HSIC0PHY	78

/* S5L8947X UART register offsets (0x32500000 / 0x32600000 / 0x32700000) */
#define S5L_UART_ULCON		0x00
#define S5L_UART_UCON		0x04
#define S5L_UART_UFCON		0x08
#define S5L_UART_UMCON		0x0c
#define S5L_UART_UTRSTAT	0x10
#define S5L_UART_UERSTAT	0x14
#define S5L_UART_UFSTAT		0x18
#define S5L_UART_UTXH		0x20
#define S5L_UART_URXH		0x24
#define S5L_UART_UBRDIV		0x28
#define S5L_UART_UFRACVAL	0x2c

#define S5L_ULCON_8N1		0x03U
#define S5L_UCON_POLL_RXTX	0x05U
#define S5L_UCON_SBREAK		BIT(4)
#define S5L_UFCON_FIFO_RESET	0x07U
#define S5L_UFCON_FIFO_EN	0x01U
#define S5L_UTRSTAT_RXDR	BIT(0)
#define S5L_UTRSTAT_TXFE	BIT(1)
#define S5L_UTRSTAT_TXE		BIT(2)
#define S5L_UFSTAT_RX_COUNT	0x0fU
#define S5L_UFSTAT_RX_FULL	BIT(8)
#define S5L_UFSTAT_TX_FULL	BIT(9)

/* 24 MHz osc24m / (16 * (UBRDIV + 1)) */
#define S5L_UBRDIV_250000	5U
#define S5L_UBRDIV_115200	12U

/* MCU "St" protocol constants */
#define MCU_SYNC_BYTE0		0x53U /* 'S' */
#define MCU_SYNC_BYTE1		0x74U /* 't' */
#define MCU_PROTO_VER		0x01U
#define MCU_HEADER_LEN		6U
#define MCU_CRC_LEN		2U
#define MCU_MAX_PAYLOAD		32U
#define MCU_MAX_FRAME		(MCU_HEADER_LEN + MCU_MAX_PAYLOAD + MCU_CRC_LEN)

#define MCU_CMD_PING_VERSION	0x02U
#define MCU_CMD_SET_STATE	0x04U

#define MCU_STATE_ACTIVE	0x02U
#define MCU_STATE_STANDBY	0x03U

#define MCU_RSP_ACK		0x80U
#define MCU_RSP_IR_EVENT1	0x81U
#define MCU_RSP_IR_EVENT2	0x82U
#define MCU_RSP_IR_EVENT3	0x83U
#define MCU_RSP_BUSY		0x84U
#define MCU_RSP_IR_EVENT5	0x85U
#define MCU_RSP_EVENT_RAW	0x9fU

enum mcu_rx_state {
	MCU_RX_WAIT_S = 0,
	MCU_RX_WAIT_T,
	MCU_RX_HEADER,
	MCU_RX_PAYLOAD_CRC,
};

struct s5l8947x_mcu {
	struct device *dev;
	void __iomem *regs;
	void __iomem *pmgr;
	struct input_dev *input;
	struct led_classdev led;
	struct notifier_block reboot_nb;
	struct delayed_work poll_work;
	struct mutex lock;

	enum mcu_rx_state rx_state;
	u8 rx_frame[MCU_MAX_FRAME];
	size_t rx_pos;
	size_t rx_expected;

	u8 rsp_cmd;
	u8 rsp_buf[MCU_MAX_PAYLOAD + 1];
	size_t rsp_len;

	unsigned long rx_count;
	u8 last_cmd;
	u8 last_payload[MCU_MAX_PAYLOAD];
	size_t last_len;
	char fw_version[MCU_MAX_PAYLOAD + 1];
	bool hw_ready;
	bool init_done;
};

static int s5l8947x_pmgr_enable_gate(void __iomem *pmgr, unsigned int gate)
{
	u32 off = PMGR_GATE_OFFSET(gate);
	u32 v;
	int i;

	if (!pmgr)
		return -ENODEV;

	v = readl(pmgr + off);
	if (!v)
		return -ENODEV;
	writel((v & ~0x10fU) | 0x0fU, pmgr + off);
	for (i = 0; i < 10000; i++) {
		v = readl(pmgr + off);
		if (((v >> 4) & 0x0fU) == 0x0fU)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static void s5l8947x_uart_hw_init(void __iomem *regs, u32 ubrdiv)
{
	writel(S5L_ULCON_8N1, regs + S5L_UART_ULCON);
	writel(0, regs + S5L_UART_UMCON);
	writel(S5L_UFCON_FIFO_RESET, regs + S5L_UART_UFCON);
	udelay(10);
	writel(S5L_UFCON_FIFO_EN, regs + S5L_UART_UFCON);
	writel(ubrdiv, regs + S5L_UART_UBRDIV);
	writel(0, regs + S5L_UART_UFRACVAL);
	writel(S5L_UCON_POLL_RXTX, regs + S5L_UART_UCON);
	(void)readl(regs + S5L_UART_UERSTAT);
}

static bool s5l8947x_uart_rx_ready(void __iomem *regs)
{
	u32 ufstat = readl(regs + S5L_UART_UFSTAT);
	u32 utrstat = readl(regs + S5L_UART_UTRSTAT);

	return (ufstat & (S5L_UFSTAT_RX_COUNT | S5L_UFSTAT_RX_FULL)) != 0 ||
	       (utrstat & S5L_UTRSTAT_RXDR) != 0;
}

static void s5l8947x_uart_putc(void __iomem *regs, u8 ch)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (!(readl(regs + S5L_UART_UFSTAT) & S5L_UFSTAT_TX_FULL))
			break;
		udelay(1);
	}
	writel(ch, regs + S5L_UART_UTXH);
}

static void s5l8947x_uart_flush_tx(void __iomem *regs)
{
	int i;

	for (i = 0; i < 20000; i++) {
		if (readl(regs + S5L_UART_UTRSTAT) & S5L_UTRSTAT_TXE)
			break;
		udelay(1);
	}
}

static u16 s5l8947x_mcu_crc16(u16 crc, const u8 *data, size_t len)
{
	size_t i;
	u8 b, mask;
	u16 top;

	for (i = 0; i < len; i++) {
		b = data[i];
		for (mask = 0x80U; mask != 0; mask >>= 1) {
			top = crc & 0x8000U;
			if (b & mask)
				top ^= 0x8000U;
			crc <<= 1;
			if (top)
				crc ^= 0x1021U;
		}
	}
	return crc;
}

static unsigned int s5l8947x_mcu_map_ir_key(u8 code)
{
	switch (code & 0xfeU) {
	case 0x02:
		return KEY_MENU;
	case 0x04:
	case 0x5c:
		return KEY_ENTER;
	case 0x06:
		return KEY_RIGHT;
	case 0x08:
		return KEY_LEFT;
	case 0x0a:
		return KEY_UP;
	case 0x0c:
		return KEY_DOWN;
	case 0x5e:
		return KEY_PLAYPAUSE;
	default:
		return KEY_UNKNOWN;
	}
}

static void s5l8947x_mcu_handle_ir_event(struct s5l8947x_mcu *mcu, u8 cmd,
					 const u8 *payload, size_t len)
{
	unsigned int keycode = KEY_UNKNOWN;
	u32 scancode = 0;
	size_t i;

	for (i = 0; i < min_t(size_t, len, 4); i++)
		scancode = (scancode << 8) | payload[i];

	if (len >= 3 && payload[0] == 0xeeU && payload[1] == 0x87U)
		keycode = s5l8947x_mcu_map_ir_key(payload[2]);
	else if (len >= 1)
		keycode = s5l8947x_mcu_map_ir_key(payload[len - 1]);

	input_event(mcu->input, EV_MSC, MSC_SCAN, scancode);
	if (keycode != KEY_UNKNOWN) {
		input_report_key(mcu->input, keycode, 1);
		input_sync(mcu->input);
		input_report_key(mcu->input, keycode, 0);
	}
	input_sync(mcu->input);

	dev_dbg(mcu->dev, "IR event cmd=0x%02x len=%zu scan=0x%08x key=%u\n",
		cmd, len, scancode, keycode);
}

static void s5l8947x_mcu_dispatch_frame(struct s5l8947x_mcu *mcu,
					const u8 *frame, size_t total)
{
	u8 cmd = frame[2];
	size_t len = ((size_t)frame[4] << 8) | frame[5];
	const u8 *payload = &frame[MCU_HEADER_LEN];
	u16 expected_crc, rx_crc;

	if (total != MCU_HEADER_LEN + len + MCU_CRC_LEN)
		return;

	expected_crc = s5l8947x_mcu_crc16(0xffffU, frame, MCU_HEADER_LEN + len);
	rx_crc = ((u16)frame[MCU_HEADER_LEN + len] << 8) |
		 frame[MCU_HEADER_LEN + len + 1];
	if (expected_crc != rx_crc)
		return;

	mcu->rx_count++;
	mcu->last_cmd = cmd;
	mcu->last_len = min_t(size_t, len, MCU_MAX_PAYLOAD);
	if (mcu->last_len)
		memcpy(mcu->last_payload, payload, mcu->last_len);

	if (cmd == MCU_RSP_ACK || cmd == MCU_RSP_BUSY) {
		mcu->rsp_cmd = cmd;
		mcu->rsp_len = min_t(size_t, len, MCU_MAX_PAYLOAD);
		if (mcu->rsp_len)
			memcpy(mcu->rsp_buf, payload, mcu->rsp_len);
		mcu->rsp_buf[mcu->rsp_len] = '\0';
		return;
	}

	if (cmd == MCU_RSP_IR_EVENT1 || cmd == MCU_RSP_IR_EVENT2 ||
	    cmd == MCU_RSP_IR_EVENT3 || cmd == MCU_RSP_IR_EVENT5 ||
	    cmd == MCU_RSP_EVENT_RAW)
		s5l8947x_mcu_handle_ir_event(mcu, cmd, payload, len);
}

static void s5l8947x_mcu_feed_byte(struct s5l8947x_mcu *mcu, u8 b)
{
	size_t pay_len;

	switch (mcu->rx_state) {
	case MCU_RX_WAIT_S:
		if (b == MCU_SYNC_BYTE0) {
			mcu->rx_frame[0] = b;
			mcu->rx_pos = 1;
			mcu->rx_state = MCU_RX_WAIT_T;
		}
		break;

	case MCU_RX_WAIT_T:
		if (b == MCU_SYNC_BYTE1) {
			mcu->rx_frame[1] = b;
			mcu->rx_pos = 2;
			mcu->rx_state = MCU_RX_HEADER;
		} else if (b == MCU_SYNC_BYTE0) {
			mcu->rx_frame[0] = b;
			mcu->rx_pos = 1;
		} else {
			mcu->rx_state = MCU_RX_WAIT_S;
		}
		break;

	case MCU_RX_HEADER:
		mcu->rx_frame[mcu->rx_pos++] = b;
		if (mcu->rx_pos == MCU_HEADER_LEN) {
			pay_len = ((size_t)mcu->rx_frame[4] << 8) |
				  mcu->rx_frame[5];
			if (pay_len > MCU_MAX_PAYLOAD) {
				mcu->rx_state = MCU_RX_WAIT_S;
				break;
			}
			mcu->rx_expected = MCU_HEADER_LEN + pay_len +
					   MCU_CRC_LEN;
			mcu->rx_state = MCU_RX_PAYLOAD_CRC;
		}
		break;

	case MCU_RX_PAYLOAD_CRC:
		mcu->rx_frame[mcu->rx_pos++] = b;
		if (mcu->rx_pos == mcu->rx_expected) {
			s5l8947x_mcu_dispatch_frame(mcu, mcu->rx_frame,
						    mcu->rx_expected);
			mcu->rx_state = MCU_RX_WAIT_S;
		}
		break;
	}
}

static void s5l8947x_mcu_drain_rx(struct s5l8947x_mcu *mcu)
{
	int limit = 256;

	while (limit-- > 0 && s5l8947x_uart_rx_ready(mcu->regs)) {
		u8 b = readl(mcu->regs + S5L_UART_URXH) & 0xffU;

		(void)readl(mcu->regs + S5L_UART_UERSTAT);
		s5l8947x_mcu_feed_byte(mcu, b);
	}
}

static int s5l8947x_mcu_send_frame(struct s5l8947x_mcu *mcu, u8 cmd,
				   const u8 *payload, size_t len)
{
	u8 frame[MCU_MAX_FRAME];
	size_t i, total;
	u16 crc;

	if (!mcu->hw_ready)
		return -ENODEV;
	if (len > MCU_MAX_PAYLOAD)
		return -EINVAL;

	frame[0] = MCU_SYNC_BYTE0;
	frame[1] = MCU_SYNC_BYTE1;
	frame[2] = cmd;
	frame[3] = MCU_PROTO_VER;
	frame[4] = (len >> 8) & 0xffU;
	frame[5] = len & 0xffU;
	if (len && payload)
		memcpy(&frame[MCU_HEADER_LEN], payload, len);

	crc = s5l8947x_mcu_crc16(0xffffU, frame, MCU_HEADER_LEN + len);
	frame[MCU_HEADER_LEN + len] = (crc >> 8) & 0xffU;
	frame[MCU_HEADER_LEN + len + 1] = crc & 0xffU;
	total = MCU_HEADER_LEN + len + MCU_CRC_LEN;

	for (i = 0; i < total; i++)
		s5l8947x_uart_putc(mcu->regs, frame[i]);
	s5l8947x_uart_flush_tx(mcu->regs);
	return 0;
}

static int s5l8947x_mcu_transact(struct s5l8947x_mcu *mcu, u8 cmd,
				 const u8 *payload, size_t len,
				 u8 *out_buf, size_t *out_len)
{
	int attempt, wait_ms;

	for (attempt = 0; attempt < 3; attempt++) {
		mcu->rsp_cmd = 0;
		mcu->rsp_len = 0;
		s5l8947x_mcu_drain_rx(mcu);
		s5l8947x_mcu_send_frame(mcu, cmd, payload, len);

		for (wait_ms = 0; wait_ms < 120; wait_ms++) {
			s5l8947x_mcu_drain_rx(mcu);
			if (mcu->rsp_cmd != 0)
				break;
			usleep_range(900, 1100);
		}

		if (mcu->rsp_cmd == MCU_RSP_ACK) {
			if (out_buf && out_len) {
				size_t copy = min(*out_len, mcu->rsp_len);

				memcpy(out_buf, mcu->rsp_buf, copy);
				*out_len = copy;
			}
			return 0;
		}
		if (mcu->rsp_cmd == MCU_RSP_BUSY)
			msleep(40);
	}
	return -ETIMEDOUT;
}

static void s5l8947x_mcu_pulse_break(struct s5l8947x_mcu *mcu)
{
	u32 ucon;

	ucon = readl(mcu->regs + S5L_UART_UCON);
	writel(ucon | S5L_UCON_SBREAK, mcu->regs + S5L_UART_UCON);
	msleep(80);
	ucon = readl(mcu->regs + S5L_UART_UCON);
	writel(ucon & ~S5L_UCON_SBREAK, mcu->regs + S5L_UART_UCON);
	(void)readl(mcu->regs + S5L_UART_UERSTAT);
	msleep(5);
	s5l8947x_mcu_drain_rx(mcu);
}

static void s5l8947x_mcu_poll_work_fn(struct work_struct *work)
{
	struct s5l8947x_mcu *mcu =
		container_of(to_delayed_work(work), struct s5l8947x_mcu,
			     poll_work);

	if (!mcu->hw_ready)
		return;

	mutex_lock(&mcu->lock);
	if (!mcu->init_done) {
		mcu->init_done = true;
		s5l8947x_mcu_drain_rx(mcu);
		strscpy(mcu->fw_version, "online-250k",
			sizeof(mcu->fw_version));
		dev_info(mcu->dev,
			 "UART1 250000 baud IR/system MCU receiver active\n");
	} else {
		s5l8947x_mcu_drain_rx(mcu);
	}
	mutex_unlock(&mcu->lock);

	schedule_delayed_work(&mcu->poll_work, msecs_to_jiffies(25));
}

static int s5l8947x_mcu_led_set_blocking(struct led_classdev *led_cdev,
					 enum led_brightness value)
{
	struct s5l8947x_mcu *mcu =
		container_of(led_cdev, struct s5l8947x_mcu, led);
	u8 mode = (value > LED_OFF) ? MCU_STATE_ACTIVE : MCU_STATE_STANDBY;

	mutex_lock(&mcu->lock);
	s5l8947x_mcu_send_frame(mcu, MCU_CMD_SET_STATE, &mode, 1);
	mutex_unlock(&mcu->lock);
	return 0;
}

static int s5l8947x_mcu_reboot_notify(struct notifier_block *nb,
				      unsigned long action, void *data)
{
	struct s5l8947x_mcu *mcu =
		container_of(nb, struct s5l8947x_mcu, reboot_nb);
	u8 mode = (action == SYS_HALT || action == SYS_POWER_OFF) ?
		  MCU_STATE_STANDBY : MCU_STATE_ACTIVE;

	s5l8947x_mcu_send_frame(mcu, MCU_CMD_SET_STATE, &mode, 1);
	return NOTIFY_DONE;
}

static ssize_t fw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", mcu->fw_version);
}
static DEVICE_ATTR_RO(fw_version);

static ssize_t rx_count_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", mcu->rx_count);
}
static DEVICE_ATTR_RO(rx_count);

static ssize_t last_packet_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);
	int pos;
	size_t i;

	pos = sysfs_emit(buf, "cmd=0x%02x len=%zu data=",
			 mcu->last_cmd, mcu->last_len);
	for (i = 0; i < mcu->last_len; i++)
		pos += sysfs_emit_at(buf, pos, "%02x", mcu->last_payload[i]);
	pos += sysfs_emit_at(buf, pos, "\n");
	return pos;
}
static DEVICE_ATTR_RO(last_packet);

static ssize_t send_cmd_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);
	unsigned int cmd, arg;
	u8 payload[1];
	int parsed, ret;

	mutex_lock(&mcu->lock);
	if (!mcu->hw_ready && mcu->pmgr &&
	    s5l8947x_pmgr_enable_gate(mcu->pmgr, PMGR_GATE_UART1) == 0) {
		mcu->hw_ready = true;
		s5l8947x_uart_hw_init(mcu->regs, S5L_UBRDIV_250000);
		schedule_delayed_work(&mcu->poll_work, msecs_to_jiffies(20));
	}
	if (sysfs_streq(buf, "break")) {
		s5l8947x_mcu_pulse_break(mcu);
		mutex_unlock(&mcu->lock);
		return count;
	}
	if (sysfs_streq(buf, "ping")) {
		u8 ver_buf[MCU_MAX_PAYLOAD + 1];
		size_t ver_len = MCU_MAX_PAYLOAD;

		ret = s5l8947x_mcu_transact(mcu, MCU_CMD_PING_VERSION, NULL, 0,
					    ver_buf, &ver_len);
		if (ret == 0 && ver_len > 0) {
			ver_buf[ver_len - 1] = '\0';
			strscpy(mcu->fw_version, (const char *)ver_buf,
				sizeof(mcu->fw_version));
		}
		mutex_unlock(&mcu->lock);
		return count;
	}
	parsed = sscanf(buf, "%x %x", &cmd, &arg);
	if (parsed < 1 || cmd > 0xffU) {
		mutex_unlock(&mcu->lock);
		return -EINVAL;
	}
	if (parsed == 2) {
		payload[0] = arg & 0xffU;
		ret = s5l8947x_mcu_send_frame(mcu, cmd & 0xffU, payload, 1);
	} else {
		ret = s5l8947x_mcu_send_frame(mcu, cmd & 0xffU, NULL, 0);
	}
	mutex_unlock(&mcu->lock);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_WO(send_cmd);

static struct attribute *s5l8947x_mcu_attrs[] = {
	&dev_attr_fw_version.attr,
	&dev_attr_rx_count.attr,
	&dev_attr_last_packet.attr,
	&dev_attr_send_cmd.attr,
	NULL,
};
ATTRIBUTE_GROUPS(s5l8947x_mcu);

static int s5l8947x_mcu_probe(struct platform_device *pdev)
{
	struct s5l8947x_mcu *mcu;
	int ret;

	mcu = devm_kzalloc(&pdev->dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	mcu->dev = &pdev->dev;
	mutex_init(&mcu->lock);
	INIT_DELAYED_WORK(&mcu->poll_work, s5l8947x_mcu_poll_work_fn);
	strscpy(mcu->fw_version, "initializing", sizeof(mcu->fw_version));
	platform_set_drvdata(pdev, mcu);

	mcu->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mcu->regs))
		return PTR_ERR(mcu->regs);

	mcu->pmgr = devm_ioremap(&pdev->dev, PMGR_PHYS_BASE, 0x2000);
	if (mcu->pmgr &&
	    (readl(mcu->pmgr + PMGR_GATE_OFFSET(PMGR_GATE_UART1)) & 0xffU) == 0xffU) {
		mcu->hw_ready = true;
		s5l8947x_uart_hw_init(mcu->regs, S5L_UBRDIV_250000);
	}

	mcu->input = devm_input_allocate_device(&pdev->dev);
	if (!mcu->input)
		return -ENOMEM;

	mcu->input->name = "S5L8947X IR Remote";
	mcu->input->phys = "s5l8947x-mcu/input0";
	mcu->input->id.bustype = BUS_RS232;
	mcu->input->id.vendor = 0x05ac;
	mcu->input->id.product = 0x8947;
	mcu->input->id.version = 0x0100;

	input_set_capability(mcu->input, EV_MSC, MSC_SCAN);
	input_set_capability(mcu->input, EV_KEY, KEY_UP);
	input_set_capability(mcu->input, EV_KEY, KEY_DOWN);
	input_set_capability(mcu->input, EV_KEY, KEY_LEFT);
	input_set_capability(mcu->input, EV_KEY, KEY_RIGHT);
	input_set_capability(mcu->input, EV_KEY, KEY_ENTER);
	input_set_capability(mcu->input, EV_KEY, KEY_MENU);
	input_set_capability(mcu->input, EV_KEY, KEY_PLAYPAUSE);
	input_set_capability(mcu->input, EV_KEY, KEY_VOLUMEUP);
	input_set_capability(mcu->input, EV_KEY, KEY_VOLUMEDOWN);
	input_set_capability(mcu->input, EV_KEY, KEY_POWER);

	ret = input_register_device(mcu->input);
	if (ret)
		return ret;

	mcu->led.name = "j33iap:white:status";
	mcu->led.max_brightness = 1;
	mcu->led.brightness = 1;
	mcu->led.brightness_set_blocking = s5l8947x_mcu_led_set_blocking;
	ret = devm_led_classdev_register(&pdev->dev, &mcu->led);
	if (ret)
		dev_warn(&pdev->dev, "failed to register status LED: %d\n",
			 ret);

	mcu->reboot_nb.notifier_call = s5l8947x_mcu_reboot_notify;
	devm_register_reboot_notifier(&pdev->dev, &mcu->reboot_nb);

	schedule_delayed_work(&mcu->poll_work, msecs_to_jiffies(20));
	return 0;
}

static void s5l8947x_mcu_remove(struct platform_device *pdev)
{
	struct s5l8947x_mcu *mcu = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&mcu->poll_work);
}

static const struct of_device_id s5l8947x_mcu_of_match[] = {
	{ .compatible = "apple,s5l8947x-mcu" },
	{ .compatible = "apple,j33-mcu" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l8947x_mcu_of_match);

static struct platform_driver s5l8947x_mcu_driver = {
	.probe	= s5l8947x_mcu_probe,
	.remove	= s5l8947x_mcu_remove,
	.driver	= {
		.name		= "s5l8947x-mcu",
		.of_match_table	= s5l8947x_mcu_of_match,
		.dev_groups	= s5l8947x_mcu_groups,
	},
};

/*
 * 2. Broadcom BCM4334 Wireless Combo Core (Wi-Fi HSIC + Bluetooth UART2)
 */
struct s5l8947x_bcm4334 {
	struct device *dev;
	void __iomem *uart2;
	void __iomem *pmgr;
	struct regulator *wlan_supply;
	struct mutex lock;
	bool powered;
	bool hw_ready;
	unsigned int bt_rx_bytes;
	u8 bt_evt[16];
};

static int s5l8947x_bcm4334_bt_probe_hci(struct s5l8947x_bcm4334 *wl)
{
	/* Standard Bluetooth H4 HCI_Reset command packet: 01 03 0c 00 */
	static const u8 hci_reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
	int i, wait_ms;

	if (!wl->hw_ready)
		return -ENODEV;

	s5l8947x_uart_hw_init(wl->uart2, S5L_UBRDIV_115200);
	while (s5l8947x_uart_rx_ready(wl->uart2))
		(void)readl(wl->uart2 + S5L_UART_URXH);

	for (i = 0; i < 4; i++)
		s5l8947x_uart_putc(wl->uart2, hci_reset[i]);
	s5l8947x_uart_flush_tx(wl->uart2);

	wl->bt_rx_bytes = 0;
	memset(wl->bt_evt, 0, sizeof(wl->bt_evt));
	for (wait_ms = 0; wait_ms < 50; wait_ms++) {
		while (s5l8947x_uart_rx_ready(wl->uart2) &&
		       wl->bt_rx_bytes < sizeof(wl->bt_evt)) {
			wl->bt_evt[wl->bt_rx_bytes++] =
				readl(wl->uart2 + S5L_UART_URXH) & 0xffU;
		}
		if (wl->bt_rx_bytes >= 7)
			break;
		usleep_range(900, 1100);
	}
	return 0;
}

static ssize_t wl_power_state_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct s5l8947x_bcm4334 *wl = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", wl->powered ? "on" : "off");
}

static ssize_t wl_power_state_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct s5l8947x_bcm4334 *wl = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&wl->lock);
	if (sysfs_streq(buf, "on") || sysfs_streq(buf, "1")) {
		if (wl->wlan_supply && !wl->powered) {
			ret = regulator_enable(wl->wlan_supply);
			if (!ret)
				wl->powered = true;
		}
		s5l8947x_bcm4334_bt_probe_hci(wl);
	} else if (sysfs_streq(buf, "off") || sysfs_streq(buf, "0")) {
		if (wl->wlan_supply && wl->powered) {
			regulator_disable(wl->wlan_supply);
			wl->powered = false;
		}
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&wl->lock);
	return ret < 0 ? ret : count;
}
static struct device_attribute dev_attr_wl_power_state =
	__ATTR(power_state, 0644, wl_power_state_show, wl_power_state_store);

static ssize_t bt_hci_status_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct s5l8947x_bcm4334 *wl = dev_get_drvdata(dev);
	unsigned int i;
	int pos;

	pos = sysfs_emit(buf, "uart2=0x32700000 baud=115200 rx_bytes=%u evt=",
			 wl->bt_rx_bytes);
	for (i = 0; i < wl->bt_rx_bytes; i++)
		pos += sysfs_emit_at(buf, pos, "%02x", wl->bt_evt[i]);
	pos += sysfs_emit_at(buf, pos, "\n");
	return pos;
}
static DEVICE_ATTR_RO(bt_hci_status);

static ssize_t hsic_gates_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct s5l8947x_bcm4334 *wl = dev_get_drvdata(dev);
	u32 g32 = wl->pmgr ? readl(wl->pmgr + PMGR_GATE_OFFSET(PMGR_GATE_USB20_HOST)) : 0;
	u32 g78 = wl->pmgr ? readl(wl->pmgr + PMGR_GATE_OFFSET(PMGR_GATE_HSIC0PHY)) : 0;
	u32 g62 = wl->pmgr ? readl(wl->pmgr + PMGR_GATE_OFFSET(PMGR_GATE_UART2)) : 0;

	return sysfs_emit(buf,
			  "chip=BCM4334 usb20_host_gate32=0x%08x hsic0phy_gate78=0x%08x uart2_gate62=0x%08x\n",
			  g32, g78, g62);
}
static DEVICE_ATTR_RO(hsic_gates);

static struct attribute *s5l8947x_bcm4334_attrs[] = {
	&dev_attr_wl_power_state.attr,
	&dev_attr_bt_hci_status.attr,
	&dev_attr_hsic_gates.attr,
	NULL,
};
ATTRIBUTE_GROUPS(s5l8947x_bcm4334);

static int s5l8947x_bcm4334_probe(struct platform_device *pdev)
{
	struct s5l8947x_bcm4334 *wl;
	int ret;

	wl = devm_kzalloc(&pdev->dev, sizeof(*wl), GFP_KERNEL);
	if (!wl)
		return -ENOMEM;

	wl->dev = &pdev->dev;
	mutex_init(&wl->lock);
	platform_set_drvdata(pdev, wl);

	wl->uart2 = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wl->uart2))
		return PTR_ERR(wl->uart2);

	wl->pmgr = devm_ioremap(&pdev->dev, PMGR_PHYS_BASE, 0x2000);
	if (wl->pmgr) {
		(void)s5l8947x_pmgr_enable_gate(wl->pmgr, PMGR_GATE_USB20_HOST);
		(void)s5l8947x_pmgr_enable_gate(wl->pmgr, PMGR_GATE_HSIC0PHY);
		if (s5l8947x_pmgr_enable_gate(wl->pmgr, PMGR_GATE_UART2) == 0)
			wl->hw_ready = true;
	}

	wl->wlan_supply = devm_regulator_get_optional(&pdev->dev, "wlan");
	if (IS_ERR(wl->wlan_supply)) {
		if (PTR_ERR(wl->wlan_supply) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		wl->wlan_supply = NULL;
	}
	if (wl->wlan_supply) {
		ret = regulator_enable(wl->wlan_supply);
		if (!ret) {
			wl->powered = true;
			msleep(20);
		}
	}

	s5l8947x_bcm4334_bt_probe_hci(wl);

	dev_info(&pdev->dev,
		 "BCM4334 Wireless Combo initialized (wlan_reg_on=%s, HSIC gates=32/78, UART2 HCI rx=%u)\n",
		 wl->powered ? "on" : "ext", wl->bt_rx_bytes);
	return 0;
}

static const struct of_device_id s5l8947x_bcm4334_of_match[] = {
	{ .compatible = "apple,s5l8947x-bcm4334" },
	{ .compatible = "brcm,bcm4334-combo" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l8947x_bcm4334_of_match);

static struct platform_driver s5l8947x_bcm4334_driver = {
	.probe	= s5l8947x_bcm4334_probe,
	.driver	= {
		.name		= "s5l8947x-bcm4334",
		.of_match_table	= s5l8947x_bcm4334_of_match,
		.dev_groups	= s5l8947x_bcm4334_groups,
	},
};

static int __init s5l8947x_mcu_wl_init(void)
{
	int ret;

	ret = platform_driver_register(&s5l8947x_mcu_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&s5l8947x_bcm4334_driver);
	if (ret)
		platform_driver_unregister(&s5l8947x_mcu_driver);
	return ret;
}
module_init(s5l8947x_mcu_wl_init);

static void __exit s5l8947x_mcu_wl_exit(void)
{
	platform_driver_unregister(&s5l8947x_bcm4334_driver);
	platform_driver_unregister(&s5l8947x_mcu_driver);
}
module_exit(s5l8947x_mcu_wl_exit);

MODULE_DESCRIPTION("S5L8947X On-Board IR/System MCU and BCM4334 Wireless Combo Driver");
MODULE_LICENSE("GPL");
