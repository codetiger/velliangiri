// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X (j33iap) system / IR remote MCU
 *
 * The MCU is a serdev client of UART1 (250000 baud 8N1). Frames in both
 * directions are
 *
 *   'S' 't' cmd 0x01 len_hi len_lo payload... crc_hi crc_lo
 *
 * with a CRC-16/CCITT-FALSE (init 0xffff, poly 0x1021) over everything
 * before the CRC. Requests are answered with an ACK (0x80) or BUSY (0x84)
 * frame.
 *
 * IR remote input arrives unsolicited. A button press is a 0x10 frame
 * carrying the 32-bit NEC code (0x87 0xee address, remote ID, command);
 * while the button is held the MCU sends a payload-less 0x11 frame about
 * every 110 ms. There is no release frame, so a key is released once
 * the frames stop. Remotes with dedicated Select and Play buttons follow
 * those codes with 0x04 (Play on single-code remotes), which is folded
 * into the first key.
 *
 * Nothing is sent at probe. The firmware version query, a line break and
 * raw commands are issued on demand through sysfs. The standby state puts
 * the whole box into standby, so it is only sent on halt and power-off.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/serdev.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define MCU_BAUD		250000U
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

#define MCU_EVT_IR_KEY		0x10U
#define MCU_EVT_IR_REPEAT	0x11U
#define MCU_RSP_ACK		0x80U
#define MCU_RSP_BUSY		0x84U

/* NEC command byte of the IR key event; bit 0 does not select the key */
#define MCU_IR_PLAY_COMPAT	0x04U
#define MCU_IR_SELECT		0x5cU
#define MCU_IR_PLAY		0x5eU
#define MCU_KEY_RELEASE		msecs_to_jiffies(200)

#define MCU_TX_TIMEOUT		msecs_to_jiffies(100)
#define MCU_RSP_TIMEOUT		msecs_to_jiffies(120)
#define MCU_BUSY_RETRY_MS	40
#define MCU_RETRIES		3
#define MCU_BREAK_MS		80

enum mcu_rx_state {
	MCU_RX_WAIT_S = 0,
	MCU_RX_WAIT_T,
	MCU_RX_HEADER,
	MCU_RX_PAYLOAD_CRC,
};

struct s5l8947x_mcu {
	struct serdev_device *serdev;
	struct input_dev *input;
	struct notifier_block reboot_nb;
	struct delayed_work key_release;
	struct mutex cmd_lock;		/* one request on the wire at a time */
	spinlock_t lock;		/* response and statistics fields */
	struct completion rsp_done;

	/* Receive parser, only touched from the serdev receive path */
	enum mcu_rx_state rx_state;
	u8 rx_frame[MCU_MAX_FRAME];
	size_t rx_pos;
	size_t rx_expected;

	u8 rsp_cmd;
	u8 rsp_buf[MCU_MAX_PAYLOAD];
	size_t rsp_len;

	unsigned int key;		/* key held down, or KEY_RESERVED */
	u8 key_code;			/* its NEC command byte */

	unsigned long rx_count;
	unsigned long crc_errors;
	u8 last_cmd;
	u8 last_payload[MCU_MAX_PAYLOAD];
	size_t last_len;
	char fw_version[MCU_MAX_PAYLOAD + 1];
};

static const unsigned int s5l8947x_mcu_keys[] = {
	KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_MENU,
	KEY_PLAYPAUSE,
};

static u16 s5l8947x_mcu_crc16(u16 crc, const u8 *data, size_t len)
{
	size_t i;
	int bit;

	for (i = 0; i < len; i++) {
		crc ^= (u16)data[i] << 8;
		for (bit = 0; bit < 8; bit++)
			crc = (crc & 0x8000U) ? (crc << 1) ^ 0x1021U : crc << 1;
	}
	return crc;
}

