// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU driver for BCM2712
 *
 * Copyright (c) 2023-2025 Raspberry Pi Ltd.
 */

#include "bcm2712-iommu.h"
#include "iommu-pages.h"

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/minmax.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define MMU_WR(off, val)   writel(val, mmu->reg_base + (off))
#define MMU_RD(off)        readl(mmu->reg_base + (off))

#define domain_to_mmu(d) (container_of(d, struct bcm2712_iommu_domain, base)->mmu)

#define MMMU_CTRL_OFFSET                       0x00
#define MMMU_CTRL_CAP_EXCEEDED                 BIT(27)
#define MMMU_CTRL_CAP_EXCEEDED_ABORT_EN        BIT(26)
#define MMMU_CTRL_CAP_EXCEEDED_INT_EN          BIT(25)
#define MMMU_CTRL_CAP_EXCEEDED_EXCEPTION_EN    BIT(24)
#define MMMU_CTRL_PT_INVALID                   BIT(20)
#define MMMU_CTRL_PT_INVALID_ABORT_EN          BIT(19)
#define MMMU_CTRL_PT_INVALID_INT_EN            BIT(18)
#define MMMU_CTRL_PT_INVALID_EXCEPTION_EN      BIT(17)
#define MMMU_CTRL_PT_INVALID_EN                BIT(16)
#define MMMU_CTRL_WRITE_VIOLATION              BIT(12)
#define MMMU_CTRL_WRITE_VIOLATION_ABORT_EN     BIT(11)
#define MMMU_CTRL_WRITE_VIOLATION_INT_EN       BIT(10)
#define MMMU_CTRL_WRITE_VIOLATION_EXCEPTION_EN BIT(9)
#define MMMU_CTRL_BYPASS                       BIT(8)
#define MMMU_CTRL_TLB_CLEARING                 BIT(7)
#define MMMU_CTRL_STATS_CLEAR                  BIT(3)
#define MMMU_CTRL_TLB_CLEAR                    BIT(2)
#define MMMU_CTRL_STATS_ENABLE                 BIT(1)
#define MMMU_CTRL_ENABLE                       BIT(0)

#define MMMU_PT_PA_BASE_OFFSET                 0x04

#define MMMU_HIT_OFFSET                        0x08
#define MMMU_MISS_OFFSET                       0x0C
#define MMMU_STALL_OFFSET                      0x10

#define MMMU_ADDR_CAP_OFFSET                   0x14
#define MMMU_ADDR_CAP_ENABLE                   BIT(31)
#define ADDR_CAP_SHIFT 28 /* ADDR_CAP is defined to be in 256 MByte units */

#define MMMU_SHOOT_DOWN_OFFSET                 0x18
#define MMMU_SHOOT_DOWN_SHOOTING               BIT(31)
#define MMMU_SHOOT_DOWN_SHOOT                  BIT(30)

#define MMMU_BYPASS_START_OFFSET               0x1C
#define MMMU_BYPASS_START_ENABLE               BIT(31)
#define MMMU_BYPASS_START_INVERT               BIT(30)

#define MMMU_BYPASS_END_OFFSET                 0x20
#define MMMU_BYPASS_END_ENABLE                 BIT(31)

#define MMMU_MISC_OFFSET                       0x24
#define MMMU_MISC_SINGLE_TABLE                 BIT(31)

#define MMMU_ILLEGAL_ADR_OFFSET                0x30
#define MMMU_ILLEGAL_ADR_ENABLE                BIT(31)

#define MMMU_VIO_ADDR_OFFSET                   0x34

#define MMMU_DEBUG_INFO_OFFSET                 0x38
#define MMMU_DEBUG_INFO_VERSION_MASK           0x0000000Fu
#define MMMU_DEBUG_INFO_VA_WIDTH_MASK          0x000000F0u
#define MMMU_DEBUG_INFO_PA_WIDTH_MASK          0x00000F00u
#define MMMU_DEBUG_INFO_BIGPAGE_WIDTH_MASK     0x000FF000u
#define MMMU_DEBUG_INFO_SUPERPAGE_WIDTH_MASK   0x0FF00000u
#define MMMU_DEBUG_INFO_BYPASS_4M              BIT(28)
#define MMMU_DEBUG_INFO_BYPASS                 BIT(29)

