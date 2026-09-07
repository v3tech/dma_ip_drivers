/*
 * This file is part of the Xilinx DMA IP Core driver for Linux
 *
 * Copyright (c) 2016-present,  Xilinx, Inc.
 * All rights reserved.
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */

#define pr_fmt(fmt)     KBUILD_MODNAME ":%s: " fmt, __func__

#include <linux/ioctl.h>
#include <linux/capability.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include "version.h"
#include "xdma_cdev.h"
#include "cdev_ctrl.h"

#if ACCESS_OK_2_ARGS
#define xlx_access_ok(X, Y, Z) access_ok(Y, Z)
#else
#define xlx_access_ok(X, Y, Z) access_ok(X, Y, Z)
#endif

struct xdma_user_dma_map {
	struct list_head node;
	struct file *file;
	struct device *dev;
	struct page **pages;
	unsigned long nr_pages;
	struct sg_table sgt;
	dma_addr_t dma_addr;
	size_t length;
	bool dma_mapped;
	void *coherent_cpu_addr;
	bool coherent_allocated;
	bool release_fenced;
	bool address_published;
	bool module_pinned;
	atomic_t vma_refs;
	u32 fence_subcard;
	u32 fence_direction;
	u32 allocation_subcard;
	u32 allocation_direction;
	u32 rx_accepted_base;
	bool rx_accepted_base_valid;
};

static LIST_HEAD(xdma_user_dma_maps);
static LIST_HEAD(xdma_quarantined_dma_maps);
static LIST_HEAD(xdma_reusable_coherent_maps);
static DEFINE_MUTEX(xdma_user_dma_maps_lock);

static struct xdma_user_dma_map *xdma_dma_find_file(struct file *file)
{
	struct xdma_user_dma_map *map;

	list_for_each_entry(map, &xdma_user_dma_maps, node) {
		if (map->file == file)
			return map;
	}

	return NULL;
}

static void xdma_dma_map_release(struct xdma_user_dma_map *map)
{
	if (!map)
		return;

	if (map->coherent_allocated)
		dma_free_coherent(map->dev, map->length,
				  map->coherent_cpu_addr, map->dma_addr);
	else if (map->dma_mapped)
		dma_unmap_sgtable(map->dev, &map->sgt,
				  DMA_BIDIRECTIONAL, 0);
	if (map->sgt.sgl)
		sg_free_table(&map->sgt);
	if (map->pages) {
		if (map->nr_pages)
			unpin_user_pages_dirty_lock(map->pages, map->nr_pages,
						    true);
		kvfree(map->pages);
	}
	kfree(map);
}

static struct xdma_user_dma_map *xdma_dma_find_reusable(
		struct device *dev, size_t length, u32 subcard, u32 direction)
{
	struct xdma_user_dma_map *map;

	list_for_each_entry(map, &xdma_reusable_coherent_maps, node) {
		if (map->dev == dev && map->length == length &&
		    map->allocation_subcard == subcard &&
		    map->allocation_direction == direction)
			return map;
	}
	return NULL;
}

void xdma_dma_cleanup_reusable(struct device *dev)
{
	struct xdma_user_dma_map *map, *tmp;
	LIST_HEAD(release);

	mutex_lock(&xdma_user_dma_maps_lock);
	list_for_each_entry_safe(map, tmp, &xdma_reusable_coherent_maps, node) {
		if (map->dev == dev)
			list_move_tail(&map->node, &release);
	}
	mutex_unlock(&xdma_user_dma_maps_lock);

	list_for_each_entry_safe(map, tmp, &release, node) {
		list_del(&map->node);
		xdma_dma_map_release(map);
	}
}

/* Explicit unregister can report BUSY. Final file close cannot: retain an
 * unauthorized map in a non-searchable quarantine list rather than freeing
 * memory still reachable by the endpoint. Recovery is a separate ioctl. */