static unsigned int s5l8947x_mcu_map_ir_key(u8 code)
{
	switch (code & 0xfeU) {
	case 0x02:
		return KEY_MENU;
	case MCU_IR_SELECT:
		return KEY_ENTER;
	case 0x06:
		return KEY_RIGHT;
	case 0x08:
		return KEY_LEFT;
	case 0x0a:
		return KEY_UP;
	case 0x0c:
		return KEY_DOWN;
	case MCU_IR_PLAY_COMPAT:
	case MCU_IR_PLAY:
		return KEY_PLAYPAUSE;
	default:
		return KEY_UNKNOWN;
	}
}

/* Called with mcu->lock held */
static void s5l8947x_mcu_key_up(struct s5l8947x_mcu *mcu)
{
	if (mcu->key == KEY_RESERVED)
		return;
	input_report_key(mcu->input, mcu->key, 0);
	input_sync(mcu->input);
	mcu->key = KEY_RESERVED;
}

static void s5l8947x_mcu_key_release_work(struct work_struct *work)
{
	struct s5l8947x_mcu *mcu =
		container_of(to_delayed_work(work), struct s5l8947x_mcu,
			     key_release);

	spin_lock(&mcu->lock);
	s5l8947x_mcu_key_up(mcu);
	spin_unlock(&mcu->lock);
}

/* Called with mcu->lock held */
static void s5l8947x_mcu_handle_ir_key(struct s5l8947x_mcu *mcu,
				       const u8 *payload, size_t len)
{
	unsigned int keycode;
	u8 code;

	if (len < 4)
		return;
	code = payload[3] & 0xfeU;
	input_event(mcu->input, EV_MSC, MSC_SCAN, get_unaligned_be32(payload));

	/* The 0x04 that trails a Select or Play press belongs to that press */
	if (code == MCU_IR_PLAY_COMPAT && mcu->key != KEY_RESERVED &&
	    (mcu->key_code == MCU_IR_SELECT || mcu->key_code == MCU_IR_PLAY)) {
		input_sync(mcu->input);
		mod_delayed_work(system_wq, &mcu->key_release, MCU_KEY_RELEASE);
		return;
	}

	s5l8947x_mcu_key_up(mcu);
	keycode = s5l8947x_mcu_map_ir_key(code);
	if (keycode != KEY_UNKNOWN) {
		input_report_key(mcu->input, keycode, 1);
		mcu->key = keycode;
		mcu->key_code = code;
		mod_delayed_work(system_wq, &mcu->key_release, MCU_KEY_RELEASE);
	}
	input_sync(mcu->input);
}

/* Called with mcu->lock held */
static void s5l8947x_mcu_handle_ir_repeat(struct s5l8947x_mcu *mcu)
{
	if (mcu->key == KEY_RESERVED)
		return;
	input_event(mcu->input, EV_KEY, mcu->key, 2);
	input_sync(mcu->input);
	mod_delayed_work(system_wq, &mcu->key_release, MCU_KEY_RELEASE);
}

static void s5l8947x_mcu_dispatch_frame(struct s5l8947x_mcu *mcu,
					const u8 *frame, size_t len)
{
	const u8 *payload = &frame[MCU_HEADER_LEN];
	u8 cmd = frame[2];
	u16 crc;

	crc = get_unaligned_be16(&frame[MCU_HEADER_LEN + len]);
	spin_lock(&mcu->lock);
	if (s5l8947x_mcu_crc16(0xffffU, frame, MCU_HEADER_LEN + len) != crc) {
		mcu->crc_errors++;
		spin_unlock(&mcu->lock);
		return;
	}

	mcu->rx_count++;
	mcu->last_cmd = cmd;
	mcu->last_len = len;
	memcpy(mcu->last_payload, payload, len);

	if (cmd == MCU_RSP_ACK || cmd == MCU_RSP_BUSY) {
		mcu->rsp_cmd = cmd;
		mcu->rsp_len = len;
		memcpy(mcu->rsp_buf, payload, len);
		spin_unlock(&mcu->lock);
		complete(&mcu->rsp_done);
		return;
	}

	if (cmd == MCU_EVT_IR_KEY)
		s5l8947x_mcu_handle_ir_key(mcu, payload, len);
	else if (cmd == MCU_EVT_IR_REPEAT)
		s5l8947x_mcu_handle_ir_repeat(mcu);
	spin_unlock(&mcu->lock);
}