#define MMMU_PTE_PAGESIZE_MASK                 0xC0000000u
#define MMMU_PTE_WRITEABLE                     BIT(29)
#define MMMU_PTE_VALID                         BIT(28)

/*
 * BCM2712 IOMMU is organized around 4Kbyte pages (IOMMU_PAGE_SIZE).
 * Linux PAGE_SIZE must not be smaller but may be larger (e.g. 4K, 16K).
 *
 * Unlike many larger MMUs, this one uses a 4-byte word size, allowing
 * 1024 entries within each 4K table page, and two-level translation.
 * To avoid needing CMA, we require the top-level table to fit within
 * a single Linux page. This restricts IOVA space to (PAGE_SIZE << 20).
 * A "default" page is allocated to catch illegal reads and writes.
 *
 * To deal with multiple blocks connected to an IOMMU when only some
 * of them are IOMMU clients, by default we start the IOVA region at
 * 40GB; requests to lower addresses pass straight through to SDRAM.
 * Both the base and size of the IOVA region are configurable.
 *
 * Currently we assume a 1:1:1 correspondence of IOMMU, group and domain.
 */

#define IOMMU_PAGE_SHIFT    12
#define IOMMU_PAGE_SIZE     BIT(IOMMU_PAGE_SHIFT)

#define LX_PAGEWORDS_SHIFT    (PAGE_SHIFT - 2)
#define LX_PAGEWORDS_MASK     ((1u << LX_PAGEWORDS_SHIFT) - 1u)
#define IOMMU_PAGEWORDS_SHIFT (IOMMU_PAGE_SHIFT - 2)
#define IOMMU_HUGEPAGE_SHIFT  (IOMMU_PAGE_SHIFT + IOMMU_PAGEWORDS_SHIFT)
#define L1_AP_BASE_SHIFT      (IOMMU_PAGE_SHIFT + 2 * IOMMU_PAGEWORDS_SHIFT)
#define IOVA_TO_LXPAGE_SHIFT  (IOMMU_PAGE_SHIFT + LX_PAGEWORDS_SHIFT)

#define DEFAULT_APERTURE_BASE (40ul << 30)
#define DEFAULT_APERTURE_SIZE (2ul << 30)

/*
 * Allocate a Linux page for tables; return the index of its first IOMMU page.
 * To avoid having to store DMA addresses, we assume that a DMA address used
 * for IOMMU tables is always a physical address -- this is true for BCM2712.
 * Note that iommu_alloc_page() will zero the page contents.
 */
static u32 bcm2712_iommu_get_page(struct bcm2712_iommu *mmu, u32 **ptr)
{
	*ptr = iommu_alloc_pages_sz(GFP_KERNEL, IOMMU_PAGE_SIZE);
	if (*ptr) {
		dma_addr_t dma = dma_map_single(mmu->dev, *ptr,
						PAGE_SIZE, DMA_TO_DEVICE);
		WARN_ON(dma_mapping_error(mmu->dev, dma) ||
			dma != virt_to_phys(*ptr));
		return (u32)(dma >> IOMMU_PAGE_SHIFT);
	}
	dev_err(mmu->dev, "Failed to allocate a page\n");
	return 0;
}

static void bcm2712_iommu_free_page(struct bcm2712_iommu *mmu, void *ptr)
{
	if (ptr) {
		dma_unmap_single(mmu->dev, virt_to_phys(ptr),
				 PAGE_SIZE, DMA_TO_DEVICE);
		iommu_free_pages(ptr);
	}
}

