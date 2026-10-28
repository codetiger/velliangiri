// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X 10/100 Fast Ethernet MAC driver (Arasan 10/100 MAC + SMSC LAN8742 PHY)
 *
 * Register map at 0x3b000000 (AIC IRQ 27, PMGR gate 79 at 0x3f101144):
 *   DMA block (+0x000):
 *     +0x00  DMA_CONFIG       bit 0 soft reset, 0x410 normal operation
 *     +0x04  DMA_CONTROL      bit 0 start TX DMA, bit 1 start RX DMA
 *     +0x08  DMA_STATUS       write-1-to-clear; bit 0 TX, bit 4 RX, bit 5 RX nobuf, bit 8 MAC
 *     +0x0c  DMA_INT_ENABLE   0x1f1 enabled mask
 *     +0x14  DMA_TX_POLL      write 1 after queueing TX frame
 *     +0x18  DMA_RX_POLL      write 1 after refilling RX ring
 *     +0x1c  DMA_TX_BASE      physical address of TX descriptor ring
 *     +0x20  DMA_RX_BASE      physical address of RX descriptor ring
 *     +0x40  DMA_REG40        bit 31 asserted before DMA reset
 *   MAC block (+0x100):
 *     +0x100 MAC_GLOBAL       bit 0 speed 100M, bit 2 full duplex, 0x18 soft reset
 *     +0x104 MAC_TX_CTRL      bit 0 enable, 0x08 default
 *     +0x108 MAC_RX_CTRL      bit 0 enable, 0x18 default
 *     +0x10c MAC_MAX_FRAME
 *     +0x110 MAC_TX_JABBER    +0x114 MAC_RX_JABBER
 *     +0x118 MAC_ADDR_CTRL    1 = enable station address 1 filter
 *     +0x120 MAC_ADDR1_0..4   16-bit station address words (low byte first)
 *     +0x150 MAC_HASH0..3     128-bit multicast hash table
 *     +0x1a0 MAC_MDIO_CTRL    phy | (reg << 5) | 0x400 (read) | bit 15 (start/busy)
 *     +0x1a4 MAC_MDIO_DATA    16-bit MDIO data
 *     +0x1c0 MAC_TX_FIFO_AFULL (0x1f8), +0x1c4 MAC_TX_START_THRESH (0x80)
 *     +0x1e0 MAC_INT_STATUS,  +0x1e4 MAC_INT_ENABLE
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/skbuff.h>

#define DRV_NAME		"s5l-fe"

/* DMA block */
#define DMA_CONFIG		0x000
#define  DMA_CONFIG_RESET	BIT(0)
#define  DMA_CONFIG_DEFAULT	0x410
#define DMA_CONTROL		0x004
#define  DMA_CONTROL_TX		BIT(0)
#define  DMA_CONTROL_RX		BIT(1)
#define DMA_STATUS		0x008
#define DMA_INT_ENABLE		0x00c
#define  DMA_INT_TX_DONE	BIT(0)
#define  DMA_INT_RX_DONE	BIT(4)
#define  DMA_INT_RX_NOBUF	BIT(5)
#define  DMA_INT_BIT6		BIT(6)
#define  DMA_INT_BIT7		BIT(7)
#define  DMA_INT_MAC		BIT(8)
#define  DMA_INT_ALL		0x1f1
#define  DMA_INT_RX		(DMA_INT_RX_DONE | DMA_INT_RX_NOBUF)
#define DMA_TX_AUTO_POLL	0x010
#define DMA_TX_POLL		0x014
#define DMA_RX_POLL		0x018
#define DMA_TX_BASE		0x01c
#define DMA_RX_BASE		0x020
#define DMA_RX_MITIGATION	0x02c
#define DMA_REG40		0x040

