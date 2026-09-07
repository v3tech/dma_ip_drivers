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

#ifndef _XDMA_IOCALLS_POSIX_H_
#define _XDMA_IOCALLS_POSIX_H_

#include <linux/ioctl.h>
#include <linux/types.h>

/* Use 'x' as magic number */
#define XDMA_IOC_MAGIC	'x'
/* XL OpenCL X->58(ASCII), L->6C(ASCII), O->0 C->C L->6C(ASCII); */
#define XDMA_XCL_MAGIC 0X586C0C6C

/*
 * S means "Set" through a ptr,
 * T means "Tell" directly with the argument value
 * G means "Get": reply by setting through a pointer
 * Q means "Query": response is on the return value
 * X means "eXchange": switch G and S atomically
 * H means "sHift": switch T and Q atomically
 *
 * _IO(type,nr)		    no arguments
 * _IOR(type,nr,datatype)   read data from driver
 * _IOW(type,nr.datatype)   write data to driver
 * _IORW(type,nr,datatype)  read/write data
 *
 * _IOC_DIR(nr)		    returns direction
 * _IOC_TYPE(nr)	    returns magic
 * _IOC_NR(nr)		    returns number
 * _IOC_SIZE(nr)	    returns size
 */

enum XDMA_IOC_TYPES {
	XDMA_IOC_NOP,
	XDMA_IOC_INFO,
	XDMA_IOC_OFFLINE,
	XDMA_IOC_ONLINE,
	XDMA_IOC_NUMANODE,
	XDMA_IOC_DMA_MAP_REGISTER,
	XDMA_IOC_DMA_MAP_UNREGISTER,
	XDMA_IOC_DMA_SYNC_FOR_CPU,
	XDMA_IOC_DMA_SYNC_FOR_DEVICE,
	XDMA_IOC_DMA_COHERENT_ALLOC,
	XDMA_IOC_DMA_RELEASE_FENCE,
	XDMA_IOC_MAX
};

#define XDMA_DMA_COHERENT_MMAP_OFFSET	0x40000000ULL
#define XDMA_DMA_OWNER_MAGIC		0x84000000U
#define XDMA_DMA_OWNER_DIRECTION_RX	0x00000100U
#define XDMA_DMA_OWNER_SUBCARD1		0x00000200U
#define XDMA_DMA_OWNER_MASK		0xfffffcffU

/*
 * Diagnostic interface for a persistent user-buffer DMA mapping.  The XDMA
 * bypass RTL consumes one linear DMA address, so the driver rejects mappings
 * whose DMA segments are not contiguous.  This is intentionally opt-in and
 * does not alter the normal XDMA read/write path.
 */
struct xdma_ioc_dma_map {
	__u64 user_addr;
	__u64 length;
	__u64 dma_addr;
	__u32 mapped_nents;
	__u32 flags; /* in: OWNER_MAGIC|direction|subcard; out: same|type bits */
};

struct xdma_ioc_dma_sync {
	__u64 offset;
	__u64 length;
};

/* Authorize release of one coherent ring only after the matching engine has
 * stopped. direction: 0 H2C (device reads host), 1 C2H (device writes host).
 * For RX, accepted is the absolute monotonic FPGA counter and completed is the
 * generation-relative XDMA counter; the driver records the accepted baseline
 * when the C2H address is published. TX accepted must equal completed.
 */
struct xdma_ioc_dma_release_fence {
	__u64 dma_addr;
	__u32 subcard;
	__u32 direction;
	__u32 reset_epoch;
	__u32 accepted;
	__u32 completed;
	__u32 flags; /* 1 publish, 2 completed fence, 3 cancel, 4 admin recovery */
};
#define XDMA_DMA_STATE_PUBLISH	1U
#define XDMA_DMA_STATE_FENCE	2U
#define XDMA_DMA_STATE_CANCEL	3U
#define XDMA_DMA_STATE_RECOVER	4U

struct xdma_ioc_base {
	unsigned int magic;
	unsigned int command;
};

struct xdma_ioc_info {
	struct xdma_ioc_base	base;
	unsigned short		vendor;
	unsigned short		device;
	unsigned short		subsystem_vendor;
	unsigned short		subsystem_device;
	unsigned int		dma_engine_version;
	unsigned int		driver_version;
	unsigned long long	feature_id;
	unsigned short		domain;
	unsigned char		bus;
	unsigned char		dev;
	unsigned char		func;
};

/* IOCTL codes */
#define XDMA_IOCINFO		_IOWR(XDMA_IOC_MAGIC, XDMA_IOC_INFO, \
					struct xdma_ioc_info)
#define XDMA_IOCOFFLINE		_IO(XDMA_IOC_MAGIC, XDMA_IOC_OFFLINE)
#define XDMA_IOCONLINE		_IO(XDMA_IOC_MAGIC, XDMA_IOC_ONLINE)
#define XDMA_IOCNUMANODE		_IOR(XDMA_IOC_MAGIC, XDMA_IOC_NUMANODE, int)
#define XDMA_IOCDMAMAPREGISTER	_IOWR(XDMA_IOC_MAGIC, \
					XDMA_IOC_DMA_MAP_REGISTER, \
					struct xdma_ioc_dma_map)
#define XDMA_IOCDMAMAPUNREGISTER _IO(XDMA_IOC_MAGIC, \
					 XDMA_IOC_DMA_MAP_UNREGISTER)
#define XDMA_IOCDMASYNCFORCPU	_IOW(XDMA_IOC_MAGIC, \
					 XDMA_IOC_DMA_SYNC_FOR_CPU, \
					 struct xdma_ioc_dma_sync)
#define XDMA_IOCDMASYNCFORDEVICE _IOW(XDMA_IOC_MAGIC, \
					  XDMA_IOC_DMA_SYNC_FOR_DEVICE, \
					  struct xdma_ioc_dma_sync)
#define XDMA_IOCDMACOHERENTALLOC _IOWR(XDMA_IOC_MAGIC, \
					 XDMA_IOC_DMA_COHERENT_ALLOC, \
					 struct xdma_ioc_dma_map)
#define XDMA_IOCDMARELEASEFENCE _IOW(XDMA_IOC_MAGIC, \
					 XDMA_IOC_DMA_RELEASE_FENCE, \
					 struct xdma_ioc_dma_release_fence)

#define IOCTL_XDMA_ADDRMODE_SET	_IOW('q', 4, int)
#define IOCTL_XDMA_ADDRMODE_GET	_IOR('q', 5, int)
#define IOCTL_XDMA_ALIGN_GET	_IOR('q', 6, int)

#endif /* _XDMA_IOCALLS_POSIX_H_ */