static int bcm2712_iommu_init(struct bcm2712_iommu *mmu)
{
	u32 u = MMU_RD(MMMU_DEBUG_INFO_OFFSET);

	/*
	 * Check IOMMU version and hardware configuration.
	 * This driver is for VC IOMMU version >= 4 (with 2-level tables)
	 * and assumes at least 36 bits of virtual and physical address space.
	 * Bigpage and superpage sizes are typically 64K and 1M, but may vary
	 * (hugepage size is fixed at 4M, the range covered by an L2 page).
	 */
	dev_info(mmu->dev, "%s: DEBUG_INFO = 0x%08x\n", __func__, u);
	WARN_ON(FIELD_GET(MMMU_DEBUG_INFO_VERSION_MASK, u) < 4 ||
		FIELD_GET(MMMU_DEBUG_INFO_VA_WIDTH_MASK, u) < 6 ||
		FIELD_GET(MMMU_DEBUG_INFO_PA_WIDTH_MASK, u) < 6 ||
		!(u & MMMU_DEBUG_INFO_BYPASS));

	dma_set_mask_and_coherent(mmu->dev,
				  DMA_BIT_MASK(FIELD_GET(MMMU_DEBUG_INFO_PA_WIDTH_MASK, u) + 30u));
	mmu->bigpage_mask =
		((1u << FIELD_GET(MMMU_DEBUG_INFO_BIGPAGE_WIDTH_MASK, u)) - 1u) <<
		IOMMU_PAGE_SHIFT;
	mmu->superpage_mask =
		((1u << FIELD_GET(MMMU_DEBUG_INFO_SUPERPAGE_WIDTH_MASK, u)) - 1u) <<
		IOMMU_PAGE_SHIFT;

	/* Disable MMU and clear sticky flags; meanwhile flush the TLB */
	MMU_WR(MMMU_CTRL_OFFSET,
	       MMMU_CTRL_CAP_EXCEEDED    |
	       MMMU_CTRL_PT_INVALID      |
	       MMMU_CTRL_WRITE_VIOLATION |
	       MMMU_CTRL_STATS_CLEAR     |
	       MMMU_CTRL_TLB_CLEAR);

	/*
	 * Put MMU into 2-level mode; set address cap and "bypass" range
	 * (note that some of these registers have unintuitive off-by-ones).
	 * Addresses below the "aperture" are passed unchanged: this is
	 * useful for blocks which share an IOMMU with other blocks
	 * whose drivers are not IOMMU-aware.
	 */
	MMU_WR(MMMU_MISC_OFFSET,
	       MMU_RD(MMMU_MISC_OFFSET) & ~MMMU_MISC_SINGLE_TABLE);
	MMU_WR(MMMU_ADDR_CAP_OFFSET,
	       MMMU_ADDR_CAP_ENABLE +
	       ((mmu->aperture_end - mmu->dma_iova_offset) >> ADDR_CAP_SHIFT) - 1);
	MMU_WR(MMMU_BYPASS_START_OFFSET, 0);
	if (mmu->aperture_base > mmu->dma_iova_offset) {
		unsigned int byp_shift = (u & MMMU_DEBUG_INFO_BYPASS_4M) ?
			IOMMU_HUGEPAGE_SHIFT : ADDR_CAP_SHIFT;

		MMU_WR(MMMU_BYPASS_END_OFFSET,
		       MMMU_BYPASS_END_ENABLE +
		       ((mmu->aperture_base - mmu->dma_iova_offset) >> byp_shift));
	} else {
		MMU_WR(MMMU_BYPASS_END_OFFSET, 0);
	}

	/*
	 * Configure the addresses of the top-level table (offset because
	 * the aperture does not start from zero), and of the default page.
	 * For simplicity, both these regions are whole Linux pages.
	 */
	u = bcm2712_iommu_get_page(mmu, &mmu->top_table);
	if (!u)
		return -ENOMEM;
	MMU_WR(MMMU_PT_PA_BASE_OFFSET,
	       u - ((mmu->aperture_base - mmu->dma_iova_offset) >> L1_AP_BASE_SHIFT));
	u = bcm2712_iommu_get_page(mmu, &mmu->default_page);
	if (!u) {
		bcm2712_iommu_free_page(mmu, mmu->top_table);
		return -ENOMEM;
	}
	MMU_WR(MMMU_ILLEGAL_ADR_OFFSET, MMMU_ILLEGAL_ADR_ENABLE + u);
	mmu->nmapped_pages = 0;

	/* Flush (and enable) the shared TLB cache; enable this MMU. */
	if (mmu->cache)
		bcm2712_iommu_cache_flush(mmu->cache);
	MMU_WR(MMMU_CTRL_OFFSET,
	       MMMU_CTRL_CAP_EXCEEDED_ABORT_EN    |
	       MMMU_CTRL_PT_INVALID_ABORT_EN      |
	       MMMU_CTRL_PT_INVALID_EN            |
	       MMMU_CTRL_WRITE_VIOLATION_ABORT_EN |
	       MMMU_CTRL_STATS_ENABLE             |
	       MMMU_CTRL_ENABLE);

	return 0;
}