static int xdma_dma_unmap_file_internal(struct file *file, bool final_close)
{
	struct xdma_user_dma_map *map;
	bool reusable = false;
	int rv = 0;

	mutex_lock(&xdma_user_dma_maps_lock);
	map = xdma_dma_find_file(file);
	if (map && atomic_read(&map->vma_refs)) {
		rv = -EBUSY;
	} else if (map && !map->release_fenced && !map->address_published) {
		/* The endpoint was never given this IOVA; abnormal init exit is safe. */
		map->release_fenced = true;
		list_del(&map->node);
	} else if (map && !map->release_fenced) {
		if (final_close) {
			list_move(&map->node, &xdma_quarantined_dma_maps);
			map->file = NULL;
			__module_get(THIS_MODULE);
			map->module_pinned = true;
			pr_err("quarantined unfenced DMA map dma=%pad length=%zu\n",
			       &map->dma_addr, map->length);
		} else {
			rv = -EBUSY;
		}
	} else if (map && map->coherent_allocated) {
		list_move_tail(&map->node, &xdma_reusable_coherent_maps);
		map->file = NULL;
		map->release_fenced = false;
		map->address_published = false;
		map->rx_accepted_base = 0;
		map->rx_accepted_base_valid = false;
		reusable = true;
	} else if (map) {
		list_del(&map->node);
	}
	mutex_unlock(&xdma_user_dma_maps_lock);

	if (map && !rv && reusable) {
		pr_info("DMA coherent ring pooled: dma=%pad length=%zu sub=%u dir=%u\n",
			&map->dma_addr, map->length, map->allocation_subcard,
			map->allocation_direction);
	} else if (map && !rv && map->release_fenced) {
		pr_info("DMA API diagnostic unmap: dma=%pad length=%zu nents=%u\n",
			&map->dma_addr, map->length, map->sgt.nents);
		xdma_dma_map_release(map);
	}
	return rv;
}

int xdma_dma_unmap_file(struct file *file)
{
	return xdma_dma_unmap_file_internal(file, false);
}

void xdma_dma_close_file(struct file *file)
{
	int rv = xdma_dma_unmap_file_internal(file, true);
	if (rv)
		pr_err("final close retained DMA map: %d\n", rv);
}

static long xdma_dma_map_register(struct file *file,
				  struct xdma_cdev *xcdev, void __user *arg)
{
	struct xdma_ioc_dma_map req;
	struct xdma_user_dma_map *map = NULL;
	struct scatterlist *sg;
	dma_addr_t expected = 0;
	unsigned long start;
	unsigned long nr_pages;
	long pinned;
	size_t remaining;
	unsigned int i;
	int rv = 0;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (!req.user_addr || !req.length ||
	    !IS_ALIGNED(req.user_addr, PAGE_SIZE) ||
	    !IS_ALIGNED(req.length, PAGE_SIZE) || req.length > SZ_1G)
		return -EINVAL;

	start = (unsigned long)req.user_addr;
	nr_pages = req.length >> PAGE_SHIFT;
	if (!nr_pages || nr_pages > UINT_MAX)
		return -EINVAL;

	mutex_lock(&xdma_user_dma_maps_lock);
	if (xdma_dma_find_file(file)) {
		rv = -EBUSY;
		goto out_unlock;
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		rv = -ENOMEM;
		goto out_unlock;
	}
	map->pages = kvmalloc_array(nr_pages, sizeof(*map->pages), GFP_KERNEL);
	if (!map->pages) {
		rv = -ENOMEM;
		goto out_release;
	}
	map->file = file;
	map->dev = &xcdev->xdev->pdev->dev;
	map->length = req.length;

	pinned = pin_user_pages_fast(start, nr_pages,
				     FOLL_WRITE | FOLL_LONGTERM, map->pages);
	if (pinned < 0) {
		rv = pinned;
		goto out_release;
	}
	map->nr_pages = pinned;
	if (pinned != nr_pages) {
		rv = -EFAULT;
		goto out_release;
	}

	rv = sg_alloc_table_from_pages(&map->sgt, map->pages, nr_pages,
				       0, req.length, GFP_KERNEL);
	if (rv)
		goto out_release;

	rv = dma_map_sgtable(map->dev, &map->sgt, DMA_BIDIRECTIONAL, 0);
	if (rv)
		goto out_release;
	map->dma_mapped = true;

	remaining = req.length;
	for_each_sgtable_dma_sg(&map->sgt, sg, i) {
		dma_addr_t addr = sg_dma_address(sg);
		size_t len = sg_dma_len(sg);

		if (!i) {
			map->dma_addr = addr;
			expected = addr;
		}
		if (addr != expected || len > remaining) {
			rv = -ERANGE;
			goto out_release;
		}
		expected += len;
		remaining -= len;
	}
	if (remaining) {
		rv = -ERANGE;
		goto out_release;
	}

