// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X 10/100 Fast Ethernet MAC driver (Arasan 10/100 EMAC + SMSC LAN8742 PHY)
 *
 * Register map at 0x3b000000 (AIC IRQ 27, PMGR gate 79):
 *   0x000  DMA_CTRL       bit 0 = TX start, bit 1 = RX start, bit 8 = soft reset
 *   0x004  TX_POLL        write any value to poll TX descriptor ring
 *   0x00c  DMA_BUS        burst / priority config (0x00000100)
 *   0x010  TX_RING_BASE   physical address of TX descriptor ring
 *   0x014  RX_RING_BASE   physical address of RX descriptor ring
 *   0x018  INT_STATUS     read-only status (masked by INT_ENABLE)
 *   0x01c  INT_ENABLE     1 = enabled; write 0 followed by mask to ack + re-arm
 *   0x020  TX_MISSED
 *   0x100  MAC_CTRL       bit 0 = TX_EN, bit 1 = RX_EN, bit 2 = FULL_DUPLEX,
 *                         bit 3 = SPEED_100, bit 5 = PROMISC, bit 6 = BCAST,
 *                         bit 7 = MCAST, bit 12 = PAUSE
 *   0x104  MAC_ADDR_L     MAC bytes [0..3]
 *   0x108  MAC_ADDR_H     MAC bytes [4..5] in bits [15:0]
 *   0x10c  MCAST_HASH0..3 128-bit multicast hash table (0x10c..0x118)
 *   0x11c  MDIO_CTRL      [4:0] = phy, [9:5] = reg, bit 10 = write, bit 15 = busy
 *   0x120  MDIO_DATA      16-bit MDIO read/write payload
 *   0x134  PAUSE_TIME     pause quanta (0xffff)
 *   0x138  FIFO_THR       TX/RX FIFO thresholds (0x00200038)
 *   0x13c  FLOW_THR       flow-control thresholds (0x00c00010)
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
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

/* DMA registers */
#define FE_DMA_CTRL		0x000
#define  FE_DMA_TX_START	BIT(0)
#define  FE_DMA_RX_START	BIT(1)
#define  FE_DMA_SWRST		BIT(8)
#define FE_TX_POLL		0x004
#define FE_DMA_BUS		0x00c
#define FE_TX_BASE		0x010
#define FE_RX_BASE		0x014
#define FE_INT_STATUS		0x018
#define FE_INT_ENABLE		0x01c
#define  FE_INT_TX_DONE		BIT(0)
#define  FE_INT_RX_DONE		BIT(2)
#define  FE_INT_RX_NOBUF	BIT(3)
#define  FE_INT_RX_ERR		BIT(7)
#define  FE_INT_TX_UNDERFLOW	BIT(9)
#define  FE_INT_MASK_DEFAULT	(FE_INT_TX_DONE | FE_INT_RX_DONE | \
				 FE_INT_RX_NOBUF | FE_INT_RX_ERR | \
				 FE_INT_TX_UNDERFLOW)
#define FE_TX_MISSED		0x020

/* MAC registers */
#define FE_MAC_CTRL		0x100
#define  FE_MAC_TX_EN		BIT(0)
#define  FE_MAC_RX_EN		BIT(1)
#define  FE_MAC_FULL_DUPLEX	BIT(2)
#define  FE_MAC_SPEED_100	BIT(3)
#define  FE_MAC_PROMISC		BIT(5)
#define  FE_MAC_BCAST		BIT(6)
#define  FE_MAC_MCAST		BIT(7)
#define  FE_MAC_PAUSE		BIT(12)
#define FE_MAC_ADDR_L		0x104
#define FE_MAC_ADDR_H		0x108
#define FE_MCAST_HASH0		0x10c
#define FE_MCAST_HASH1		0x110
#define FE_MCAST_HASH2		0x114
#define FE_MCAST_HASH3		0x118
#define FE_MDIO_CTRL		0x11c
#define  FE_MDIO_WRITE		BIT(10)
#define  FE_MDIO_BUSY		BIT(15)
#define FE_MDIO_DATA		0x120
#define FE_PAUSE_TIME		0x134
#define FE_FIFO_THR		0x138
#define FE_FLOW_THR		0x13c
#define FE_STAT_BASE		0x200

