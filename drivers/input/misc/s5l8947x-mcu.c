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
 * At probe the driver repeats the stock bring-up: version query, then the
 * active state. The second step also takes an MCU that the previous
 * power-off left in standby back onto the wire. The board wires the MCU's
 * NMI/SOS line to GPIO 2, which is requested here as a counter and wake
 * source; commands the driver does not understand are kept in a small
 * event ring for later protocol work. A line break and raw commands are
 * issued on demand through sysfs. The standby state puts the whole box
 * into standby, so it is only sent on halt and power-off.
 */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>
#include <linux/reboot.h>
#include <linux/serdev.h>
#include <linux/spinlock.h>
#include <linux/string.h>
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
#define MCU_EVENT_RING		16U

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
#define MCU_RETRY_GUARD_MS	20
#define MCU_RETRIES		3
#define MCU_BREAK_MS		80

enum mcu_rx_state {
	MCU_RX_WAIT_S = 0,
	MCU_RX_WAIT_T,
	MCU_RX_HEADER,
	MCU_RX_PAYLOAD_CRC,
};

/* An unsolicited frame, kept so unknown commands can be decoded later */
struct s5l8947x_mcu_event {
	u64 ts_ns;
	u8 cmd;
	u8 len;
	u8 payload[MCU_MAX_PAYLOAD];
};

struct s5l8947x_mcu {
	struct serdev_device *serdev;
	struct input_dev *input;
	struct notifier_block reboot_nb;
	struct delayed_work key_release;
	struct mutex cmd_lock;		/* one request on the wire at a time */
	spinlock_t lock;		/* response, statistics and event ring */
	struct completion rsp_done;

	/* Receive parser, only touched from the serdev receive path */
	enum mcu_rx_state rx_state;
	u8 rx_frame[MCU_MAX_FRAME];
	size_t rx_pos;
	size_t rx_expected;

	/*
	 * A response is only accepted while a request is armed. The window
	 * is disarmed on timeout and re-armed only after the retry guard,
	 * so a late ACK from a previous attempt is normally drained while
	 * disarmed and counted instead of completing the next request.
	 */
	bool rsp_armed;
	u8 rsp_cmd;
	u8 rsp_buf[MCU_MAX_PAYLOAD];
	size_t rsp_len;

	unsigned int key;		/* key held down, or KEY_RESERVED */
	u8 key_code;			/* its NEC command byte */
	bool game_map;			/* remote reports game keys */

	int nmi_irq;
	atomic_t nmi_count;
	bool wake_enabled;
	u32 recovery_cmd;		/* 0 = no recovery command configured */

	unsigned long rx_count;
	unsigned long crc_errors;
	unsigned long late_acks;
	unsigned long timeouts;

	struct s5l8947x_mcu_event events[MCU_EVENT_RING];
	unsigned int event_head;
	unsigned int event_seq;

	u8 last_cmd;
	u8 last_payload[MCU_MAX_PAYLOAD];
	size_t last_len;
	char fw_version[MCU_MAX_PAYLOAD + 1];
};

static const unsigned int s5l8947x_mcu_keys[] = {
	KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_MENU,
	KEY_PLAYPAUSE,
	/* game mapping of the same buttons */
	KEY_ESC, KEY_LEFTCTRL, KEY_SPACE,
};

static u16 s5l8947x_mcu_crc16(u16 crc, const u8 *data, size_t len)
{
	size_t i;
	int bit;

	for (i = 0; i < len; i++) {
		crc ^= (u16)data[i] << 8;
		for (bit = 0; bit < 8; bit++)
			crc = (crc << 1) ^ ((crc & 0x8000U) ? 0x1021U : 0);
	}
	return crc;
}

/*
 * The remote has seven buttons. The default mapping keeps their media key
 * codes; the game mapping (apple,game-keys in the device tree) turns the
 * same buttons into the keys a first-person shooter expects, so the arrows
 * stay on movement and the remaining three become fire, use and menu.
 */
