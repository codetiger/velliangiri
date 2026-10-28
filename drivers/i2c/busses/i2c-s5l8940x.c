// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8930X / S5L8940X / S5L8947X I2C controller driver
 *
 * Register layout (e.g. i2c0 at 0x33200000):
 *   0x00  MTXFIFO    write byte + START(0x100) / STOP(0x200) / READ(0x400)
 *   0x04  MRXFIFO    [7:0] = data byte, bit 8 = FIFO empty
 *   0x08  SMSTA      status bits (XEN=bit 27, XBUSY=bit 28, ERR=0x8007ff00)
 *   0x0c  IMASK      interrupt mask
 *   0x10  CTL        [7:0] = clkdiv, bit 8 = MTR, bit 9 = MRR, bit 10 = UJM
 *   0x14  REV        controller revision
 *   0x28  FILTER     glitch filter control
 *
 * Transfers are polled; the controller interrupt is not used.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define REG_MTXFIFO		0x00
#define  REG_MTXFIFO_DATA_MASK	0xff
#define  REG_MTXFIFO_START	0x100
#define  REG_MTXFIFO_STOP	0x200
#define  REG_MTXFIFO_READ	0x400

#define REG_MRXFIFO		0x04
#define  REG_MRXFIFO_DATA_MASK	0xff
#define  REG_MRXFIFO_EMPTY	0x100

#define REG_SMSTA		0x08
#define  REG_SMSTA_MTN		0x00200000
#define  REG_SMSTA_XEN		0x08000000
#define  REG_SMSTA_XBUSY	0x10000000
#define  REG_SMSTA_ERR		0x8007ff00

#define REG_IMASK		0x0c

#define REG_CTL			0x10
#define  REG_CTL_CLK_MASK	0xff
#define  REG_CTL_MTR		0x100
#define  REG_CTL_MRR		0x200
#define  REG_CTL_UJM		0x400

#define REG_REV			0x14
#define REG_FILTER		0x28
#define  REG_FILTER_EN		0x100

#define DEFAULT_FREQUENCY	100000

struct s5l8940x_i2c {
	struct device *dev;
	struct i2c_adapter adap;

	void __iomem *base;
	u32 frequency;
	unsigned int clkdiv;
	unsigned int version;
};

static inline void i2c_s5l_wr(struct s5l8940x_i2c *ai, unsigned int reg, u32 val)
{
	writel_relaxed(val, ai->base + reg);
}

static inline u32 i2c_s5l_rd(struct s5l8940x_i2c *ai, unsigned int reg)
{
	return readl_relaxed(ai->base + reg);
}

static void s5l8940x_i2c_set_ctl(struct s5l8940x_i2c *ai)
{
	i2c_s5l_wr(ai, REG_CTL,
		   ai->clkdiv | REG_CTL_MTR | REG_CTL_MRR |
		   (ai->version >= 6 ? REG_CTL_UJM : 0));
}

/* Wait for the queued FIFO sequence to finish and decode its status */
static int s5l8940x_i2c_wait_complete(struct s5l8940x_i2c *ai)
{
	u32 val;
	int err;

	err = readl_relaxed_poll_timeout(ai->base + REG_SMSTA, val,
					 (val & (REG_SMSTA_XEN | REG_SMSTA_ERR)) ||
					 !(val & REG_SMSTA_XBUSY),
					 20, 100000);
	if (val)
		i2c_s5l_wr(ai, REG_SMSTA, val);
	if (err)
		return err;
	if (val & REG_SMSTA_MTN)
		return -ENXIO;
	if (val & REG_SMSTA_ERR)
		return -EIO;
	return 0;
}