/* Descriptor ring sizes and bit definitions */
#define FE_RX_RING_SIZE		64
#define FE_TX_RING_SIZE		32
#define FE_RX_BUF_SIZE		1536

#define FE_DESC_OWN		BIT(31)

/* RX descriptor status/control bits */
#define FE_RXD_LEN_MASK		0x7ffU
#define FE_RXD_CRC_ERR		BIT(14)
#define FE_RXD_RUNT_ERR		BIT(19)
#define FE_RXD_ALIGN_ERR	BIT(20)
#define FE_RXD_ERR_MASK		(FE_RXD_CRC_ERR | FE_RXD_RUNT_ERR | FE_RXD_ALIGN_ERR)
#define FE_RXD_LS		BIT(28)
#define FE_RXD_FS		BIT(29)
#define FE_RXD_EOR		BIT(26)

/* TX descriptor control bits */
#define FE_TXD_LEN_MASK		0x7ffU
#define FE_TXD_EOR		BIT(26)
#define FE_TXD_FS		BIT(29)
#define FE_TXD_LS		BIT(30)
#define FE_TXD_IOC		BIT(31)

struct s5l_fe_desc {
	__le32 status;
	__le32 ctrl;
	__le32 buf_addr;
	__le32 reserved;
} __aligned(16);

struct s5l_fe_priv {
	void __iomem *base;
	struct net_device *ndev;
	struct device *dev;
	struct clk *clk;
	struct regulator *phy_supply;
	struct mii_bus *mii_bus;
	struct napi_struct napi;
	spinlock_t lock;

	struct s5l_fe_desc *rx_ring;
	dma_addr_t rx_ring_dma;
	struct sk_buff *rx_skbs[FE_RX_RING_SIZE];
	dma_addr_t rx_dmas[FE_RX_RING_SIZE];
	unsigned int rx_head;

	struct s5l_fe_desc *tx_ring;
	dma_addr_t tx_ring_dma;
	struct sk_buff *tx_skbs[FE_TX_RING_SIZE];
	dma_addr_t tx_dmas[FE_TX_RING_SIZE];
	unsigned int tx_head;
	unsigned int tx_tail;

	int irq;
	int link;
	int speed;
	int duplex;
};

static inline u32 fe_rd(struct s5l_fe_priv *priv, u32 reg)
{
	return readl_relaxed(priv->base + reg);
}

static inline void fe_wr(struct s5l_fe_priv *priv, u32 reg, u32 val)
{
	writel_relaxed(val, priv->base + reg);
}

static int s5l_fe_mdio_wait(struct s5l_fe_priv *priv)
{
	u32 val;

	return readl_relaxed_poll_timeout_atomic(priv->base + FE_MDIO_CTRL,
						 val, !(val & FE_MDIO_BUSY),
						 10, 50000);
}

static int s5l_fe_mdio_read(struct mii_bus *bus, int phy_id, int regnum)
{
	struct s5l_fe_priv *priv = bus->priv;
	u32 cmd;
	int ret;

	ret = s5l_fe_mdio_wait(priv);
	if (ret)
		return ret;

	cmd = (phy_id & 0x1f) | ((regnum & 0x1f) << 5) | FE_MDIO_BUSY;
	fe_wr(priv, FE_MDIO_CTRL, cmd);

	ret = s5l_fe_mdio_wait(priv);
	if (ret)
		return ret;

	return fe_rd(priv, FE_MDIO_DATA) & 0xffff;
}