static int bcm2712_iommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct bcm2712_iommu *mmu = dev ? dev_iommu_priv_get(dev) : 0;
	struct bcm2712_iommu_domain *mydomain =
		container_of(domain, struct bcm2712_iommu_domain, base);

	dev_info(dev, "%s: MMU %s\n",
		 __func__, mmu ? dev_name(mmu->dev) : "");

	if (mmu) {
		mydomain->mmu = mmu;
		mmu->domain = mydomain;
		domain->geometry.aperture_start = mmu->aperture_base;
		domain->geometry.aperture_end = mmu->aperture_end - 1ul;
		domain->geometry.force_aperture = true;

		return 0;
	}
	return -EINVAL;
}

static int bcm2712_iommu_map(struct iommu_domain *domain, unsigned long iova,
			     phys_addr_t pa, size_t bytes, size_t count,
			     int prot, gfp_t gfp, size_t *mapped)
{
	struct bcm2712_iommu *mmu = domain_to_mmu(domain);
	u32 entry = MMMU_PTE_VALID | (pa >> IOMMU_PAGE_SHIFT);
	u32 align = (u32)(iova | pa | bytes);
	unsigned long flags;
	unsigned int p, p_last, lxp;
	bool dirty_top = false;

	/* Reject if not entirely within our aperture (should never happen) */
	bytes *= count;
	if (iova < mmu->aperture_base || iova + bytes > mmu->aperture_end) {
		*mapped = 0;
		return -EINVAL;
	}

	/* DMA addresses -> page numbers */
	iova -= mmu->aperture_base;
	p = iova >> IOMMU_PAGE_SHIFT;
	p_last = (iova + bytes - 1) >> IOMMU_PAGE_SHIFT;

	/*
	 * Check we have allocated the required Level-2 tables (in units of
	 * whole Linux pages, each covering one or more IOMMU-table pages).
	 * Any new L2 pages need to be linked to the top-level table.
	 * A failure here will cause the entire map_pages() call to fail.
	 */
	spin_lock_irqsave(&mmu->hw_lock, flags);
	for (lxp = (p >> LX_PAGEWORDS_SHIFT);
	     lxp <= (p_last >> LX_PAGEWORDS_SHIFT); lxp++) {
		if (!mmu->tables[lxp]) {
			unsigned int i, u;

			u = bcm2712_iommu_get_page(mmu, &mmu->tables[lxp]);
			if (!u)
				break;
			u |= MMMU_PTE_VALID;
			if (!dirty_top) {
				dma_sync_single_for_cpu(mmu->dev, virt_to_phys(mmu->top_table),
							PAGE_SIZE, DMA_TO_DEVICE);
				dirty_top = true;
			}
			for (i = lxp << (PAGE_SHIFT - IOMMU_PAGE_SHIFT);
			     i < (lxp + 1) << (PAGE_SHIFT - IOMMU_PAGE_SHIFT); i++) {
				mmu->top_table[i] = u++;
			}
		}
	}
	if (dirty_top)
		dma_sync_single_for_device(mmu->dev, virt_to_phys(mmu->top_table),
					   PAGE_SIZE, DMA_TO_DEVICE);
	if (lxp <= (p_last >> LX_PAGEWORDS_SHIFT)) {
		spin_unlock_irqrestore(&mmu->hw_lock, flags);
		*mapped = 0;
		return -ENOMEM;
	}

	/* large page and write enable flags */
	if (!(align & ((1 << IOMMU_HUGEPAGE_SHIFT) - 1)))
		entry |= FIELD_PREP(MMMU_PTE_PAGESIZE_MASK, 3);
	else if (!(align & mmu->superpage_mask) && mmu->superpage_mask)
		entry |= FIELD_PREP(MMMU_PTE_PAGESIZE_MASK, 2);
	else if (!(align &  mmu->bigpage_mask) && mmu->bigpage_mask)
		entry |= FIELD_PREP(MMMU_PTE_PAGESIZE_MASK, 1);
	if (prot & IOMMU_WRITE)
		entry |= MMMU_PTE_WRITEABLE;

	/*
	 * Again iterate over table-pages and bring them into CPU ownwership.
	 * Fill in the required PT entries, then give them back to the device.
	 */
	while (p <= p_last) {
		u32 *tbl = mmu->tables[p >> LX_PAGEWORDS_SHIFT];

		dma_sync_single_for_cpu(mmu->dev, virt_to_phys(tbl),
					PAGE_SIZE, DMA_TO_DEVICE);
		while (p <= p_last) {
			mmu->nmapped_pages += !tbl[p & LX_PAGEWORDS_MASK];
			tbl[p & LX_PAGEWORDS_MASK] = entry++;
			if (IS_ALIGNED(++p, (1u << LX_PAGEWORDS_SHIFT)))
				break;
		}
		dma_sync_single_for_device(mmu->dev, virt_to_phys(tbl),
					   PAGE_SIZE, DMA_TO_DEVICE);
	}

	spin_unlock_irqrestore(&mmu->hw_lock, flags);
	*mapped = bytes;
	return 0;
}