/* MAC block */
#define MAC_GLOBAL		0x100
#define  MAC_GLOBAL_100		BIT(0)
#define  MAC_GLOBAL_FDX		BIT(2)
#define  MAC_GLOBAL_RESET	0x18
#define MAC_TX_CTRL		0x104
#define  MAC_TX_CTRL_EN		BIT(0)
#define  MAC_TX_CTRL_DEFAULT	0x08
#define MAC_RX_CTRL		0x108
#define  MAC_RX_CTRL_EN		BIT(0)
#define  MAC_RX_CTRL_DEFAULT	0x18
#define MAC_MAX_FRAME		0x10c
#define MAC_TX_JABBER		0x110
#define MAC_RX_JABBER		0x114
#define MAC_ADDR_CTRL		0x118
#define MAC_MDIO_CLKDIV		0x11c
#define MAC_ADDR1_0		0x120
#define MAC_ADDR1_2		0x124
#define MAC_ADDR1_4		0x128
#define MAC_HASH(n)		(0x150 + 4 * (n))
#define MAC_MDIO_CTRL		0x1a0
#define  MAC_MDIO_READ		BIT(10)
#define  MAC_MDIO_START		BIT(15)
#define MAC_MDIO_DATA		0x1a4
#define MAC_TX_FIFO_AFULL	0x1c0
#define MAC_TX_START_THRESH	0x1c4
#define MAC_INT_STATUS		0x1e0
#define MAC_INT_ENABLE		0x1e4

/* Descriptor flags */
#define DESC_OWN		BIT(31)
#define RX_STATUS_LEN		GENMASK(13, 0)
#define RX_STATUS_ERR		BIT(14)
#define TX_CTRL_CHAIN		BIT(25)
#define TX_CTRL_FIRST		BIT(29)
#define TX_CTRL_LAST		BIT(30)
#define TX_CTRL_IRQ		BIT(31)
#define RX_CTRL_EOR		BIT(26)

#define RING_SIZE		128
#define RX_BUF_SIZE		1536
#define JABBER_SIZE		0x600
#define MAX_FRAME		(ETH_FRAME_LEN + ETH_FCS_LEN + 4)

struct s5l_fe_desc {
	__le32 status;
	__le32 ctrl;
	__le32 buf;
	__le32 next;
	u32 sw[4];
};

struct s5l_fe_buf {
	struct sk_buff *skb;
	dma_addr_t dma;
};

struct s5l_fe {
	struct net_device *ndev;
	struct device *dev;
	void __iomem *base;
	int irq;

	struct mii_bus *mii;
	struct phy_device *phy;
	struct regulator *phy_supply;
	int speed, duplex, link;

	struct napi_struct napi;
	spinlock_t lock;

	struct s5l_fe_desc *tx_ring;
	dma_addr_t tx_ring_dma;
	struct s5l_fe_buf tx_buf[RING_SIZE];
	unsigned int tx_head, tx_tail;

	struct s5l_fe_desc *rx_ring;
	dma_addr_t rx_ring_dma;
	struct s5l_fe_buf rx_buf[RING_SIZE];
	unsigned int rx_next;

	u32 int_mask;
};

static inline u32 fe_rd(struct s5l_fe *p, u32 off)
{
	return readl(p->base + off);
}

static inline void fe_wr(struct s5l_fe *p, u32 off, u32 val)
{
	writel(val, p->base + off);
}

static int s5l_fe_mdio_wait(struct s5l_fe *p)
{
	u32 v;

	return readl_poll_timeout(p->base + MAC_MDIO_CTRL, v,
				  !(v & MAC_MDIO_START), 10, 100000);
}

static int s5l_fe_mdio_read(struct mii_bus *bus, int addr, int reg)
{
	struct s5l_fe *p = bus->priv;
	u32 ctl = (addr & 0x1f) | ((reg & 0x1f) << 5) | MAC_MDIO_READ;

	fe_wr(p, MAC_MDIO_CTRL, ctl);
	fe_wr(p, MAC_MDIO_CTRL, ctl | MAC_MDIO_START);
	if (s5l_fe_mdio_wait(p)) {
		dev_warn_ratelimited(p->dev, "MDIO read %d.%d timed out\n", addr, reg);
		return -ETIMEDOUT;
	}
	return fe_rd(p, MAC_MDIO_DATA) & 0xffff;
}