static int s5l_fe_mdio_write(struct mii_bus *bus, int phy_id, int regnum,
			     u16 val)
{
	struct s5l_fe_priv *priv = bus->priv;
	u32 cmd;
	int ret;

	ret = s5l_fe_mdio_wait(priv);
	if (ret)
		return ret;

	fe_wr(priv, FE_MDIO_DATA, val);
	cmd = (phy_id & 0x1f) | ((regnum & 0x1f) << 5) |
	      FE_MDIO_WRITE | FE_MDIO_BUSY;
	fe_wr(priv, FE_MDIO_CTRL, cmd);

	return s5l_fe_mdio_wait(priv);
}

static void s5l_fe_write_hwaddr(struct s5l_fe_priv *priv)
{
	const u8 *mac = priv->ndev->dev_addr;
	u32 lo, hi;

	lo = (u32)mac[0] | ((u32)mac[1] << 8) |
	     ((u32)mac[2] << 16) | ((u32)mac[3] << 24);
	hi = (u32)mac[4] | ((u32)mac[5] << 8);

	fe_wr(priv, FE_MAC_ADDR_L, lo);
	fe_wr(priv, FE_MAC_ADDR_H, hi);
}

static void s5l_fe_read_hwaddr(struct s5l_fe_priv *priv, u8 *addr)
{
	u32 lo = fe_rd(priv, FE_MAC_ADDR_L);
	u32 hi = fe_rd(priv, FE_MAC_ADDR_H);

	addr[0] = lo & 0xff;
	addr[1] = (lo >> 8) & 0xff;
	addr[2] = (lo >> 16) & 0xff;
	addr[3] = (lo >> 24) & 0xff;
	addr[4] = hi & 0xff;
	addr[5] = (hi >> 8) & 0xff;
}

static int s5l_fe_alloc_rx_buf(struct s5l_fe_priv *priv, unsigned int idx,
			       gfp_t gfp)
{
	struct sk_buff *skb;
	dma_addr_t dma;
	u32 ctrl;

	skb = __netdev_alloc_skb(priv->ndev, FE_RX_BUF_SIZE + NET_IP_ALIGN, gfp);
	if (!skb)
		return -ENOMEM;

	skb_reserve(skb, NET_IP_ALIGN);
	dma = dma_map_single(priv->dev, skb->data, FE_RX_BUF_SIZE,
			     DMA_FROM_DEVICE);
	if (dma_mapping_error(priv->dev, dma)) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}

	priv->rx_skbs[idx] = skb;
	priv->rx_dmas[idx] = dma;

	ctrl = FE_RX_BUF_SIZE;
	if (idx == FE_RX_RING_SIZE - 1)
		ctrl |= FE_RXD_EOR;

	priv->rx_ring[idx].buf_addr = cpu_to_le32((u32)dma);
	priv->rx_ring[idx].ctrl = cpu_to_le32(ctrl);
	dma_wmb();
	priv->rx_ring[idx].status = cpu_to_le32(FE_DESC_OWN);
	return 0;
}

static int s5l_fe_init_rings(struct s5l_fe_priv *priv)
{
	unsigned int i;
	int ret;

	priv->rx_head = 0;
	priv->tx_head = 0;
	priv->tx_tail = 0;

	memset(priv->rx_ring, 0, FE_RX_RING_SIZE * sizeof(struct s5l_fe_desc));
	memset(priv->tx_ring, 0, FE_TX_RING_SIZE * sizeof(struct s5l_fe_desc));

	for (i = 0; i < FE_RX_RING_SIZE; i++) {
		ret = s5l_fe_alloc_rx_buf(priv, i, GFP_KERNEL);
		if (ret)
			return ret;
	}

	for (i = 0; i < FE_TX_RING_SIZE; i++) {
		u32 ctrl = (i == FE_TX_RING_SIZE - 1) ? FE_TXD_EOR : 0;

		priv->tx_ring[i].status = 0;
		priv->tx_ring[i].ctrl = cpu_to_le32(ctrl);
		priv->tx_ring[i].buf_addr = 0;
		priv->tx_skbs[i] = NULL;
		priv->tx_dmas[i] = 0;
	}

	dma_wmb();
	return 0;
}

