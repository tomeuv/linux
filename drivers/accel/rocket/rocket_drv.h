/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#ifndef __ROCKET_DRV_H__
#define __ROCKET_DRV_H__

#include <linux/io.h>
#include <drm/gpu_scheduler.h>

struct rocket_file_priv {
       struct rocket_device *rdev;

       struct drm_sched_entity sched_entity;
};

#define rocket_read(dev, core, reg) readl(dev->iomem[core] + (reg))
#define rocket_write(dev, core, reg, value) writel(value, dev->iomem[core] + (reg))

#endif