static unsigned int s5l8947x_mcu_map_ir_key(struct s5l8947x_mcu *mcu, u8 code)
{
	switch (code & 0xfeU) {
	case 0x02:
		return mcu->game_map ? KEY_ESC : KEY_MENU;
	case MCU_IR_SELECT:
		return mcu->game_map ? KEY_LEFTCTRL : KEY_ENTER;
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
		return mcu->game_map ? KEY_SPACE : KEY_PLAYPAUSE;
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
static void s5l8947x_mcu_key_repeat(struct s5l8947x_mcu *mcu)
{
	input_event(mcu->input, EV_KEY, mcu->key, 2);
	input_sync(mcu->input);
	mod_delayed_work(system_wq, &mcu->key_release, MCU_KEY_RELEASE);
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
	keycode = s5l8947x_mcu_map_ir_key(mcu, code);
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
	s5l8947x_mcu_key_repeat(mcu);
}

/* Called with mcu->lock held */
static void s5l8947x_mcu_record_event(struct s5l8947x_mcu *mcu, u8 cmd,
				      const u8 *payload, size_t len)
{
	struct s5l8947x_mcu_event *event = &mcu->events[mcu->event_head];

	event->ts_ns = ktime_get_ns();
	event->cmd = cmd;
	event->len = len;
	if (len)
		memcpy(event->payload, payload, len);

	mcu->event_head = (mcu->event_head + 1) % MCU_EVENT_RING;
	mcu->event_seq++;
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
		/*
		 * No request is waiting: this is a response to a raw command
		 * or a late reply to an attempt that already timed out.
		 */
		if (!mcu->rsp_armed) {
			mcu->late_acks++;
			spin_unlock(&mcu->lock);
			return;
		}
		mcu->rsp_armed = false;
		mcu->rsp_cmd = cmd;
		mcu->rsp_len = len;
		memcpy(mcu->rsp_buf, payload, len);
		spin_unlock(&mcu->lock);
		complete(&mcu->rsp_done);
		return;
	}

	s5l8947x_mcu_record_event(mcu, cmd, payload, len);

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

/* Write the whole frame; flush on failure so the next request starts clean */
static int s5l8947x_mcu_write(struct s5l8947x_mcu *mcu, const u8 *buf,
			      size_t len)
{
	size_t off = 0;
	ssize_t ret;

	while (off < len) {
		ret = serdev_device_write(mcu->serdev, buf + off, len - off,
					  MCU_TX_TIMEOUT);
		if (ret < 0) {
			serdev_device_write_flush(mcu->serdev);
			return ret;
		}
		if (!ret) {
			serdev_device_write_flush(mcu->serdev);
			return -ETIMEDOUT;
		}
		off += ret;
	}

	serdev_device_wait_until_sent(mcu->serdev, MCU_TX_TIMEOUT);
	return 0;
}

/* Called with cmd_lock held */
static int s5l8947x_mcu_send_frame(struct s5l8947x_mcu *mcu, u8 cmd,
				   const u8 *payload, size_t len)
{
	u8 frame[MCU_MAX_FRAME];
	size_t total = MCU_HEADER_LEN + len + MCU_CRC_LEN;

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

	return s5l8947x_mcu_write(mcu, frame, total);
}

/*
 * Send a request and wait for its ACK. Called with cmd_lock held. A late
 * reply to an earlier attempt is drained by the guard delay before the
 * window is re-armed, so it cannot satisfy this request. Frames carry no
 * request identifier, which is why the guard exists at all.
 */
static int s5l8947x_mcu_transact_n(struct s5l8947x_mcu *mcu, u8 cmd,
				   const u8 *payload, size_t len, u8 *out,
				   size_t *out_len, unsigned int tries)
{
	unsigned int attempt;
	int ret;
	u8 rsp;

	for (attempt = 0; attempt < tries; attempt++) {
		/* Let a late reply to the previous attempt arrive first */
		if (attempt)
			msleep(MCU_RETRY_GUARD_MS);

		spin_lock(&mcu->lock);
		mcu->rsp_cmd = 0;
		mcu->rsp_armed = true;
		reinit_completion(&mcu->rsp_done);
		spin_unlock(&mcu->lock);

		ret = s5l8947x_mcu_send_frame(mcu, cmd, payload, len);
		if (ret) {
			spin_lock(&mcu->lock);
			mcu->rsp_armed = false;
			spin_unlock(&mcu->lock);
			return ret;
		}

		if (!wait_for_completion_timeout(&mcu->rsp_done,
						 MCU_RSP_TIMEOUT)) {
			spin_lock(&mcu->lock);
			mcu->rsp_armed = false;
			mcu->timeouts++;
			spin_unlock(&mcu->lock);
			continue;
		}

		spin_lock(&mcu->lock);
		rsp = mcu->rsp_cmd;
		if (rsp == MCU_RSP_ACK && out && out_len) {
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

static int s5l8947x_mcu_transact(struct s5l8947x_mcu *mcu, u8 cmd,
				 const u8 *payload, size_t len, u8 *out,
				 size_t *out_len)
{
	return s5l8947x_mcu_transact_n(mcu, cmd, payload, len, out, out_len,
				       MCU_RETRIES);
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

/*
 * Stock bring-up: query the version, then select the active state. The
 * state frame is what brings an MCU left in standby by the previous
 * power-off back onto the wire. Both steps are best effort.
 */
static int s5l8947x_mcu_handshake(struct s5l8947x_mcu *mcu)
{
	struct device *dev = &mcu->serdev->dev;
	u8 state = MCU_STATE_ACTIVE;
	int ret, state_ret;

	ret = s5l8947x_mcu_query_version(mcu);
	state_ret = s5l8947x_mcu_transact_n(mcu, MCU_CMD_SET_STATE, &state, 1,
					    NULL, NULL, 2);

	/* An MCU that was in standby may only answer after waking up */
	if (ret && !state_ret)
		ret = s5l8947x_mcu_query_version(mcu);

	if (ret)
		dev_warn(dev, "MCU version query failed: %d\n", ret);
	if (state_ret)
		dev_warn(dev, "MCU did not acknowledge the active state: %d\n",
			 state_ret);

	return ret ? ret : state_ret;
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
	struct device *dev = &mcu->serdev->dev;
	const char *command = data;
	u8 state;
	int ret;

	mutex_lock(&mcu->cmd_lock);
	if (action == SYS_HALT || action == SYS_POWER_OFF) {
		state = MCU_STATE_STANDBY;
		s5l8947x_mcu_transact_n(mcu, MCU_CMD_SET_STATE, &state, 1,
					NULL, NULL, 1);
	} else if (action == SYS_RESTART && command &&
		   !strcmp(command, "recovery")) {
		/*
		 * The recovery trigger itself is not decoded yet; a board can
		 * opt in with apple,recovery-command once it is known.
		 */
		if (!mcu->recovery_cmd) {
			dev_info(dev, "recovery reboot requested, no MCU command configured\n");
		} else {
			ret = s5l8947x_mcu_transact_n(mcu, (u8)mcu->recovery_cmd,
						      NULL, 0, NULL, NULL, 1);
			if (ret)
				dev_warn(dev, "MCU recovery command 0x%02x failed: %d\n",
					 mcu->recovery_cmd, ret);
		}
	} else {
		state = MCU_STATE_ACTIVE;
		s5l8947x_mcu_transact_n(mcu, MCU_CMD_SET_STATE, &state, 1,
					NULL, NULL, 1);
	}
	mutex_unlock(&mcu->cmd_lock);
	return NOTIFY_DONE;
}

static irqreturn_t s5l8947x_mcu_nmi(int irq, void *data)
{
	struct s5l8947x_mcu *mcu = data;

	atomic_inc(&mcu->nmi_count);
	return IRQ_HANDLED;
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

static ssize_t response_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);
	ssize_t ret;

	spin_lock(&mcu->lock);
	if (!mcu->rsp_cmd)
		ret = sysfs_emit(buf, "none\n");
	else
		ret = sysfs_emit(buf, "cmd=0x%02x len=%zu data=%*phN\n",
				 mcu->rsp_cmd, mcu->rsp_len,
				 (int)mcu->rsp_len, mcu->rsp_buf);
	spin_unlock(&mcu->lock);
	return ret;
}
static DEVICE_ATTR_RO(response);

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

static ssize_t late_acks_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", READ_ONCE(mcu->late_acks));
}
static DEVICE_ATTR_RO(late_acks);

static ssize_t timeouts_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", READ_ONCE(mcu->timeouts));
}
static DEVICE_ATTR_RO(timeouts);

static ssize_t nmi_count_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", atomic_read(&mcu->nmi_count));
}
static DEVICE_ATTR_RO(nmi_count);

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

/* Monotonic milliseconds, command and payload of recent unsolicited frames */
static ssize_t events_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);
	unsigned int count, i;
	ssize_t pos = 0;

	spin_lock(&mcu->lock);
	count = min(mcu->event_seq, MCU_EVENT_RING);
	for (i = 0; i < count; i++) {
		unsigned int index = (mcu->event_head + MCU_EVENT_RING - count +
				      i) % MCU_EVENT_RING;
		struct s5l8947x_mcu_event *event = &mcu->events[index];

		pos += sysfs_emit_at(buf, pos, "%llu cmd=0x%02x len=%u data=%*phN\n",
				     div_u64(event->ts_ns, NSEC_PER_MSEC), event->cmd,
				     event->len, event->len, event->payload);
	}
	spin_unlock(&mcu->lock);
	return pos;
}
static DEVICE_ATTR_RO(events);