static void s5l_fe_free_rings(struct s5l_fe_priv *priv)
{
	unsigned int i;

	for (i = 0; i < FE_RX_RING_SIZE; i++) {
		if (priv->rx_skbs[i]) {
			dma_unmap_single(priv->dev, priv->rx_dmas[i],
					 FE_RX_BUF_SIZE, DMA_FROM_DEVICE);
			dev_kfree_skb_any(priv->rx_skbs[i]);
			priv->rx_skbs[i] = NULL;
		}
	}

	for (i = 0; i < FE_TX_RING_SIZE; i++) {
		if (priv->tx_skbs[i]) {
			dma_unmap_single(priv->dev, priv->tx_dmas[i],
					 priv->tx_skbs[i]->len, DMA_TO_DEVICE);
			dev_kfree_skb_any(priv->tx_skbs[i]);
			priv->tx_skbs[i] = NULL;
		}
	}
}

static void s5l_fe_hw_init(struct s5l_fe_priv *priv)
{
	unsigned int i;
	u32 val;

	fe_wr(priv, FE_DMA_CTRL, FE_DMA_SWRST);
	readl_relaxed_poll_timeout_atomic(priv->base + FE_DMA_CTRL, val,
					  !(val & FE_DMA_SWRST), 10, 10000);

	fe_wr(priv, FE_INT_ENABLE, 0);
	(void)fe_rd(priv, FE_INT_STATUS);

	fe_wr(priv, FE_DMA_BUS, 0x00000100);
	fe_wr(priv, FE_FIFO_THR, 0x00200038);
	fe_wr(priv, FE_FLOW_THR, 0x00c00010);
	fe_wr(priv, FE_PAUSE_TIME, 0x0000ffff);

	for (i = 0; i < 0x64; i += 4)
		fe_wr(priv, FE_STAT_BASE + i, 0);

	fe_wr(priv, FE_MCAST_HASH0, 0xffffffffU);
	fe_wr(priv, FE_MCAST_HASH1, 0xffffffffU);
	fe_wr(priv, FE_MCAST_HASH2, 0xffffffffU);
	fe_wr(priv, FE_MCAST_HASH3, 0xffffffffU);

	s5l_fe_write_hwaddr(priv);

	fe_wr(priv, FE_TX_BASE, (u32)priv->tx_ring_dma);
	fe_wr(priv, FE_RX_BASE, (u32)priv->rx_ring_dma);

	fe_wr(priv, FE_MAC_CTRL,
	      FE_MAC_TX_EN | FE_MAC_RX_EN | FE_MAC_FULL_DUPLEX |
	      FE_MAC_SPEED_100 | FE_MAC_BCAST | FE_MAC_MCAST | FE_MAC_PAUSE);

	fe_wr(priv, FE_DMA_CTRL, FE_DMA_TX_START | FE_DMA_RX_START);
	fe_wr(priv, FE_INT_ENABLE, FE_INT_MASK_DEFAULT);
	dsb(sy);
}