static int s5l_fe_mdio_write(struct mii_bus *bus, int addr, int reg, u16 val)
{
	struct s5l_fe *p = bus->priv;
	u32 ctl = (addr & 0x1f) | ((reg & 0x1f) << 5);

	fe_wr(p, MAC_MDIO_CTRL, ctl);
	fe_wr(p, MAC_MDIO_DATA, val);
	fe_wr(p, MAC_MDIO_CTRL, ctl | MAC_MDIO_START);
	if (s5l_fe_mdio_wait(p)) {
		dev_warn_ratelimited(p->dev, "MDIO write %d.%d timed out\n", addr, reg);
		return -ETIMEDOUT;
	}
	return 0;
}

static int s5l_fe_rx_refill(struct s5l_fe *p, unsigned int i)
{
	struct s5l_fe_desc *d = &p->rx_ring[i];
	struct sk_buff *skb;
	dma_addr_t dma;

	skb = netdev_alloc_skb(p->ndev, RX_BUF_SIZE + NET_IP_ALIGN);
	if (!skb)
		return -ENOMEM;
	skb_reserve(skb, NET_IP_ALIGN);
	dma = dma_map_single(p->dev, skb->data, RX_BUF_SIZE, DMA_FROM_DEVICE);
	if (dma_mapping_error(p->dev, dma)) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}
	p->rx_buf[i].skb = skb;
	p->rx_buf[i].dma = dma;
	d->buf = cpu_to_le32(dma);
	d->ctrl = cpu_to_le32(RX_BUF_SIZE | (i == RING_SIZE - 1 ? RX_CTRL_EOR : 0));
	d->next = 0;
	dma_wmb();
	d->status = cpu_to_le32(DESC_OWN);
	return 0;
}

static void s5l_fe_free_rings(struct s5l_fe *p)
{
	unsigned int i;

	for (i = 0; i < RING_SIZE; i++) {
		if (p->rx_buf[i].skb) {
			dma_unmap_single(p->dev, p->rx_buf[i].dma, RX_BUF_SIZE,
					 DMA_FROM_DEVICE);
			dev_kfree_skb(p->rx_buf[i].skb);
			p->rx_buf[i].skb = NULL;
		}
		if (p->tx_buf[i].skb) {
			dma_unmap_single(p->dev, p->tx_buf[i].dma,
					 p->tx_buf[i].skb->len, DMA_TO_DEVICE);
			dev_kfree_skb(p->tx_buf[i].skb);
			p->tx_buf[i].skb = NULL;
		}
	}
}

static int s5l_fe_init_rings(struct s5l_fe *p)
{
	unsigned int i;

	memset(p->tx_ring, 0, RING_SIZE * sizeof(*p->tx_ring));
	memset(p->rx_ring, 0, RING_SIZE * sizeof(*p->rx_ring));
	for (i = 0; i < RING_SIZE; i++) {
		p->tx_ring[i].ctrl = cpu_to_le32(TX_CTRL_CHAIN);
		p->tx_ring[i].next = cpu_to_le32(p->tx_ring_dma +
				((i + 1) % RING_SIZE) * sizeof(*p->tx_ring));
	}
	p->tx_head = p->tx_tail = 0;
	p->rx_next = 0;
	for (i = 0; i < RING_SIZE; i++)
		if (s5l_fe_rx_refill(p, i)) {
			s5l_fe_free_rings(p);
			return -ENOMEM;
		}
	wmb();
	return 0;
}

static void s5l_fe_set_mac_addr(struct s5l_fe *p)
{
	const u8 *a = p->ndev->dev_addr;

	fe_wr(p, MAC_ADDR1_0, a[0] | (a[1] << 8));
	fe_wr(p, MAC_ADDR1_2, a[2] | (a[3] << 8));
	fe_wr(p, MAC_ADDR1_4, a[4] | (a[5] << 8));
	fe_wr(p, MAC_ADDR_CTRL, 1);
}

static void s5l_fe_set_rx_mode(struct net_device *ndev)
{
	struct s5l_fe *p = netdev_priv(ndev);
	int i;

	for (i = 0; i < 4; i++)
		fe_wr(p, MAC_HASH(i), 0xffffffffU);
}