/*
 * "ping"  - query the firmware version (answered with an ACK)
 * "break" - hold the line in break for 80 ms
 * "<cmd> [<arg>]" - send a raw command frame, without waiting
 */
static ssize_t send_cmd_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);
	unsigned int cmd, arg = 0;
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
	if (ret < 0)
		return ret;
	return count;
}
static DEVICE_ATTR_WO(send_cmd);

/*
 * Same syntax as send_cmd, but waits for the ACK. The reply, including its
 * payload, is left in /sys/.../response for the next read.
 */
static ssize_t transact_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);
	unsigned int cmd, arg = 0;
	u8 payload;
	int parsed, ret;

	parsed = sscanf(buf, "%x %x", &cmd, &arg);
	if (parsed < 1 || cmd > 0xffU || (parsed == 2 && arg > 0xffU))
		return -EINVAL;

	payload = arg;
	mutex_lock(&mcu->cmd_lock);
	ret = s5l8947x_mcu_transact_n(mcu, cmd, &payload, parsed == 2 ? 1 : 0,
				      NULL, NULL, MCU_RETRIES);
	mutex_unlock(&mcu->cmd_lock);
	if (ret < 0)
		return ret;
	return count;
}
static DEVICE_ATTR_WO(transact);

/* 0 = media key codes, 1 = game key codes (apple,game-keys in the DT) */
static ssize_t game_map_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);
	bool on;

	spin_lock(&mcu->lock);
	on = mcu->game_map;
	spin_unlock(&mcu->lock);
	return sysfs_emit(buf, "%u\n", on);
}