static void s5l_fe_adjust_link(struct net_device *ndev)
{
	struct s5l_fe_priv *priv = netdev_priv(ndev);
	struct phy_device *phydev = ndev->phydev;
	unsigned long flags;
	bool changed = false;
	u32 mac_ctrl;

	if (!phydev)
		return;

	spin_lock_irqsave(&priv->lock, flags);
	if (phydev->link) {
		if (!priv->link || priv->speed != phydev->speed ||
		    priv->duplex != phydev->duplex) {
			mac_ctrl = fe_rd(priv, FE_MAC_CTRL);
			mac_ctrl &= ~(FE_MAC_SPEED_100 | FE_MAC_FULL_DUPLEX);
			if (phydev->speed == SPEED_100)
				mac_ctrl |= FE_MAC_SPEED_100;
			if (phydev->duplex == DUPLEX_FULL)
				mac_ctrl |= FE_MAC_FULL_DUPLEX;
			fe_wr(priv, FE_MAC_CTRL, mac_ctrl);

			priv->link = phydev->link;
			priv->speed = phydev->speed;
			priv->duplex = phydev->duplex;
			changed = true;
		}
	} else if (priv->link) {
		priv->link = 0;
		priv->speed = 0;
		priv->duplex = -1;
		changed = true;
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	if (changed)
		phy_print_status(phydev);
}

static void s5l_fe_tx_clean(struct s5l_fe_priv *priv)
{
	struct net_device *ndev = priv->ndev;

	spin_lock(&priv->lock);
	while (priv->tx_tail != priv->tx_head) {
		unsigned int idx = priv->tx_tail % FE_TX_RING_SIZE;
		u32 status = le32_to_cpu(READ_ONCE(priv->tx_ring[idx].status));
		struct sk_buff *skb;

		if (status & FE_DESC_OWN)
			break;

		skb = priv->tx_skbs[idx];
		if (skb) {
			dma_unmap_single(priv->dev, priv->tx_dmas[idx],
					 skb->len, DMA_TO_DEVICE);
			ndev->stats.tx_packets++;
			ndev->stats.tx_bytes += skb->len;
			dev_consume_skb_any(skb);
			priv->tx_skbs[idx] = NULL;
		}
		priv->tx_tail++;
	}

	if (netif_queue_stopped(ndev) &&
	    (priv->tx_head - priv->tx_tail) < (FE_TX_RING_SIZE - 2))
		netif_wake_queue(ndev);
	spin_unlock(&priv->lock);
}

static int s5l_fe_rx_poll(struct s5l_fe_priv *priv, int budget)
{
	struct net_device *ndev = priv->ndev;
	int received = 0;

	while (received < budget) {
		unsigned int idx = priv->rx_head % FE_RX_RING_SIZE;
		struct s5l_fe_desc *desc = &priv->rx_ring[idx];
		u32 status = le32_to_cpu(READ_ONCE(desc->status));
		u32 ctrl;
		unsigned int pkt_len;
		struct sk_buff *skb, *new_skb;
		dma_addr_t new_dma;

		if (status & FE_DESC_OWN)
			break;

		dma_rmb();
		pkt_len = status & FE_RXD_LEN_MASK;

		if ((status & FE_RXD_ERR_MASK) ||
		    !(status & FE_RXD_FS) || !(status & FE_RXD_LS) ||
		    pkt_len == 0 || pkt_len > FE_RX_BUF_SIZE) {
			ndev->stats.rx_errors++;
			goto recycle;
		}

		skb = priv->rx_skbs[idx];
		new_skb = __netdev_alloc_skb(ndev, FE_RX_BUF_SIZE + NET_IP_ALIGN,
					     GFP_ATOMIC);
		if (!new_skb) {
			ndev->stats.rx_dropped++;
			goto recycle;
		}

		skb_reserve(new_skb, NET_IP_ALIGN);
		new_dma = dma_map_single(priv->dev, new_skb->data,
					 FE_RX_BUF_SIZE, DMA_FROM_DEVICE);
		if (dma_mapping_error(priv->dev, new_dma)) {
			dev_kfree_skb_any(new_skb);
			ndev->stats.rx_dropped++;
			goto recycle;
		}

		dma_unmap_single(priv->dev, priv->rx_dmas[idx],
				 FE_RX_BUF_SIZE, DMA_FROM_DEVICE);

		skb_put(skb, pkt_len);
		skb->protocol = eth_type_trans(skb, ndev);
		napi_gro_receive(&priv->napi, skb);

		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += pkt_len;

		priv->rx_skbs[idx] = new_skb;
		priv->rx_dmas[idx] = new_dma;
		desc->buf_addr = cpu_to_le32((u32)new_dma);

recycle:
		ctrl = FE_RX_BUF_SIZE;
		if (idx == FE_RX_RING_SIZE - 1)
			ctrl |= FE_RXD_EOR;
		desc->ctrl = cpu_to_le32(ctrl);
		dma_wmb();
		desc->status = cpu_to_le32(FE_DESC_OWN);

		priv->rx_head++;
		received++;
	}

	/* Ensure RX DMA remains active */
	fe_wr(priv, FE_DMA_CTRL, FE_DMA_TX_START | FE_DMA_RX_START);
	return received;
}

static int s5l_fe_napi_poll(struct napi_struct *napi, int budget)
{
	struct s5l_fe_priv *priv = container_of(napi, struct s5l_fe_priv, napi);
	int work_done;

	s5l_fe_tx_clean(priv);
	work_done = s5l_fe_rx_poll(priv, budget);

	if (work_done < budget) {
		napi_complete_done(napi, work_done);
		fe_wr(priv, FE_INT_ENABLE, FE_INT_MASK_DEFAULT);
		dsb(sy);
	}

	return work_done;
}

static irqreturn_t s5l_fe_irq(int irq, void *dev_id)
{
	struct s5l_fe_priv *priv = dev_id;
	u32 status = fe_rd(priv, FE_INT_STATUS);

	if (!status)
		return IRQ_NONE;

	/* Mask interrupts to acknowledge and schedule NAPI */
	fe_wr(priv, FE_INT_ENABLE, 0);
	dsb(sy);
	napi_schedule(&priv->napi);
	return IRQ_HANDLED;
}

static netdev_tx_t s5l_fe_start_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct s5l_fe_priv *priv = netdev_priv(ndev);
	unsigned int idx;
	struct s5l_fe_desc *desc;
	dma_addr_t dma;
	unsigned long flags;
	u32 ctrl;

	if (skb_padto(skb, ETH_ZLEN))
		return NETDEV_TX_OK;

	spin_lock_irqsave(&priv->lock, flags);
	if ((priv->tx_head - priv->tx_tail) >= (FE_TX_RING_SIZE - 1)) {
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&priv->lock, flags);
		return NETDEV_TX_BUSY;
	}

	dma = dma_map_single(priv->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(priv->dev, dma)) {
		spin_unlock_irqrestore(&priv->lock, flags);
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	idx = priv->tx_head % FE_TX_RING_SIZE;
	desc = &priv->tx_ring[idx];

	priv->tx_skbs[idx] = skb;
	priv->tx_dmas[idx] = dma;

	ctrl = (skb->len & FE_TXD_LEN_MASK) | FE_TXD_FS | FE_TXD_LS | FE_TXD_IOC;
	if (idx == FE_TX_RING_SIZE - 1)
		ctrl |= FE_TXD_EOR;

	desc->buf_addr = cpu_to_le32((u32)dma);
	desc->ctrl = cpu_to_le32(ctrl);
	dma_wmb();
	desc->status = cpu_to_le32(FE_DESC_OWN);
	dma_wmb();

	priv->tx_head++;
	if ((priv->tx_head - priv->tx_tail) >= (FE_TX_RING_SIZE - 1))
		netif_stop_queue(ndev);

	fe_wr(priv, FE_DMA_CTRL, FE_DMA_TX_START | FE_DMA_RX_START);
	fe_wr(priv, FE_TX_POLL, 1);
	dsb(sy);

	spin_unlock_irqrestore(&priv->lock, flags);
	return NETDEV_TX_OK;
}