	req.dma_addr = map->dma_addr;
	req.mapped_nents = map->sgt.nents;
	req.flags = 1; /* bit 0: all mapped DMA segments are contiguous */
	if (copy_to_user(arg, &req, sizeof(req))) {
		rv = -EFAULT;
		goto out_release;
	}

	list_add(&map->node, &xdma_user_dma_maps);
	pr_info("DMA API diagnostic map: user=0x%llx dma=%pad length=%zu pages=%lu nents=%u\n",
		(unsigned long long)req.user_addr, &map->dma_addr, map->length,
		map->nr_pages, map->sgt.nents);
	mutex_unlock(&xdma_user_dma_maps_lock);
	return 0;

out_release:
	xdma_dma_map_release(map);
out_unlock:
	mutex_unlock(&xdma_user_dma_maps_lock);
	return rv;
}

static long xdma_dma_coherent_alloc(struct file *file,
				    struct xdma_cdev *xcdev, void __user *arg)
{
	struct xdma_ioc_dma_map req;
	struct xdma_user_dma_map *map = NULL;
	bool reused = false;
	long rv = 0;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	u32 owner = req.flags;
	if ((owner & XDMA_DMA_OWNER_MASK) != XDMA_DMA_OWNER_MAGIC ||
	    !req.length || !IS_ALIGNED(req.length, PAGE_SIZE) ||
	    req.length > SZ_256M)
		return -EINVAL;

	mutex_lock(&xdma_user_dma_maps_lock);
	if (xdma_dma_find_file(file)) {
		rv = -EBUSY;
		goto out_unlock;
	}
	map = xdma_dma_find_reusable(&xcdev->xdev->pdev->dev, req.length,
			!!(owner & XDMA_DMA_OWNER_SUBCARD1),
			!!(owner & XDMA_DMA_OWNER_DIRECTION_RX));
	if (map) {
		list_del_init(&map->node);
		map->file = file;
		map->release_fenced = false;
		map->address_published = false;
		map->rx_accepted_base = 0;
		map->rx_accepted_base_valid = false;
		atomic_set(&map->vma_refs, 0);
		reused = true;
		goto publish;
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		rv = -ENOMEM;
		goto out_unlock;
	}
	map->file = file;
	map->dev = &xcdev->xdev->pdev->dev;
	map->length = req.length;
	map->allocation_subcard = !!(owner & XDMA_DMA_OWNER_SUBCARD1);
	map->allocation_direction = !!(owner & XDMA_DMA_OWNER_DIRECTION_RX);
	map->coherent_cpu_addr = dma_alloc_coherent(map->dev, map->length,
						     &map->dma_addr, GFP_KERNEL);
	if (!map->coherent_cpu_addr) {
		rv = -ENOMEM;
		goto out_release;
	}
	map->coherent_allocated = true;
	atomic_set(&map->vma_refs, 0);

publish:
	req.user_addr = 0;
	req.dma_addr = map->dma_addr;
	req.mapped_nents = 1;
	req.flags = owner | 2; /* bit 1: driver-owned dma_alloc_coherent buffer */
	if (copy_to_user(arg, &req, sizeof(req))) {
		rv = -EFAULT;
		goto out_release;
	}

	list_add(&map->node, &xdma_user_dma_maps);
	pr_info("DMA coherent diagnostic %s: cpu=%p dma=%pad length=%zu sub=%u dir=%u\n",
		reused ? "reuse" : "alloc", map->coherent_cpu_addr,
		&map->dma_addr, map->length, map->allocation_subcard,
		map->allocation_direction);
	mutex_unlock(&xdma_user_dma_maps_lock);
	return 0;

out_release:
	if (reused) {
		map->file = NULL;
		list_add_tail(&map->node, &xdma_reusable_coherent_maps);
	} else {
		xdma_dma_map_release(map);
	}
out_unlock:
	mutex_unlock(&xdma_user_dma_maps_lock);
	return rv;
}