static void s5l8947x_mcu_feed_byte(struct s5l8947x_mcu *mcu, u8 b)
{
	size_t len;

	switch (mcu->rx_state) {
	case MCU_RX_WAIT_S:
		if (b == MCU_SYNC_BYTE0) {
			mcu->rx_frame[0] = b;
			mcu->rx_state = MCU_RX_WAIT_T;
		}
		break;

	case MCU_RX_WAIT_T:
		if (b == MCU_SYNC_BYTE1) {
			mcu->rx_frame[1] = b;
			mcu->rx_pos = 2;
			mcu->rx_state = MCU_RX_HEADER;
		} else if (b != MCU_SYNC_BYTE0) {
			mcu->rx_state = MCU_RX_WAIT_S;
		}
		break;

	case MCU_RX_HEADER:
		mcu->rx_frame[mcu->rx_pos++] = b;
		if (mcu->rx_pos < MCU_HEADER_LEN)
			break;
		len = get_unaligned_be16(&mcu->rx_frame[4]);
		if (len > MCU_MAX_PAYLOAD) {
			mcu->rx_state = MCU_RX_WAIT_S;
			break;
		}
		mcu->rx_expected = MCU_HEADER_LEN + len + MCU_CRC_LEN;
		mcu->rx_state = MCU_RX_PAYLOAD_CRC;
		break;

	case MCU_RX_PAYLOAD_CRC:
		mcu->rx_frame[mcu->rx_pos++] = b;
		if (mcu->rx_pos == mcu->rx_expected) {
			s5l8947x_mcu_dispatch_frame(mcu, mcu->rx_frame,
				mcu->rx_expected - MCU_HEADER_LEN - MCU_CRC_LEN);
			mcu->rx_state = MCU_RX_WAIT_S;
		}
		break;
	}
}

static size_t s5l8947x_mcu_receive_buf(struct serdev_device *serdev,
				       const u8 *data, size_t count)
{
	struct s5l8947x_mcu *mcu = serdev_device_get_drvdata(serdev);
	size_t i;

	for (i = 0; i < count; i++)
		s5l8947x_mcu_feed_byte(mcu, data[i]);
	return count;
}

static const struct serdev_device_ops s5l8947x_mcu_serdev_ops = {
	.receive_buf	= s5l8947x_mcu_receive_buf,
	.write_wakeup	= serdev_device_write_wakeup,
};

/* Called with cmd_lock held */
static int s5l8947x_mcu_send_frame(struct s5l8947x_mcu *mcu, u8 cmd,
				   const u8 *payload, size_t len)
{
	u8 frame[MCU_MAX_FRAME];
	size_t total = MCU_HEADER_LEN + len + MCU_CRC_LEN;
	ssize_t ret;

	if (len > MCU_MAX_PAYLOAD)
		return -EINVAL;

	frame[0] = MCU_SYNC_BYTE0;
	frame[1] = MCU_SYNC_BYTE1;
	frame[2] = cmd;
	frame[3] = MCU_PROTO_VER;
	put_unaligned_be16(len, &frame[4]);
	if (len)
		memcpy(&frame[MCU_HEADER_LEN], payload, len);
	put_unaligned_be16(s5l8947x_mcu_crc16(0xffffU, frame,
					      MCU_HEADER_LEN + len),
			   &frame[MCU_HEADER_LEN + len]);

	ret = serdev_device_write(mcu->serdev, frame, total, MCU_TX_TIMEOUT);
	if (ret < 0)
		return ret;
	if (ret != total)
		return -ETIMEDOUT;
	serdev_device_wait_until_sent(mcu->serdev, MCU_TX_TIMEOUT);
	return 0;
}