static void s5l_fe_set_rx_mode(struct net_device *ndev)
{
	struct s5l_fe_priv *priv = netdev_priv(ndev);
	unsigned long flags;
	u32 mac_ctrl;

	spin_lock_irqsave(&priv->lock, flags);
	mac_ctrl = fe_rd(priv, FE_MAC_CTRL);
	if (ndev->flags & IFF_PROMISC)
		mac_ctrl |= FE_MAC_PROMISC;
	else
		mac_ctrl &= ~FE_MAC_PROMISC;
	fe_wr(priv, FE_MAC_CTRL, mac_ctrl);
	spin_unlock_irqrestore(&priv->lock, flags);
}

static int s5l_fe_set_mac_address(struct net_device *ndev, void *p)
{
	struct s5l_fe_priv *priv = netdev_priv(ndev);
	int ret;

	ret = eth_mac_addr(ndev, p);
	if (ret)
		return ret;

	s5l_fe_write_hwaddr(priv);
	return 0;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void s5l_fe_poll_controller(struct net_device *ndev)
{
	struct s5l_fe_priv *priv = netdev_priv(ndev);

	disable_irq(priv->irq);
	s5l_fe_irq(priv->irq, priv);
	enable_irq(priv->irq);
}
#endif

static int s5l_fe_open(struct net_device *ndev)
{
	struct s5l_fe_priv *priv = netdev_priv(ndev);
	struct device_node *phy_node;
	struct phy_device *phydev = NULL;
	int ret;

	ret = s5l_fe_init_rings(priv);
	if (ret) {
		s5l_fe_free_rings(priv);
		return ret;
	}

	priv->link = 0;
	priv->speed = 0;
	priv->duplex = -1;

	s5l_fe_hw_init(priv);

	phy_node = of_parse_phandle(priv->dev->of_node, "phy-handle", 0);
	if (phy_node) {
		phydev = of_phy_connect(ndev, phy_node, s5l_fe_adjust_link, 0,
					PHY_INTERFACE_MODE_RMII);
		of_node_put(phy_node);
	}
	if (!phydev) {
		phydev = phy_find_first(priv->mii_bus);
		if (phydev) {
			ret = phy_connect_direct(ndev, phydev,
						 s5l_fe_adjust_link,
						 PHY_INTERFACE_MODE_RMII);
			if (ret)
				phydev = ERR_PTR(ret);
		}
	}
	if (IS_ERR_OR_NULL(phydev)) {
		dev_err(priv->dev, "failed to connect to Ethernet PHY\n");
		s5l_fe_free_rings(priv);
		return phydev ? PTR_ERR(phydev) : -ENODEV;
	}

	phy_attached_info(phydev);
	napi_enable(&priv->napi);

	ret = request_irq(priv->irq, s5l_fe_irq, 0, ndev->name, priv);
	if (ret) {
		napi_disable(&priv->napi);
		phy_disconnect(phydev);
		s5l_fe_free_rings(priv);
		return ret;
	}

	phy_start(phydev);
	netif_start_queue(ndev);
	return 0;
}

static int s5l_fe_stop(struct net_device *ndev)
{
	struct s5l_fe_priv *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);
	if (ndev->phydev) {
		phy_stop(ndev->phydev);
		phy_disconnect(ndev->phydev);
	}

	fe_wr(priv, FE_INT_ENABLE, 0);
	fe_wr(priv, FE_DMA_CTRL, 0);
	fe_wr(priv, FE_MAC_CTRL, 0);
	dsb(sy);

	free_irq(priv->irq, priv);
	napi_disable(&priv->napi);
	s5l_fe_free_rings(priv);
	return 0;
}