static long xdma_dma_sync(struct file *file, void __user *arg, bool for_cpu)
{
	struct xdma_ioc_dma_sync req;
	struct xdma_user_dma_map *map;
	long rv = 0;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&xdma_user_dma_maps_lock);
	map = xdma_dma_find_file(file);
	if (!map) {
		rv = -ENOENT;
		goto out_unlock;
	}
	if (!req.length || req.offset > map->length ||
	    req.length > map->length - req.offset) {
		rv = -EINVAL;
		goto out_unlock;
	}

	if (for_cpu)
		dma_sync_single_range_for_cpu(map->dev, map->dma_addr,
					      req.offset, req.length,
					      DMA_BIDIRECTIONAL);
	else
		dma_sync_single_range_for_device(map->dev, map->dma_addr,
						 req.offset, req.length,
						 DMA_BIDIRECTIONAL);

out_unlock:
	mutex_unlock(&xdma_user_dma_maps_lock);
	return rv;
}

static void xdma_dma_vma_open(struct vm_area_struct *vma)
{
	struct xdma_user_dma_map *map = vma->vm_private_data;
	if (map)
		atomic_inc(&map->vma_refs);
}

static void xdma_dma_vma_close(struct vm_area_struct *vma)
{
	struct xdma_user_dma_map *map = vma->vm_private_data;
	if (map && atomic_dec_return(&map->vma_refs) < 0) {
		pr_err("DMA VMA reference underflow\n");
		atomic_set(&map->vma_refs, 0);
	}
}

static const struct vm_operations_struct xdma_dma_vm_ops = {
	.open = xdma_dma_vma_open,
	.close = xdma_dma_vma_close,
};

static long xdma_dma_recover_quarantined(struct xdma_cdev *xcdev,
					 struct xdma_ioc_dma_release_fence *req)
{
	struct xdma_user_dma_map *map, *tmp;
	struct xdma_dev *xdev = xcdev->xdev;
	void __iomem *config, *user;
	LIST_HEAD(release);
	bool affected[2][2] = {{ false, false }, { false, false }};
	u32 subcard, direction, base, id, control, status, gate, cap;
	u32 recovered = 0;
	int rv = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (xdev->config_bar_idx < 0 || xdev->user_bar_idx < 0)
		return -ENODEV;
	config = xdev->bar[xdev->config_bar_idx];
	user = xdev->bar[xdev->user_bar_idx];
	if (!config || !user)
		return -ENODEV;
	if (req->dma_addr || req->subcard || req->direction ||
	    req->reset_epoch || req->accepted || req->completed)
		return -EINVAL;

	mutex_lock(&xdma_user_dma_maps_lock);
	list_for_each_entry(map, &xdma_quarantined_dma_maps, node) {
		if (map->dev == &xdev->pdev->dev)
			affected[map->allocation_direction][map->allocation_subcard] = true;
	}
	if (!affected[0][0] && !affected[0][1] &&
	    !affected[1][0] && !affected[1][1]) {
		rv = -ENOENT;
		goto out_unlock;
	}

	/* Stop new RX descriptors and allow the current descriptor to finish. */
	for (subcard = 0; subcard < 2; subcard++) {
		if (!affected[1][subcard])
			continue;
		cap = readl(user + 0x8038 + subcard * 0x80);
		if (cap != 0x52585131) { rv = -EPROTONOSUPPORT; goto out_unlock; }
		writel(1, user + 0x803c + subcard * 0x80);
		rv = readl_poll_timeout(user + 0x803c + subcard * 0x80,
					gate, (gate & 0xb) == 0xb, 10, 500000);
		if (rv)
			goto out_unlock;
	}

	/* Halt every engine whose published ring is quarantined. */
	for (direction = 0; direction < 2; direction++) {
		for (subcard = 0; subcard < 2; subcard++) {
			if (!affected[direction][subcard])
				continue;
			base = (direction ? 0x1000 : 0) + subcard * 0x100;
			id = readl(config + base);
			if ((id & 0xffffff00) !=
			    ((direction ? 0x1fc18000 : 0x1fc08000) + subcard * 0x100)) {
				rv = -ENODEV;
				goto out_unlock;
			}
			control = readl(config + base + 4);
			writel(control & ~1U, config + base + 4);
			readl(config + base + 4);
			rv = readl_poll_timeout(config + base + 0x40, status,
						!(status & 1), 10, 500000);
			if (rv || (status & 0x00f8fe38)) {
				if (!rv)
					rv = -EIO;
				goto out_unlock;
			}
		}
	}