/* Send a request and wait for its ACK. Called with cmd_lock held. */
static int s5l8947x_mcu_transact(struct s5l8947x_mcu *mcu, u8 cmd,
				 const u8 *payload, size_t len,
				 u8 *out, size_t *out_len)
{
	int attempt, ret;
	u8 rsp;

	for (attempt = 0; attempt < MCU_RETRIES; attempt++) {
		spin_lock(&mcu->lock);
		mcu->rsp_cmd = 0;
		reinit_completion(&mcu->rsp_done);
		spin_unlock(&mcu->lock);

		ret = s5l8947x_mcu_send_frame(mcu, cmd, payload, len);
		if (ret)
			return ret;
		if (!wait_for_completion_timeout(&mcu->rsp_done,
						 MCU_RSP_TIMEOUT))
			continue;

		spin_lock(&mcu->lock);
		rsp = mcu->rsp_cmd;
		if (rsp == MCU_RSP_ACK && out) {
			*out_len = min(*out_len, mcu->rsp_len);
			memcpy(out, mcu->rsp_buf, *out_len);
		}
		spin_unlock(&mcu->lock);

		if (rsp == MCU_RSP_ACK)
			return 0;
		msleep(MCU_BUSY_RETRY_MS);
	}
	return -ETIMEDOUT;
}

static int s5l8947x_mcu_query_version(struct s5l8947x_mcu *mcu)
{
	u8 buf[MCU_MAX_PAYLOAD];
	size_t len = sizeof(buf);
	int ret;

	ret = s5l8947x_mcu_transact(mcu, MCU_CMD_PING_VERSION, NULL, 0,
				    buf, &len);
	if (ret)
		return ret;
	if (!len)
		return -EPROTO;

	/* The version string's last byte is its terminator */
	spin_lock(&mcu->lock);
	strscpy(mcu->fw_version, (const char *)buf, len);
	spin_unlock(&mcu->lock);
	return 0;
}

static int s5l8947x_mcu_send_break(struct s5l8947x_mcu *mcu)
{
	int ret;

	ret = serdev_device_break_ctl(mcu->serdev, -1);
	if (ret)
		return ret;
	msleep(MCU_BREAK_MS);
	return serdev_device_break_ctl(mcu->serdev, 0);
}

static int s5l8947x_mcu_reboot_notify(struct notifier_block *nb,
				      unsigned long action, void *data)
{
	struct s5l8947x_mcu *mcu =
		container_of(nb, struct s5l8947x_mcu, reboot_nb);
	u8 state = (action == SYS_HALT || action == SYS_POWER_OFF) ?
		   MCU_STATE_STANDBY : MCU_STATE_ACTIVE;

	mutex_lock(&mcu->cmd_lock);
	s5l8947x_mcu_send_frame(mcu, MCU_CMD_SET_STATE, &state, 1);
	mutex_unlock(&mcu->cmd_lock);
	return NOTIFY_DONE;
}

static ssize_t fw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);
	ssize_t ret;

	spin_lock(&mcu->lock);
	ret = sysfs_emit(buf, "%s\n", mcu->fw_version);
	spin_unlock(&mcu->lock);
	return ret;
}
static DEVICE_ATTR_RO(fw_version);

static ssize_t rx_count_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", READ_ONCE(mcu->rx_count));
}
static DEVICE_ATTR_RO(rx_count);

static ssize_t crc_errors_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", READ_ONCE(mcu->crc_errors));
}
static DEVICE_ATTR_RO(crc_errors);

static ssize_t last_packet_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);
	int pos;

	spin_lock(&mcu->lock);
	pos = sysfs_emit(buf, "cmd=0x%02x len=%zu data=%*phN\n",
			 mcu->last_cmd, mcu->last_len, (int)mcu->last_len,
			 mcu->last_payload);
	spin_unlock(&mcu->lock);
	return pos;
}
static DEVICE_ATTR_RO(last_packet);

/*
 * "ping"  - query the firmware version (answered with an ACK)
 * "break" - hold the line in break for 80 ms
 * "<cmd> [<arg>]" - send a raw command frame, hex, without waiting
 */
static ssize_t send_cmd_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);
	unsigned int cmd, arg;
	u8 payload;
	int parsed, ret;

	mutex_lock(&mcu->cmd_lock);
	if (sysfs_streq(buf, "ping")) {
		ret = s5l8947x_mcu_query_version(mcu);
	} else if (sysfs_streq(buf, "break")) {
		ret = s5l8947x_mcu_send_break(mcu);
	} else {
		parsed = sscanf(buf, "%x %x", &cmd, &arg);
		if (parsed < 1 || cmd > 0xffU || (parsed == 2 && arg > 0xffU)) {
			ret = -EINVAL;
		} else {
			payload = arg;
			ret = s5l8947x_mcu_send_frame(mcu, cmd, &payload,
						      parsed == 2 ? 1 : 0);
		}
	}
	mutex_unlock(&mcu->cmd_lock);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_WO(send_cmd);