static void s5l_fe_hw_init(struct s5l_fe *p)
{
	fe_wr(p, DMA_REG40, BIT(31));
	(void)fe_rd(p, DMA_REG40);

	fe_wr(p, DMA_CONFIG, DMA_CONFIG_RESET);
	fe_wr(p, DMA_CONFIG, 0);
	fe_wr(p, DMA_CONFIG, DMA_CONFIG_DEFAULT);

	fe_wr(p, MAC_GLOBAL, MAC_GLOBAL_RESET);
	(void)fe_rd(p, MAC_GLOBAL);
	fe_wr(p, MAC_GLOBAL, 0);

	fe_wr(p, MAC_TX_START_THRESH, 0x80);
	fe_wr(p, MAC_TX_FIFO_AFULL, 0x1f8);
	fe_wr(p, MAC_TX_CTRL, MAC_TX_CTRL_DEFAULT);
	fe_wr(p, MAC_RX_CTRL, MAC_RX_CTRL_DEFAULT);

	s5l_fe_set_mac_addr(p);
	fe_wr(p, MAC_MAX_FRAME, MAX_FRAME);
	fe_wr(p, MAC_TX_JABBER, JABBER_SIZE);
	fe_wr(p, MAC_RX_JABBER, JABBER_SIZE);
	s5l_fe_set_rx_mode(p->ndev);

	fe_wr(p, DMA_TX_BASE, p->tx_ring_dma);
	fe_wr(p, DMA_RX_BASE, p->rx_ring_dma);

	fe_wr(p, MAC_INT_ENABLE, 0);
	fe_wr(p, DMA_STATUS, fe_rd(p, DMA_STATUS));
}

static void s5l_fe_hw_start(struct s5l_fe *p)
{
	fe_wr(p, MAC_RX_CTRL, fe_rd(p, MAC_RX_CTRL) | MAC_RX_CTRL_EN);
	fe_wr(p, MAC_TX_CTRL, fe_rd(p, MAC_TX_CTRL) | MAC_TX_CTRL_EN);
	fe_wr(p, DMA_CONTROL, fe_rd(p, DMA_CONTROL) | DMA_CONTROL_TX | DMA_CONTROL_RX);
	p->int_mask = DMA_INT_ALL;
	fe_wr(p, DMA_INT_ENABLE, p->int_mask);
	fe_wr(p, DMA_RX_POLL, 1);
}

static void s5l_fe_hw_stop(struct s5l_fe *p)
{
	fe_wr(p, DMA_INT_ENABLE, 0);
	p->int_mask = 0;
	fe_wr(p, DMA_CONTROL, fe_rd(p, DMA_CONTROL) & ~(DMA_CONTROL_TX | DMA_CONTROL_RX));
	fe_wr(p, DMA_STATUS, fe_rd(p, DMA_STATUS));
	fe_wr(p, MAC_RX_CTRL, fe_rd(p, MAC_RX_CTRL) & ~MAC_RX_CTRL_EN);
	fe_wr(p, MAC_TX_CTRL, fe_rd(p, MAC_TX_CTRL) & ~MAC_TX_CTRL_EN);
}

static void s5l_fe_adjust_link(struct net_device *ndev)
{
	struct s5l_fe *p = netdev_priv(ndev);
	struct phy_device *phy = p->phy;
	u32 v = 0;

	if (phy->link == p->link && phy->speed == p->speed &&
	    phy->duplex == p->duplex)
		return;
	p->link = phy->link;
	p->speed = phy->speed;
	p->duplex = phy->duplex;

	if (phy->link) {
		if (phy->speed == SPEED_100)
			v |= MAC_GLOBAL_100;
		if (phy->duplex == DUPLEX_FULL)
			v |= MAC_GLOBAL_FDX;
		fe_wr(p, MAC_GLOBAL, v);
	}
	phy_print_status(phy);
}