	/* A final non-posted BAR read and DMA barrier precede freeing any ring. */
	if (readl(user + 0x8038) != 0x52585131) {
		rv = -EIO;
		goto out_unlock;
	}
	dma_rmb();
	list_for_each_entry_safe(map, tmp, &xdma_quarantined_dma_maps, node) {
		if (map->dev != &xdev->pdev->dev)
			continue;
		list_move_tail(&map->node, &release);
		map->release_fenced = true;
		recovered++;
	}
	req->completed = recovered;

out_unlock:
	mutex_unlock(&xdma_user_dma_maps_lock);
	list_for_each_entry_safe(map, tmp, &release, node) {
		bool pinned = map->module_pinned;
		list_del(&map->node);
		xdma_dma_map_release(map);
		if (pinned)
			module_put(THIS_MODULE);
	}
	if (!rv)
		pr_info("recovered %u quarantined DMA maps after hardware quiesce\n",
			recovered);
	return rv;
}

static long xdma_dma_release_fence(struct file *file, struct xdma_cdev *xcdev,
				   void __user *arg)
{
	struct xdma_ioc_dma_release_fence req;
	struct xdma_user_dma_map *map;
	struct xdma_dev *xdev = xcdev->xdev;
	void __iomem *config, *user;
	u32 base, id, control, status, done, cap;
	long rv = 0;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.flags == XDMA_DMA_STATE_RECOVER)
		return xdma_dma_recover_quarantined(xcdev, &req);
	if (req.subcard > 1 || req.direction > 1 ||
	    req.flags < XDMA_DMA_STATE_PUBLISH ||
	    req.flags > XDMA_DMA_STATE_CANCEL || req.dma_addr == 0)
		return -EINVAL;
	if (xdev->config_bar_idx < 0 || xdev->user_bar_idx < 0)
		return -ENODEV;
	config = xdev->bar[xdev->config_bar_idx];
	user = xdev->bar[xdev->user_bar_idx];
	if (!config || !user)
		return -ENODEV;

	mutex_lock(&xdma_user_dma_maps_lock);
	map = xdma_dma_find_file(file);
	if (!map || !map->coherent_allocated) { rv = -ENOENT; goto out; }
	if (map->dma_addr != req.dma_addr) { rv = -ESTALE; goto out; }
	if (map->allocation_subcard != req.subcard ||
	    map->allocation_direction != req.direction) {
		rv = -EPERM; goto out;
	}
	if (req.flags == XDMA_DMA_STATE_PUBLISH) {
		if (map->address_published || map->release_fenced) rv = -EBUSY;
		else {
			/* The RX accepted counter is deliberately monotonic across
			 * channel-reset epochs, while the XDMA completed counter resets
			 * on each owned RUN generation.  Capture the raw RX baseline when
			 * this C2H ring is first published so the release fence can compare
			 * generation-relative accepted work with XDMA completions. */
			if (req.direction) {
				u32 epoch_before, epoch_after;

				cap = readl(user + 0x8038 + req.subcard * 0x80);
				if (cap != 0x52585131) {
					rv = -EPROTONOSUPPORT;
					goto out;
				}
				epoch_before = readl(user + 0x8044 + req.subcard * 0x80);
				map->rx_accepted_base =
					readl(user + 0x8040 + req.subcard * 0x80);
				epoch_after = readl(user + 0x8044 + req.subcard * 0x80);
				if (epoch_before != epoch_after) {
					rv = -EAGAIN;
					goto out;
				}
				map->rx_accepted_base_valid = true;
			}
			map->address_published = true;
		}
		goto out;
	}
	if (req.flags == XDMA_DMA_STATE_CANCEL) {
		if (map->address_published) rv = -EBUSY;
		else map->release_fenced = true;
		goto out;
	}
	if (!map->address_published ||
	    (!req.direction && req.accepted != req.completed)) {
		rv = -EINVAL; goto out;
	}
	base = (req.direction ? 0x1000 : 0) + req.subcard * 0x100;
	id = readl(config + base);
	control = readl(config + base + 4);
	status = readl(config + base + 0x40);
	done = readl(config + base + 0x48);
	if ((id & 0xffffff00) != ((req.direction ? 0x1fc18000 : 0x1fc08000) +
					 req.subcard * 0x100) || (control & 1) ||
	    (status & 1) || (status & 0x00f8fe38) || done != req.completed) {
		rv = -EBUSY; goto out;
	}
	cap = readl(user + 0x8038 + req.subcard * 0x80);
	if (cap != 0x52585131) { rv = -EPROTONOSUPPORT; goto out; }
	if (req.direction) {
		u32 gate = readl(user + 0x803c + req.subcard * 0x80);
		u32 accepted = readl(user + 0x8040 + req.subcard * 0x80);
		u32 epoch = readl(user + 0x8044 + req.subcard * 0x80);
		u32 generation_accepted;

		if (!map->rx_accepted_base_valid) {
			rv = -EINVAL; goto out;
		}
		generation_accepted = accepted - map->rx_accepted_base;
		if (gate != 11 || accepted != req.accepted ||
		    epoch != req.reset_epoch ||
		    generation_accepted != req.completed ||
		    generation_accepted > 0x3fffffffU) {
			rv = -ESTALE; goto out;
		}
	}
	/* Non-posted read completion orders prior device DMA writes; dma_rmb orders
	 * subsequent CPU access/free after the observed device completion. */
	if (readl(user + 0x8038 + req.subcard * 0x80) != 0x52585131) {
		rv = -EIO; goto out;
	}
	dma_rmb();
	map->release_fenced = true;
	map->fence_subcard = req.subcard;
	map->fence_direction = req.direction;
	pr_info("DMA release fenced: dma=%pad sub=%u dir=%u done=%u epoch=%u rx_base=%u raw_accepted=%u\n",
		&map->dma_addr, req.subcard, req.direction, req.completed,
		req.reset_epoch, map->rx_accepted_base, req.accepted);