static int s5l8940x_i2c_xfer_msg(struct s5l8940x_i2c *ai, struct i2c_msg *msg,
				 int stop)
{
	u32 status;
	unsigned int i;
	int ret;

	s5l8940x_i2c_set_ctl(ai);

	if (msg->flags & I2C_M_RD) {
		i2c_s5l_wr(ai, REG_MTXFIFO,
			   REG_MTXFIFO_START | (msg->addr << 1) | 1);
		i2c_s5l_wr(ai, REG_MTXFIFO,
			   REG_MTXFIFO_READ | msg->len |
			   (stop ? REG_MTXFIFO_STOP : 0));
		ret = s5l8940x_i2c_wait_complete(ai);
		if (ret)
			return ret;
		for (i = 0; i < msg->len; i++) {
			status = i2c_s5l_rd(ai, REG_MRXFIFO);
			if (status & REG_MRXFIFO_EMPTY)
				return -EIO;
			msg->buf[i] = status & REG_MRXFIFO_DATA_MASK;
		}
	} else {
		i2c_s5l_wr(ai, REG_MTXFIFO,
			   REG_MTXFIFO_START | (msg->addr << 1) |
			   (msg->len == 0 && stop ? REG_MTXFIFO_STOP : 0));
		for (i = 0; i < msg->len; i++) {
			i2c_s5l_wr(ai, REG_MTXFIFO,
				   msg->buf[i] |
				   (i == (unsigned int)(msg->len - 1) && stop ?
				    REG_MTXFIFO_STOP : 0));
		}
		ret = s5l8940x_i2c_wait_complete(ai);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Combined write + read (repeated-START) transfer in a single FIFO sequence.
 * Required by slave devices (such as the Dialog D1815 PMU at 0x74) that reset
 * their internal register pointer if a STOP condition occurs between the
 * register address write and the data read.
 */
static int s5l8940x_i2c_xfer_combined(struct s5l8940x_i2c *ai,
				      struct i2c_msg *wmsg,
				      struct i2c_msg *rmsg)
{
	u32 status = 0;
	unsigned int i;
	int ret;

	i2c_s5l_wr(ai, REG_SMSTA, i2c_s5l_rd(ai, REG_SMSTA));
	s5l8940x_i2c_set_ctl(ai);

	/* 1. Queue write phase without STOP */
	i2c_s5l_wr(ai, REG_MTXFIFO, REG_MTXFIFO_START | (wmsg->addr << 1));
	for (i = 0; i < wmsg->len; i++)
		i2c_s5l_wr(ai, REG_MTXFIFO, wmsg->buf[i]);

	/* 2. Queue repeated-START + read + STOP */
	i2c_s5l_wr(ai, REG_MTXFIFO, REG_MTXFIFO_START | (rmsg->addr << 1) | 1);
	i2c_s5l_wr(ai, REG_MTXFIFO,
		   REG_MTXFIFO_READ | rmsg->len | REG_MTXFIFO_STOP);

	ret = s5l8940x_i2c_wait_complete(ai);
	if (ret)
		return ret;

	for (i = 0; i < rmsg->len; i++) {
		int wait = 1000;

		while (wait-- > 0) {
			status = i2c_s5l_rd(ai, REG_MRXFIFO);
			if (!(status & REG_MRXFIFO_EMPTY))
				break;
			udelay(5);
		}
		if (status & REG_MRXFIFO_EMPTY)
			return -EIO;
		rmsg->buf[i] = status & REG_MRXFIFO_DATA_MASK;
	}
	return 0;
}

static int s5l8940x_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			     int num)
{
	struct s5l8940x_i2c *ai = adap->algo_data;
	int i, err;

	i2c_s5l_wr(ai, REG_IMASK, 0);

	if (num == 2 &&
	    !(msgs[0].flags & I2C_M_RD) &&
	    (msgs[1].flags & I2C_M_RD) &&
	    msgs[0].addr == msgs[1].addr &&
	    msgs[0].len <= 4 && msgs[1].len <= 16) {
		err = s5l8940x_i2c_xfer_combined(ai, &msgs[0], &msgs[1]);
		if (err)
			return err;
		return 2;
	}

	for (i = 0; i < num; i++) {
		err = s5l8940x_i2c_xfer_msg(ai, &msgs[i], i == (num - 1));
		if (err)
			return err;
	}
	return num;
}

static u32 s5l8940x_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm s5l8940x_i2c_algo = {
	.xfer		= s5l8940x_i2c_xfer,
	.functionality	= s5l8940x_i2c_func,
};

static int s5l8940x_i2c_probe(struct platform_device *pdev)
{
	struct s5l8940x_i2c *ai;
	struct device_node *np = pdev->dev.of_node;
	struct clk *clk;
	u32 clkrate;
	int err;

	ai = devm_kzalloc(&pdev->dev, sizeof(*ai), GFP_KERNEL);
	if (!ai)
		return -ENOMEM;
	ai->dev = &pdev->dev;

	ai->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ai->base))
		return PTR_ERR(ai->base);

	clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk), "failed to enable clock\n");

	if (of_property_read_u32(np, "clock-frequency", &ai->frequency))
		ai->frequency = DEFAULT_FREQUENCY;

	clkrate = clk_get_rate(clk);
	ai->clkdiv = clamp(DIV_ROUND_UP(clkrate, 16 * ai->frequency), 4U, 255U);

	platform_set_drvdata(pdev, ai);

	ai->version = i2c_s5l_rd(ai, REG_REV);
	s5l8940x_i2c_set_ctl(ai);
	i2c_s5l_wr(ai, REG_IMASK, 0);
	i2c_s5l_wr(ai, REG_SMSTA, i2c_s5l_rd(ai, REG_SMSTA));
	i2c_s5l_wr(ai, REG_FILTER, i2c_s5l_rd(ai, REG_FILTER) | REG_FILTER_EN);

	ai->adap.owner = THIS_MODULE;
	strscpy(ai->adap.name, "s5l8940x-i2c", sizeof(ai->adap.name));
	ai->adap.algo = &s5l8940x_i2c_algo;
	ai->adap.algo_data = ai;
	ai->adap.dev.parent = &pdev->dev;
	ai->adap.dev.of_node = np;

	err = i2c_add_adapter(&ai->adap);
	if (err)
		return err;

	dev_info(&pdev->dev, "S5L8940X I2C adapter rev %u (clkdiv=%u)\n",
		 ai->version, ai->clkdiv);
	return 0;
}

static void s5l8940x_i2c_remove(struct platform_device *pdev)
{
	struct s5l8940x_i2c *ai = platform_get_drvdata(pdev);

	i2c_del_adapter(&ai->adap);
}

static const struct of_device_id s5l8940x_i2c_match[] = {
	{ .compatible = "apple,s5l8930x-i2c" },
	{ .compatible = "apple,s5l8940x-i2c" },
	{ .compatible = "apple,s5l8947x-i2c" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l8940x_i2c_match);

static struct platform_driver s5l8940x_i2c_driver = {
	.probe		= s5l8940x_i2c_probe,
	.remove		= s5l8940x_i2c_remove,
	.driver		= {
		.name		= "i2c-s5l8940x",
		.of_match_table	= s5l8940x_i2c_match,
	},
};
module_platform_driver(s5l8940x_i2c_driver);

MODULE_DESCRIPTION("S5L8930X/S5L8940X/S5L8947X I2C master driver");
MODULE_LICENSE("GPL");