static void s5l_fe_tx_complete(struct s5l_fe *p)
{
	struct net_device *ndev = p->ndev;
	unsigned int pkts = 0, bytes = 0;

	spin_lock(&p->lock);
	while (p->tx_tail != p->tx_head) {
		struct s5l_fe_desc *d = &p->tx_ring[p->tx_tail];
		struct s5l_fe_buf *b = &p->tx_buf[p->tx_tail];

		if (le32_to_cpu(d->status) & DESC_OWN)
			break;
		dma_rmb();
		dma_unmap_single(p->dev, b->dma, b->skb->len, DMA_TO_DEVICE);
		pkts++;
		bytes += b->skb->len;
		dev_kfree_skb_any(b->skb);
		b->skb = NULL;
		p->tx_tail = (p->tx_tail + 1) % RING_SIZE;
	}
	spin_unlock(&p->lock);
	if (pkts) {
		ndev->stats.tx_packets += pkts;
		ndev->stats.tx_bytes += bytes;
		if (netif_queue_stopped(ndev))
			netif_wake_queue(ndev);
	}
}

static netdev_tx_t s5l_fe_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct s5l_fe *p = netdev_priv(ndev);
	struct s5l_fe_desc *d;
	struct s5l_fe_buf *b;
	unsigned int next;
	dma_addr_t dma;

	if (skb_padto(skb, ETH_ZLEN))
		return NETDEV_TX_OK;
	if (skb->len < ETH_ZLEN)
		skb->len = ETH_ZLEN;

	dma = dma_map_single(p->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(p->dev, dma)) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	spin_lock(&p->lock);
	next = (p->tx_head + 1) % RING_SIZE;
	if (next == p->tx_tail) {
		spin_unlock(&p->lock);
		dma_unmap_single(p->dev, dma, skb->len, DMA_TO_DEVICE);
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}
	d = &p->tx_ring[p->tx_head];
	b = &p->tx_buf[p->tx_head];
	b->skb = skb;
	b->dma = dma;
	d->buf = cpu_to_le32(dma);
	d->ctrl = cpu_to_le32(TX_CTRL_CHAIN | TX_CTRL_FIRST | TX_CTRL_LAST |
			      TX_CTRL_IRQ | skb->len);
	dma_wmb();
	d->status = cpu_to_le32(DESC_OWN);
	p->tx_head = next;
	if (((next + 1) % RING_SIZE) == p->tx_tail)
		netif_stop_queue(ndev);
	spin_unlock(&p->lock);

	skb_tx_timestamp(skb);
	wmb();
	fe_wr(p, DMA_TX_POLL, 1);
	return NETDEV_TX_OK;
}

static int s5l_fe_poll(struct napi_struct *napi, int budget)
{
	struct s5l_fe *p = container_of(napi, struct s5l_fe, napi);
	struct net_device *ndev = p->ndev;
	int done = 0;

	while (done < budget) {
		unsigned int i = p->rx_next;
		struct s5l_fe_desc *d = &p->rx_ring[i];
		struct s5l_fe_buf *b = &p->rx_buf[i];
		u32 status = le32_to_cpu(d->status);
		unsigned int len;
		struct sk_buff *skb;

		if (status & DESC_OWN)
			break;
		dma_rmb();
		len = status & RX_STATUS_LEN;
		if ((status & RX_STATUS_ERR) || len < ETH_FCS_LEN + ETH_HLEN ||
		    len > RX_BUF_SIZE) {
			ndev->stats.rx_errors++;
			if (status & RX_STATUS_ERR)
				ndev->stats.rx_crc_errors++;
			dma_wmb();
			d->status = cpu_to_le32(DESC_OWN);
		} else {
			skb = b->skb;
			dma_unmap_single(p->dev, b->dma, RX_BUF_SIZE, DMA_FROM_DEVICE);
			b->skb = NULL;
			skb_put(skb, len - ETH_FCS_LEN);
			skb->protocol = eth_type_trans(skb, ndev);
			ndev->stats.rx_packets++;
			ndev->stats.rx_bytes += skb->len;
			napi_gro_receive(napi, skb);
			if (s5l_fe_rx_refill(p, i)) {
				ndev->stats.rx_dropped++;
				break;
			}
		}
		p->rx_next = (i + 1) % RING_SIZE;
		done++;
	}
	wmb();
	fe_wr(p, DMA_RX_POLL, 1);

	if (done < budget && napi_complete_done(napi, done)) {
		unsigned long flags;

		spin_lock_irqsave(&p->lock, flags);
		p->int_mask |= DMA_INT_RX;
		fe_wr(p, DMA_INT_ENABLE, p->int_mask);
		spin_unlock_irqrestore(&p->lock, flags);
	}
	return done;
}