out:
	mutex_unlock(&xdma_user_dma_maps_lock);
	return rv;
}

/*
 * character device file operations for control bus (through control bridge)
 */
static ssize_t char_ctrl_read(struct file *fp, char __user *buf, size_t count,
		loff_t *pos)
{
	struct xdma_cdev *xcdev = (struct xdma_cdev *)fp->private_data;
	struct xdma_dev *xdev;
	void __iomem *reg;
	u32 w;
	int rv;

	rv = xcdev_check(__func__, xcdev, 0);
	if (rv < 0)
		return rv;
	xdev = xcdev->xdev;

	/* only 32-bit aligned and 32-bit multiples */
	if (*pos & 3)
		return -EPROTO;
	/* first address is BAR base plus file position offset */
	reg = xdev->bar[xcdev->bar] + *pos;
	//w = read_register(reg);
	w = ioread32(reg);
	dbg_sg("%s(@%p, count=%ld, pos=%d) value = 0x%08x\n",
			__func__, reg, (long)count, (int)*pos, w);
	rv = copy_to_user(buf, &w, 4);
	if (rv)
		dbg_sg("Copy to userspace failed but continuing\n");

	*pos += 4;
	return 4;
}

static ssize_t char_ctrl_write(struct file *file, const char __user *buf,
			size_t count, loff_t *pos)
{
	struct xdma_cdev *xcdev = (struct xdma_cdev *)file->private_data;
	struct xdma_dev *xdev;
	void __iomem *reg;
	u32 w;
	int rv;

	rv = xcdev_check(__func__, xcdev, 0);
	if (rv < 0)
		return rv;
	xdev = xcdev->xdev;

	/* only 32-bit aligned and 32-bit multiples */
	if (*pos & 3)
		return -EPROTO;

	/* first address is BAR base plus file position offset */
	reg = xdev->bar[xcdev->bar] + *pos;
	rv = copy_from_user(&w, buf, 4);
	if (rv)
		pr_info("copy from user failed %d/4, but continuing.\n", rv);

	dbg_sg("%s(0x%08x @%p, count=%ld, pos=%d)\n",
			__func__, w, reg, (long)count, (int)*pos);
	//write_register(w, reg);
	iowrite32(w, reg);
	*pos += 4;
	return 4;
}