static size_t bcm2712_iommu_unmap(struct iommu_domain *domain, unsigned long iova,
				  size_t bytes, size_t count,
				  struct iommu_iotlb_gather *gather)
{
	struct bcm2712_iommu *mmu = domain_to_mmu(domain);
	unsigned long flags;
	unsigned int p, p_last;

	/* Reject if not entirely within our aperture (should never happen) */
	bytes *= count;
	if (iova < mmu->aperture_base || iova + bytes > mmu->aperture_end)
		return 0;

	/* Record just the lower and upper bounds in "gather" */
	spin_lock_irqsave(&mmu->hw_lock, flags);
	iommu_iotlb_gather_add_range(gather, iova, bytes);

	/* DMA addresses -> page numbers */
	iova -= mmu->aperture_base;
	p = iova >> IOMMU_PAGE_SHIFT;
	p_last = (iova + bytes - 1) >> IOMMU_PAGE_SHIFT;

	/*
	 * Iterate over tables in Linux-pages and bring them into CPU ownwership.
	 * Clear the required PT entries, then give them back to the device.
	 */
	while (p <= p_last) {
		u32 *tbl = mmu->tables[p >> LX_PAGEWORDS_SHIFT];

		if (tbl) {
			dma_sync_single_for_cpu(mmu->dev, virt_to_phys(tbl),
						PAGE_SIZE, DMA_TO_DEVICE);
			while (p <= p_last) {
				mmu->nmapped_pages -= !!tbl[p & LX_PAGEWORDS_MASK];
				tbl[p & LX_PAGEWORDS_MASK] = 0;
				if (IS_ALIGNED(++p, (1u << LX_PAGEWORDS_SHIFT)))
					break;
			}
			dma_sync_single_for_device(mmu->dev, virt_to_phys(tbl),
						   PAGE_SIZE, DMA_TO_DEVICE);
		}
	}

	spin_unlock_irqrestore(&mmu->hw_lock, flags);
	return bytes;
}

static int bcm2712_iommu_sync_range(struct iommu_domain *domain,
				    unsigned long iova, size_t size)
{
	struct bcm2712_iommu *mmu = domain_to_mmu(domain);
	unsigned long flags, iova_end;
	unsigned int i, p4;

	if (!mmu)
		return 0;

	iova_end = min(mmu->aperture_end, iova + size);
	iova = max(mmu->aperture_base, iova);
	if (iova_end <= iova)
		return 0;

	/* Flush the shared TLB cache */
	spin_lock_irqsave(&mmu->hw_lock, flags);
	if (mmu->cache)
		bcm2712_iommu_cache_flush(mmu->cache);

	/*
	 * When flushing a large range or when nothing needs to be kept,
	 * it's quicker to use the "TLB_CLEAR" flag. Otherwise, invalidate
	 * TLB entries in lines of 4 words each. Each flush/clear operation
	 * should complete almost instantaneously.
	 */
	if (mmu->nmapped_pages == 0 || iova_end >= iova + (1ul << 24)) {
		MMU_WR(MMMU_CTRL_OFFSET,
		       MMMU_CTRL_CAP_EXCEEDED_ABORT_EN    |
		       MMMU_CTRL_PT_INVALID_ABORT_EN      |
		       MMMU_CTRL_PT_INVALID_EN            |
		       MMMU_CTRL_WRITE_VIOLATION_ABORT_EN |
		       MMMU_CTRL_TLB_CLEAR                |
		       MMMU_CTRL_STATS_ENABLE             |
		       MMMU_CTRL_ENABLE);
		for (i = 0; i < 1024; i++) {
			if (!(MMMU_CTRL_TLB_CLEARING & MMU_RD(MMMU_CTRL_OFFSET)))
				break;
			cpu_relax();
		}
	} else {
		iova -= mmu->dma_iova_offset; /* DMA address -> IOMMU virtual address */
		iova_end -= (mmu->dma_iova_offset + 1); /* last byte in range */
		for (p4 = iova >> (IOMMU_PAGE_SHIFT + 2);
		     p4 <= iova_end >> (IOMMU_PAGE_SHIFT + 2); p4++) {
			MMU_WR(MMMU_SHOOT_DOWN_OFFSET,
			       MMMU_SHOOT_DOWN_SHOOT + (p4 << 2));
			for (i = 0; i < 1024; i++) {
				if (!(MMMU_SHOOT_DOWN_SHOOTING & MMU_RD(MMMU_SHOOT_DOWN_OFFSET)))
					break;
				cpu_relax();
			}
		}
	}

	spin_unlock_irqrestore(&mmu->hw_lock, flags);
	return 0;
}