static irqreturn_t s5l_fe_interrupt(int irq, void *dev_id)
{
	struct s5l_fe *p = dev_id;
	u32 status = fe_rd(p, DMA_STATUS);

	if (!(status & p->int_mask) && !(status & DMA_INT_ALL))
		return IRQ_NONE;
	fe_wr(p, DMA_STATUS, status);

	if (status & DMA_INT_MAC)
		fe_wr(p, MAC_INT_STATUS, fe_rd(p, MAC_INT_STATUS));
	if (status & DMA_INT_TX_DONE)
		s5l_fe_tx_complete(p);
	if (status & DMA_INT_RX) {
		spin_lock(&p->lock);
		p->int_mask &= ~DMA_INT_RX;
		fe_wr(p, DMA_INT_ENABLE, p->int_mask);
		spin_unlock(&p->lock);
		napi_schedule(&p->napi);
	}
	if (status & (DMA_INT_BIT6 | DMA_INT_BIT7))
		net_warn_ratelimited("%s: DMA status 0x%08x\n", p->ndev->name, status);
	return IRQ_HANDLED;
}

static int s5l_fe_open(struct net_device *ndev)
{
	struct s5l_fe *p = netdev_priv(ndev);
	int ret;

	ret = s5l_fe_init_rings(p);
	if (ret)
		return ret;
	s5l_fe_hw_init(p);

	ret = request_irq(p->irq, s5l_fe_interrupt, 0, ndev->name, p);
	if (ret) {
		s5l_fe_free_rings(p);
		return ret;
	}
	p->link = 0;
	p->speed = 0;
	p->duplex = -1;
	napi_enable(&p->napi);
	s5l_fe_hw_start(p);
	phy_start(p->phy);
	netif_start_queue(ndev);
	return 0;
}

static int s5l_fe_stop(struct net_device *ndev)
{
	struct s5l_fe *p = netdev_priv(ndev);

	netif_stop_queue(ndev);
	phy_stop(p->phy);
	napi_disable(&p->napi);
	s5l_fe_hw_stop(p);
	free_irq(p->irq, p);
	s5l_fe_free_rings(p);
	return 0;
}

static void s5l_fe_tx_timeout(struct net_device *ndev, unsigned int txq)
{
	struct s5l_fe *p = netdev_priv(ndev);

	netdev_warn(ndev, "TX timeout: DMA status 0x%08x ctl 0x%08x head %u tail %u\n",
		    fe_rd(p, DMA_STATUS), fe_rd(p, DMA_CONTROL), p->tx_head, p->tx_tail);
	ndev->stats.tx_errors++;
	fe_wr(p, DMA_TX_POLL, 1);
}

static const struct net_device_ops s5l_fe_netdev_ops = {
	.ndo_open		= s5l_fe_open,
	.ndo_stop		= s5l_fe_stop,
	.ndo_start_xmit		= s5l_fe_start_xmit,
	.ndo_set_rx_mode	= s5l_fe_set_rx_mode,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_tx_timeout		= s5l_fe_tx_timeout,
	.ndo_eth_ioctl		= phy_do_ioctl_running,
};

static int s5l_fe_mdio_init(struct s5l_fe *p)
{
	struct device_node *np = p->dev->of_node, *mdio_np;
	struct mii_bus *bus;
	int ret;

	bus = devm_mdiobus_alloc(p->dev);
	if (!bus)
		return -ENOMEM;
	bus->name = DRV_NAME "-mdio";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(p->dev));
	bus->priv = p;
	bus->read = s5l_fe_mdio_read;
	bus->write = s5l_fe_mdio_write;
	bus->parent = p->dev;

	mdio_np = of_get_child_by_name(np, "mdio");
	ret = of_mdiobus_register(bus, mdio_np);
	of_node_put(mdio_np);
	if (ret) {
		dev_err(p->dev, "MDIO bus registration failed: %d\n", ret);
		return ret;
	}
	p->mii = bus;
	return 0;
}

