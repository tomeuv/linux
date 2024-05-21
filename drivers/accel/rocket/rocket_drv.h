/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#ifndef __ROCKET_DRV_H__
#define __ROCKET_DRV_H__

#include <linux/io.h>

struct rocket_file_priv {
       struct rocket_device *rdev;
};

#define rocket_read(dev, core, reg) readl(dev->iomem[core] + (reg))
#define rocket_write(dev, core, reg, value) writel(value, dev->iomem[core] + (reg)); printk("writel: 0x%x 0x%llx\n", dev->iomem[core] + (reg), (uint64_t)value);

#endif