static long version_ioctl(struct xdma_cdev *xcdev, void __user *arg)
{
	struct xdma_ioc_info obj;
	struct xdma_dev *xdev = xcdev->xdev;
	int rv;

	rv = copy_from_user((void *)&obj, arg, sizeof(struct xdma_ioc_info));
	if (rv) {
		pr_info("copy from user failed %d/%ld.\n",
			rv, sizeof(struct xdma_ioc_info));
		return -EFAULT;
	}
	memset(&obj, 0, sizeof(obj));
	obj.vendor = xdev->pdev->vendor;
	obj.device = xdev->pdev->device;
	obj.subsystem_vendor = xdev->pdev->subsystem_vendor;
	obj.subsystem_device = xdev->pdev->subsystem_device;
	obj.feature_id = xdev->feature_id;
	obj.driver_version = DRV_MOD_VERSION_NUMBER;
	obj.domain = 0;
	//obj.bus = PCI_BUS_NUM(xdev->pdev->devfn);
	obj.bus = xdev->pdev->bus->number;
	obj.dev = PCI_SLOT(xdev->pdev->devfn);
	obj.func = PCI_FUNC(xdev->pdev->devfn);
	if (copy_to_user(arg, &obj, sizeof(struct xdma_ioc_info)))
		return -EFAULT;
	return 0;
}

long char_ctrl_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct xdma_cdev *xcdev = (struct xdma_cdev *)filp->private_data;
	struct xdma_dev *xdev;
	struct xdma_ioc_base ioctl_obj;
	long result = 0;
	int rv;

	rv = xcdev_check(__func__, xcdev, 0);
	if (rv < 0)
		return rv;

	xdev = xcdev->xdev;
	if (!xdev) {
		pr_info("cmd %u, xdev NULL.\n", cmd);
		return -EINVAL;
	}
	//pr_info("cmd 0x%x, xdev 0x%p, pdev 0x%p.\n", cmd, xdev, xdev->pdev);

	if (_IOC_TYPE(cmd) != XDMA_IOC_MAGIC) {
		pr_err("cmd %u, bad magic 0x%x/0x%x.\n",
			 cmd, _IOC_TYPE(cmd), XDMA_IOC_MAGIC);
		return -ENOTTY;
	}

	if (_IOC_DIR(cmd) & _IOC_READ)
		result = !xlx_access_ok(VERIFY_WRITE, (void __user *)arg,
				_IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		result =  !xlx_access_ok(VERIFY_READ, (void __user *)arg,
				_IOC_SIZE(cmd));

	if (result) {
		pr_err("bad access %ld.\n", result);
		return -EFAULT;
	}

	switch (cmd) {
	case XDMA_IOCINFO:
		if (copy_from_user((void *)&ioctl_obj, (void __user *) arg,
			 sizeof(struct xdma_ioc_base))) {
			pr_err("copy_from_user failed.\n");
			return -EFAULT;
		}

		if (ioctl_obj.magic != XDMA_XCL_MAGIC) {
			pr_err("magic 0x%x !=  XDMA_XCL_MAGIC (0x%x).\n",
				ioctl_obj.magic, XDMA_XCL_MAGIC);
			return -ENOTTY;
		}
		return version_ioctl(xcdev, (void __user *)arg);
	case XDMA_IOCNUMANODE:
		return put_user(xdev->pdev->dev.numa_node, (int __user *)arg);
	case XDMA_IOCDMAMAPREGISTER:
		return xdma_dma_map_register(filp, xcdev, (void __user *)arg);
	case XDMA_IOCDMAMAPUNREGISTER:
		return xdma_dma_unmap_file(filp);
	case XDMA_IOCDMASYNCFORCPU:
		return xdma_dma_sync(filp, (void __user *)arg, true);
	case XDMA_IOCDMASYNCFORDEVICE:
		return xdma_dma_sync(filp, (void __user *)arg, false);
	case XDMA_IOCDMACOHERENTALLOC:
		return xdma_dma_coherent_alloc(filp, xcdev, (void __user *)arg);
	case XDMA_IOCDMARELEASEFENCE:
		return xdma_dma_release_fence(filp, xcdev, (void __user *)arg);
	case XDMA_IOCOFFLINE:
		xdma_device_offline(xdev->pdev, xdev);
		break;
	case XDMA_IOCONLINE:
		xdma_device_online(xdev->pdev, xdev);
		break;
	default:
		pr_err("UNKNOWN ioctl cmd 0x%x.\n", cmd);
		return -ENOTTY;
	}
	return 0;
}