static ssize_t game_map_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	spin_lock(&mcu->lock);
	mcu->game_map = on;
	spin_unlock(&mcu->lock);
	return count;
}
static DEVICE_ATTR_RW(game_map);

static struct attribute *s5l8947x_mcu_attrs[] = {
	&dev_attr_fw_version.attr,
	&dev_attr_response.attr,
	&dev_attr_rx_count.attr,
	&dev_attr_crc_errors.attr,
	&dev_attr_late_acks.attr,
	&dev_attr_timeouts.attr,
	&dev_attr_nmi_count.attr,
	&dev_attr_last_packet.attr,
	&dev_attr_events.attr,
	&dev_attr_send_cmd.attr,
	&dev_attr_transact.attr,
	&dev_attr_game_map.attr,
	NULL,
};
ATTRIBUTE_GROUPS(s5l8947x_mcu);

static void s5l8947x_mcu_cancel_key_release(void *data)
{
	struct s5l8947x_mcu *mcu = data;

	cancel_delayed_work_sync(&mcu->key_release);
}

static int s5l8947x_mcu_setup_nmi(struct s5l8947x_mcu *mcu)
{
	struct device *dev = &mcu->serdev->dev;
	int ret;

	mcu->nmi_irq = of_irq_get_byname(dev->of_node, "nmi");
	if (mcu->nmi_irq == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	if (mcu->nmi_irq <= 0)
		return 0;

	ret = devm_request_irq(dev, mcu->nmi_irq, s5l8947x_mcu_nmi,
			       IRQF_TRIGGER_FALLING, dev_name(dev), mcu);
	if (ret) {
		dev_warn(dev, "cannot request the MCU NMI line: %d\n", ret);
		mcu->nmi_irq = 0;
	}

	return 0;
}

static int __maybe_unused s5l8947x_mcu_suspend(struct device *dev)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);

	if (mcu->nmi_irq > 0 && device_may_wakeup(dev) &&
	    !enable_irq_wake(mcu->nmi_irq))
		mcu->wake_enabled = true;

	return 0;
}