static const struct net_device_ops s5l_fe_netdev_ops = {
	.ndo_open		= s5l_fe_open,
	.ndo_stop		= s5l_fe_stop,
	.ndo_start_xmit		= s5l_fe_start_xmit,
	.ndo_set_rx_mode	= s5l_fe_set_rx_mode,
	.ndo_set_mac_address	= s5l_fe_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_eth_ioctl		= phy_do_ioctl_running,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= s5l_fe_poll_controller,
#endif
};

static int s5l_fe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node, *mdio_np;
	struct net_device *ndev;
	struct s5l_fe_priv *priv;
	u8 hwaddr[ETH_ALEN];
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ndev = devm_alloc_etherdev(dev, sizeof(*priv));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, dev);
	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	priv->dev = dev;
	spin_lock_init(&priv->lock);
	platform_set_drvdata(pdev, ndev);

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return priv->irq;

	priv->clk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(priv->clk))
		return PTR_ERR(priv->clk);
	if (priv->clk) {
		ret = clk_prepare_enable(priv->clk);
		if (ret)
			return ret;
	}

	priv->phy_supply = devm_regulator_get_optional(dev, "phy");
	if (!IS_ERR(priv->phy_supply)) {
		ret = regulator_enable(priv->phy_supply);
		if (ret)
			dev_warn(dev, "failed to enable phy-supply: %d\n", ret);
		else
			msleep(50);
	} else {
		priv->phy_supply = NULL;
	}

	priv->rx_ring = dmam_alloc_coherent(dev,
					    FE_RX_RING_SIZE * sizeof(struct s5l_fe_desc),
					    &priv->rx_ring_dma, GFP_KERNEL);
	if (!priv->rx_ring)
		return -ENOMEM;

	priv->tx_ring = dmam_alloc_coherent(dev,
					    FE_TX_RING_SIZE * sizeof(struct s5l_fe_desc),
					    &priv->tx_ring_dma, GFP_KERNEL);
	if (!priv->tx_ring)
		return -ENOMEM;

	if (of_get_mac_address(np, hwaddr)) {
		s5l_fe_read_hwaddr(priv, hwaddr);
		if (!is_valid_ether_addr(hwaddr)) {
			/* Deterministic local-admin MAC fallback */
			hwaddr[0] = 0x02;
			hwaddr[1] = 0x53;
			hwaddr[2] = 0x35;
			hwaddr[3] = 0x4c;
			hwaddr[4] = 0x33;
			hwaddr[5] = 0x01;
		}
	}
	eth_hw_addr_set(ndev, hwaddr);

	ndev->netdev_ops = &s5l_fe_netdev_ops;
	netif_napi_add(ndev, &priv->napi, s5l_fe_napi_poll);

	priv->mii_bus = devm_mdiobus_alloc(dev);
	if (!priv->mii_bus)
		return -ENOMEM;

	priv->mii_bus->name = "s5l-fe-mdio";
	priv->mii_bus->read = s5l_fe_mdio_read;
	priv->mii_bus->write = s5l_fe_mdio_write;
	priv->mii_bus->parent = dev;
	priv->mii_bus->priv = priv;
	snprintf(priv->mii_bus->id, MII_BUS_ID_SIZE, "%s-mii", dev_name(dev));

	mdio_np = of_get_child_by_name(np, "mdio");
	ret = of_mdiobus_register(priv->mii_bus, mdio_np ? mdio_np : np);
	of_node_put(mdio_np);
	if (ret) {
		dev_err(dev, "failed to register MDIO bus: %d\n", ret);
		return ret;
	}

	ret = register_netdev(ndev);
	if (ret) {
		mdiobus_unregister(priv->mii_bus);
		return ret;
	}

	dev_info(dev, "S5L8947X 10/100 Fast Ethernet (%pM, irq %d)\n",
		 ndev->dev_addr, priv->irq);
	return 0;
}

static void s5l_fe_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct s5l_fe_priv *priv = netdev_priv(ndev);

	unregister_netdev(ndev);
	mdiobus_unregister(priv->mii_bus);
	if (priv->phy_supply)
		regulator_disable(priv->phy_supply);
	if (priv->clk)
		clk_disable_unprepare(priv->clk);
}

static const struct of_device_id s5l_fe_of_match[] = {
	{ .compatible = "apple,s5l8947x-ethernet" },
	{ .compatible = "apple,s5l-fe" },
	{ },
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
