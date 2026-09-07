/*
 * This file is part of the Xilinx DMA IP Core driver tools for Linux
 *
 * Copyright (c) 2016-present,  Xilinx, Inc.
 * All rights reserved.
 *
 * This source code is licensed under BSD-style license (found in the
 * LICENSE file in the root directory of this source tree)
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../xdma/cdev_sgdma.h"


int test_dma(char *device_name, int size, int count);

static int verbosity = 0;

int main(int argc, char *argv[])
{
    int cmd_opt;
    char *device = "/dev/xdma0_h2c_0";

    int rc = 0;
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        printf("FAILURE: Could not open %s. Make sure xdma device driver is loaded and you have access rights (maybe use sudo?).\n", device);
        exit(1);
    }

    unsigned char status = 1;

    int numa_node = -1;
    rc = ioctl(fd, IOCTL_XDMA_NUMANODE_GET, &numa_node);
    if (rc == 0) {
        printf("IOCTL_XDMA_NUMANODE_GET succesful.\n");
    } else {
        printf("ioctl(..., IOCTL_XDMA_NUMANODE_GET) = %d\n", rc);
    }

    printf("numa_node = %d\n", numa_node);

    close(fd);
}