static struct attribute *s5l8947x_mcu_attrs[] = {
	&dev_attr_fw_version.attr,
	&dev_attr_rx_count.attr,
	&dev_attr_crc_errors.attr,
	&dev_attr_last_packet.attr,
	&dev_attr_send_cmd.attr,
	NULL,
};
ATTRIBUTE_GROUPS(s5l8947x_mcu);

static void s5l8947x_mcu_cancel_key_release(void *data)
{
	struct s5l8947x_mcu *mcu = data;

	cancel_delayed_work_sync(&mcu->key_release);
}

static int s5l8947x_mcu_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct s5l8947x_mcu *mcu;
	u32 speed = MCU_BAUD;
	unsigned int i, actual;
	int ret;

	mcu = devm_kzalloc(dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	mcu->serdev = serdev;
	mutex_init(&mcu->cmd_lock);
	spin_lock_init(&mcu->lock);
	init_completion(&mcu->rsp_done);
	INIT_DELAYED_WORK(&mcu->key_release, s5l8947x_mcu_key_release_work);
	mcu->key = KEY_RESERVED;
	strscpy(mcu->fw_version, "unknown", sizeof(mcu->fw_version));
	serdev_device_set_drvdata(serdev, mcu);

	mcu->input = devm_input_allocate_device(dev);
	if (!mcu->input)
		return -ENOMEM;

	mcu->input->name = "S5L8947X IR Remote";
	mcu->input->phys = "s5l8947x-mcu/input0";
	mcu->input->id.bustype = BUS_RS232;
	mcu->input->id.vendor = 0x05ac;
	mcu->input->id.product = 0x8947;
	mcu->input->id.version = 0x0100;
	input_set_capability(mcu->input, EV_MSC, MSC_SCAN);
	for (i = 0; i < ARRAY_SIZE(s5l8947x_mcu_keys); i++)
		input_set_capability(mcu->input, EV_KEY, s5l8947x_mcu_keys[i]);

	ret = input_register_device(mcu->input);
	if (ret)
		return ret;

	/* Runs after the UART is closed and before the input device goes */
	ret = devm_add_action_or_reset(dev, s5l8947x_mcu_cancel_key_release, mcu);
	if (ret)
		return ret;

	serdev_device_set_client_ops(serdev, &s5l8947x_mcu_serdev_ops);
	ret = devm_serdev_device_open(dev, serdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to open UART\n");

	of_property_read_u32(dev->of_node, "current-speed", &speed);
	actual = serdev_device_set_baudrate(serdev, speed);
	if (actual != speed)
		dev_warn(dev, "UART runs at %u baud, not %u\n", actual, speed);
	serdev_device_set_flow_control(serdev, false);
	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set parity\n");

	mcu->reboot_nb.notifier_call = s5l8947x_mcu_reboot_notify;
	ret = devm_register_reboot_notifier(dev, &mcu->reboot_nb);
	if (ret)
		return ret;

	dev_info(dev, "IR remote / system MCU on UART at %u baud\n", speed);
	return 0;
}

static const struct of_device_id s5l8947x_mcu_of_match[] = {
	{ .compatible = "apple,j33-mcu" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l8947x_mcu_of_match);

static struct serdev_device_driver s5l8947x_mcu_driver = {
	.probe	= s5l8947x_mcu_probe,
	.driver	= {
		.name		= "s5l8947x-mcu",
		.of_match_table	= s5l8947x_mcu_of_match,
		.dev_groups	= s5l8947x_mcu_groups,
	},
};
module_serdev_device_driver(s5l8947x_mcu_driver);

MODULE_DESCRIPTION("S5L8947X system / IR remote MCU driver");
MODULE_LICENSE("GPL");