static int s5l_fe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct net_device *ndev;
	struct s5l_fe *p;
	struct clk *clk;
	int ret;

	ndev = devm_alloc_etherdev(dev, sizeof(*p));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, dev);
	platform_set_drvdata(pdev, ndev);
	p = netdev_priv(ndev);
	p->ndev = ndev;
	p->dev = dev;
	spin_lock_init(&p->lock);

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);
	p->irq = platform_get_irq(pdev, 0);
	if (p->irq < 0)
		return p->irq;

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "failed to enable clock\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	fe_wr(p, DMA_CONFIG, DMA_CONFIG_RESET);
	fe_wr(p, DMA_CONFIG, 0);
	fe_wr(p, DMA_CONFIG, DMA_CONFIG_DEFAULT);
	fe_wr(p, MAC_GLOBAL, MAC_GLOBAL_RESET);
	fe_wr(p, MAC_GLOBAL, 0);

	p->tx_ring = dmam_alloc_coherent(dev, RING_SIZE * sizeof(*p->tx_ring),
					 &p->tx_ring_dma, GFP_KERNEL);
	p->rx_ring = dmam_alloc_coherent(dev, RING_SIZE * sizeof(*p->rx_ring),
					 &p->rx_ring_dma, GFP_KERNEL);
	if (!p->tx_ring || !p->rx_ring)
		return -ENOMEM;

	ret = of_get_ethdev_address(np, ndev);
	if (ret) {
		eth_hw_addr_random(ndev);
		dev_info(dev, "no MAC address in the device tree, using %pM\n",
			 ndev->dev_addr);
	}

	p->phy_supply = devm_regulator_get_optional(dev, "phy");
	if (IS_ERR(p->phy_supply)) {
		if (PTR_ERR(p->phy_supply) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		p->phy_supply = NULL;
	} else {
		ret = regulator_enable(p->phy_supply);
		if (ret)
			return ret;
		msleep(50);
	}

	ret = s5l_fe_mdio_init(p);
	if (ret)
		return ret;

	p->phy = of_phy_get_and_connect(ndev, np, s5l_fe_adjust_link);
	if (!p->phy) {
		dev_err(dev, "no PHY found\n");
		ret = -ENODEV;
		goto err_mdio;
	}
	phy_attached_info(p->phy);

	ndev->netdev_ops = &s5l_fe_netdev_ops;
	ndev->watchdog_timeo = 5 * HZ;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = ETH_DATA_LEN;
	netif_napi_add(ndev, &p->napi, s5l_fe_poll);

	ret = register_netdev(ndev);
	if (ret)
		goto err_phy;
	dev_info(dev, "Arasan 10/100 MAC at %pR, irq %d, %pM\n",
		 platform_get_resource(pdev, IORESOURCE_MEM, 0), p->irq, ndev->dev_addr);
	return 0;

err_phy:
	phy_disconnect(p->phy);
err_mdio:
	mdiobus_unregister(p->mii);
	return ret;
}

static void s5l_fe_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct s5l_fe *p = netdev_priv(ndev);

	unregister_netdev(ndev);
	phy_disconnect(p->phy);
	mdiobus_unregister(p->mii);
	if (p->phy_supply)
		regulator_disable(p->phy_supply);
}

static const struct of_device_id s5l_fe_of_match[] = {
	{ .compatible = "apple,s5l8947x-ethernet" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5l_fe_of_match);

static struct platform_driver s5l_fe_driver = {
	.probe	= s5l_fe_probe,
	.remove	= s5l_fe_remove,
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= s5l_fe_of_match,
	},
};
module_platform_driver(s5l_fe_driver);

MODULE_DESCRIPTION("S5L8947X 10/100 Fast Ethernet MAC driver");
MODULE_LICENSE("GPL");