static void bcm2712_iommu_sync(struct iommu_domain *domain,
			       struct iommu_iotlb_gather *gather)
{
	if (gather->end)
		bcm2712_iommu_sync_range(domain, gather->start,
					 gather->end - gather->start + 1);
}

static void bcm2712_iommu_sync_all(struct iommu_domain *domain)
{
	struct bcm2712_iommu *mmu = domain_to_mmu(domain);

	if (mmu)
		bcm2712_iommu_sync_range(domain,
					 mmu->aperture_base,
					 mmu->aperture_end - mmu->aperture_base);
}

static phys_addr_t bcm2712_iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	phys_addr_t addr = 0;
	struct bcm2712_iommu *mmu = domain_to_mmu(domain);

	if (iova  >= mmu->aperture_base && iova < mmu->aperture_end) {
		unsigned long flags;
		u32 *ptr;
		u32 p;

		spin_lock_irqsave(&mmu->hw_lock, flags);
		p = (iova - mmu->aperture_base) >> IOMMU_PAGE_SHIFT;
		ptr = mmu->tables[p >> LX_PAGEWORDS_SHIFT];
		if (ptr) {
			p = ptr[p & LX_PAGEWORDS_MASK] & 0x0FFFFFFFu;
			addr = (((phys_addr_t)p) << IOMMU_PAGE_SHIFT) +
				(iova & (IOMMU_PAGE_SIZE - 1u));
		}
		spin_unlock_irqrestore(&mmu->hw_lock, flags);
	}

	return addr;
}

static void bcm2712_iommu_domain_free(struct iommu_domain *domain)
{
	struct bcm2712_iommu_domain *mydomain =
		container_of(domain, struct bcm2712_iommu_domain, base);

	kfree(mydomain);
}

static const struct iommu_domain_ops bcm2712_iommu_domain_ops = {
	.attach_dev	 = bcm2712_iommu_attach_dev,
	.map_pages	 = bcm2712_iommu_map,
	.unmap_pages	 = bcm2712_iommu_unmap,
	.iotlb_sync      = bcm2712_iommu_sync,
	.iotlb_sync_map  = bcm2712_iommu_sync_range,
	.flush_iotlb_all = bcm2712_iommu_sync_all,
	.iova_to_phys	 = bcm2712_iommu_iova_to_phys,
	.free		 = bcm2712_iommu_domain_free,
};

static struct iommu_domain *bcm2712_iommu_domain_alloc(struct device *dev, u32 flags,
								    const struct iommu_user_data *user_data)
{
	struct bcm2712_iommu_domain *domain;

	/*if (type != IOMMU_DOMAIN_UNMANAGED && type != IOMMU_DOMAIN_DMA)
		return NULL;*/

	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain)
		return NULL;

	domain->base.type = IOMMU_DOMAIN_UNMANAGED;
	domain->base.ops  = &bcm2712_iommu_domain_ops;
	domain->base.geometry.force_aperture = true;
	/* Advertise native page sizes as well as 2M, 16K which Linux may prefer */
	domain->base.pgsize_bitmap = SZ_4M | SZ_2M | SZ_1M | SZ_64K | SZ_16K | SZ_4K;

	return &domain->base;
}