/* maps the PCIe BAR into user space for memory-like access using mmap() */
int bridge_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct xdma_dev *xdev;
	struct xdma_cdev *xcdev = (struct xdma_cdev *)file->private_data;
	unsigned long off;
	unsigned long phys;
	unsigned long vsize;
	unsigned long psize;
	unsigned long saved_pgoff;
	struct xdma_user_dma_map *map;
	int rv;

	rv = xcdev_check(__func__, xcdev, 0);
	if (rv < 0)
		return rv;
	xdev = xcdev->xdev;

	off = vma->vm_pgoff << PAGE_SHIFT;
	vsize = vma->vm_end - vma->vm_start;
	if (off == XDMA_DMA_COHERENT_MMAP_OFFSET) {
		mutex_lock(&xdma_user_dma_maps_lock);
		map = xdma_dma_find_file(file);
		if (!map || !map->coherent_allocated) {
			rv = -ENOENT;
		} else if (vsize > map->length) {
			rv = -EINVAL;
		} else {
			/* dma_mmap_coherent expects an offset relative to the buffer. */
			saved_pgoff = vma->vm_pgoff;
			vma->vm_pgoff = 0;
			rv = dma_mmap_coherent(map->dev, vma,
					       map->coherent_cpu_addr,
					       map->dma_addr, map->length);
			vma->vm_pgoff = saved_pgoff;
			if (!rv) {
				vma->vm_ops = &xdma_dma_vm_ops;
				vma->vm_private_data = map;
				xdma_dma_vma_open(vma);
			}
		}
		mutex_unlock(&xdma_user_dma_maps_lock);
		return rv;
	}
	/* BAR physical address */
	phys = pci_resource_start(xdev->pdev, xcdev->bar) + off;
	/* complete resource */
	psize = pci_resource_end(xdev->pdev, xcdev->bar) -
		pci_resource_start(xdev->pdev, xcdev->bar) + 1 - off;

	dbg_sg("mmap(): xcdev = 0x%08lx\n", (unsigned long)xcdev);
	dbg_sg("mmap(): cdev->bar = %d\n", xcdev->bar);
	dbg_sg("mmap(): xdev = 0x%p\n", xdev);
	dbg_sg("mmap(): pci_dev = 0x%08lx\n", (unsigned long)xdev->pdev);
	dbg_sg("off = 0x%lx, vsize 0x%lu, psize 0x%lu.\n", off, vsize, psize);
	dbg_sg("start = 0x%llx\n",
		(unsigned long long)pci_resource_start(xdev->pdev,
		xcdev->bar));
	dbg_sg("phys = 0x%lx\n", phys);

	if (vsize > psize)
		return -EINVAL;
	/*
	 * pages must not be cached as this would result in cache line sized
	 * accesses to the end point
	 */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	/*
	 * prevent touching the pages (byte access) for swap-in,
	 * and prevent the pages from being swapped out
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
        vm_flags_set(vma, VMEM_FLAGS);
#elif defined(RHEL_RELEASE_CODE)
	#if (RHEL_RELEASE_CODE > RHEL_RELEASE_VERSION(9, 4))
        vm_flags_set(vma, VMEM_FLAGS);
	#else
	vma->vm_flags |= VMEM_FLAGS;
	#endif
#else
	vma->vm_flags |= VMEM_FLAGS;
#endif
	/* make MMIO accessible to user space */
	rv = io_remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT,
			vsize, vma->vm_page_prot);
	dbg_sg("vma=0x%p, vma->vm_start=0x%lx, phys=0x%lx, size=%lu = %d\n",
		vma, vma->vm_start, phys >> PAGE_SHIFT, vsize, rv);

	if (rv)
		return -EAGAIN;
	return 0;
}

/*
 * character device file operations for control bus (through control bridge)
 */
static const struct file_operations ctrl_fops = {
	.owner = THIS_MODULE,
	.open = char_open,
	.release = char_close,
	.read = char_ctrl_read,
	.write = char_ctrl_write,
	.mmap = bridge_mmap,
	.unlocked_ioctl = char_ctrl_ioctl,
};

void cdev_ctrl_init(struct xdma_cdev *xcdev)
{
	cdev_init(&xcdev->cdev, &ctrl_fops);
}