static int __maybe_unused s5l8947x_mcu_resume(struct device *dev)
{
	struct s5l8947x_mcu *mcu = dev_get_drvdata(dev);

	if (mcu->wake_enabled) {
		disable_irq_wake(mcu->nmi_irq);
		mcu->wake_enabled = false;
	}

	return 0;
}

static const struct dev_pm_ops s5l8947x_mcu_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(s5l8947x_mcu_suspend, s5l8947x_mcu_resume)
};

static int s5l8947x_mcu_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct s5l8947x_mcu *mcu;
	char fw_version[MCU_MAX_PAYLOAD + 1];
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
	atomic_set(&mcu->nmi_count, 0);
	serdev_device_set_drvdata(serdev, mcu);

	if (!of_property_read_u32(dev->of_node, "apple,recovery-command",
				  &mcu->recovery_cmd) &&
	    mcu->recovery_cmd > 0xffU) {
		dev_warn(dev, "ignoring out-of-range recovery command 0x%x\n",
			 mcu->recovery_cmd);
		mcu->recovery_cmd = 0;
	}

	mcu->game_map = device_property_read_bool(dev, "apple,game-keys");

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

	ret = s5l8947x_mcu_setup_nmi(mcu);
	if (ret)
		return ret;

	if (device_property_read_bool(dev, "wakeup-source"))
		device_init_wakeup(dev, true);

	mcu->reboot_nb.notifier_call = s5l8947x_mcu_reboot_notify;
	ret = devm_register_reboot_notifier(dev, &mcu->reboot_nb);
	if (ret)
		return ret;

	mutex_lock(&mcu->cmd_lock);
	s5l8947x_mcu_handshake(mcu);
	spin_lock(&mcu->lock);
	strscpy(fw_version, mcu->fw_version, sizeof(fw_version));
	spin_unlock(&mcu->lock);
	mutex_unlock(&mcu->cmd_lock);

	dev_info(dev, "IR remote / system MCU on UART at %u baud, firmware %s\n",
		 speed, fw_version);
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
		.pm		= pm_ptr(&s5l8947x_mcu_pm_ops),
	},
};
module_serdev_device_driver(s5l8947x_mcu_driver);

MODULE_DESCRIPTION("S5L8947X system / IR remote MCU driver");
MODULE_LICENSE("GPL");