static struct iommu_device *bcm2712_iommu_probe_device(struct device *dev)
{
	struct bcm2712_iommu *mmu;

	/*
	 * For reasons I don't fully understand, we need to try both
	 * cases (dev_iommu_priv_get() and platform_get_drvdata())
	 * in order to get both GPU and ISP-BE to probe successfully.
	 */
	mmu = dev_iommu_priv_get(dev);
	if (!mmu) {
		struct device_node *np;
		struct platform_device *pdev;

		/* Ignore devices that don't have an "iommus" property with exactly one phandle */
		if (!dev->of_node ||
		    of_property_count_elems_of_size(dev->of_node, "iommus", sizeof(phandle)) != 1)
			return ERR_PTR(-ENODEV);

		np = of_parse_phandle(dev->of_node, "iommus", 0);
		if (!np)
			return ERR_PTR(-EINVAL);

		pdev = of_find_device_by_node(np);
		of_node_put(np);
		if (pdev)
			mmu = platform_get_drvdata(pdev);

		if (!mmu)
			return ERR_PTR(-ENODEV);
	}

	dev_info(dev, "%s: MMU %s\n", __func__, dev_name(mmu->dev));
	dev_iommu_priv_set(dev, mmu);
	return &mmu->iommu;
}

static void bcm2712_iommu_release_device(struct device *dev)
{
	dev_iommu_priv_set(dev, NULL);
}

static struct iommu_group *bcm2712_iommu_device_group(struct device *dev)
{
	struct bcm2712_iommu *mmu = dev_iommu_priv_get(dev);

	if (!mmu || !mmu->group)
		return ERR_PTR(-EINVAL);

	dev_info(dev, "%s: MMU %s\n", __func__, dev_name(mmu->dev));
	return iommu_group_ref_get(mmu->group);
}

static int bcm2712_iommu_of_xlate(struct device *dev,
				  const struct of_phandle_args *args)
{
	struct platform_device *iommu_dev;
	struct bcm2712_iommu *mmu;

	iommu_dev = of_find_device_by_node(args->np);
	mmu = platform_get_drvdata(iommu_dev);
	dev_iommu_priv_set(dev, mmu);
	dev_info(dev, "%s: MMU %s\n", __func__, dev_name(mmu->dev));

	return 0;
}

static bool bcm2712_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	return false;
}

static const struct iommu_ops bcm2712_iommu_ops = {
	.capable        = bcm2712_iommu_capable,
	.domain_alloc_paging_flags	= bcm2712_iommu_domain_alloc,
	.probe_device	= bcm2712_iommu_probe_device,
	.release_device	= bcm2712_iommu_release_device,
	.device_group	= bcm2712_iommu_device_group,
	.default_domain_ops = &bcm2712_iommu_domain_ops,
	.of_xlate = bcm2712_iommu_of_xlate,
};

static int bcm2712_iommu_probe(struct platform_device *pdev)
{
	struct bcm2712_iommu *mmu;
	struct bcm2712_iommu_cache *cache = NULL;
	unsigned int num_pages;
	int ret;

	/* First of all, check for an IOMMU shared cache */
	if (pdev->dev.of_node) {
		struct device_node *cache_np;
		struct platform_device *cache_pdev;

		cache_np = of_parse_phandle(pdev->dev.of_node, "cache", 0);
		if (cache_np) {
			cache_pdev = of_find_device_by_node(cache_np);
			of_node_put(cache_np);
			if (cache_pdev && !IS_ERR(cache_pdev))
				cache = platform_get_drvdata(cache_pdev);
			if (!cache)
				return -EPROBE_DEFER;
		}
	}

	/* Allocate private data */
	mmu = devm_kzalloc(&pdev->dev, sizeof(*mmu), GFP_KERNEL);
	if (!mmu)
		return -ENOMEM;

	mmu->name = dev_name(&pdev->dev);
	mmu->dev = &pdev->dev;
	mmu->cache = cache;
	platform_set_drvdata(pdev, mmu);
	spin_lock_init(&mmu->hw_lock);

	/*
	 * Configurable properties. The IOVA base address may be any
	 * multiple of 4GB; when it is zero, pass-through will be disabled.
	 * Its size may be any multiple of 256MB up to (PAGE_SIZE << 20).
	 *
	 * When the IOMMU is downstream of a PCIe RC or some other chip/bus
	 * and serves some of the masters thereon (others using pass-through),
	 * we seem to fumble and lose the "dma-ranges" address offset for DMA
	 * masters on the far bus. The "dma-iova-offset" property optionally
	 * restores it. Where present, we add the offset to all DMA addresses
	 * (but we don't expect to see any offset on the IOMMU's AXI bus).
	 */
	mmu->aperture_base = DEFAULT_APERTURE_BASE;
	mmu->aperture_end  = DEFAULT_APERTURE_SIZE;
	mmu->dma_iova_offset = 0;
	if (pdev->dev.of_node) {
		if (!of_property_read_u64(pdev->dev.of_node,
					  "aperture-base", &mmu->aperture_base))
			mmu->aperture_base &= ~((1ul << L1_AP_BASE_SHIFT) - 1);
		if (!of_property_read_u64(pdev->dev.of_node,
					  "aperture-size", &mmu->aperture_end))
			mmu->aperture_end = clamp(mmu->aperture_end &
						  ~((1ul << ADDR_CAP_SHIFT) - 1),
						  (1ul << ADDR_CAP_SHIFT),
						  (PAGE_SIZE << 20));
		if (!of_property_read_u64(pdev->dev.of_node,
					  "dma-iova-offset", &mmu->dma_iova_offset))
			mmu->aperture_base += mmu->dma_iova_offset;
	}
	num_pages = mmu->aperture_end >> IOVA_TO_LXPAGE_SHIFT;
	mmu->aperture_end += mmu->aperture_base;
	dev_info(&pdev->dev, "IOVA aperture 0x%llx..0x%llx including DMA offset 0x%llx\n",
		 (unsigned long long)mmu->aperture_base,
		 (unsigned long long)mmu->aperture_end,
		 (unsigned long long)mmu->dma_iova_offset);

	/* Allocate pointers for each page */
	mmu->tables = devm_kzalloc(&pdev->dev,
				   num_pages * sizeof(u32 *), GFP_KERNEL);
	if (!mmu->tables)
		return -ENOMEM;

	/* Get IOMMU registers */
	mmu->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mmu->reg_base)) {
		dev_err(&pdev->dev, "Failed to get IOMMU registers address\n");
		ret = PTR_ERR(mmu->reg_base);
		goto done_err;
	}

	/* Stuff */
	mmu->group = iommu_group_alloc();
	if (IS_ERR(mmu->group)) {
		ret = PTR_ERR(mmu->group);
		mmu->group = NULL;
		goto done_err;
	}
	ret = iommu_device_sysfs_add(&mmu->iommu, mmu->dev, NULL, mmu->name);
	if (ret)
		goto done_err;

	/* Initialize hardware -- this will try to allocate 2 pages */
	ret = bcm2712_iommu_init(mmu);
	if (ret)
		goto done_err;

	ret = iommu_device_register(&mmu->iommu, &bcm2712_iommu_ops, &pdev->dev);
	if (!ret)
		return 0;

done_err:
	dev_info(&pdev->dev, "%s: Failure %d\n", __func__, ret);
	if (mmu->group)
		iommu_group_put(mmu->group);

	return ret;
}

static void bcm2712_iommu_remove(struct platform_device *pdev)
{
	struct bcm2712_iommu *mmu = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = 0; i < (mmu->aperture_end - mmu->aperture_base) >> IOVA_TO_LXPAGE_SHIFT; i++)
		bcm2712_iommu_free_page(mmu, mmu->tables[i]);
	bcm2712_iommu_free_page(mmu, mmu->top_table);
	bcm2712_iommu_free_page(mmu, mmu->default_page);

	if (mmu->reg_base)
		MMU_WR(MMMU_CTRL_OFFSET, 0); /* disable the MMU */
}

static const struct of_device_id bcm2712_iommu_of_match[] = {
	{
		. compatible = "brcm,bcm2712-iommu"
	},
	{ /* sentinel */ },
};

static struct platform_driver bcm2712_iommu_driver = {
	.probe = bcm2712_iommu_probe,
	.remove = bcm2712_iommu_remove,
	.driver = {
		.name = "bcm2712-iommu",
		.of_match_table = bcm2712_iommu_of_match
	},
};

builtin_platform_driver(bcm2712_iommu_driver);
