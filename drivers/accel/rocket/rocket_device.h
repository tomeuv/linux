/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#ifndef __ROCKET_DEVICE_H__
#define __ROCKET_DEVICE_H__

#include <drm/gpu_scheduler.h>
#include <linux/mutex_types.h>

#define MAX_NUM_CORES 3

/* Taken verbatim from downstream driver */
/* TODO: Switch to the definitions generated from the TRM in the Mesa tree */
#define RKNN_OFFSET_VERSION 0x0
#define RKNN_OFFSET_VERSION_NUM 0x4
#define RKNN_OFFSET_PC_OP_EN 0x8
#define RKNN_OFFSET_PC_DATA_ADDR 0x10
#define RKNN_OFFSET_PC_DATA_AMOUNT 0x14
#define RKNN_OFFSET_PC_TASK_CONTROL 0x30
#define RKNN_OFFSET_PC_DMA_BASE_ADDR 0x34

#define RKNN_OFFSET_INT_MASK 0x20
#define RKNN_OFFSET_INT_CLEAR 0x24
#define RKNN_OFFSET_INT_STATUS 0x28
#define RKNN_OFFSET_INT_RAW_STATUS 0x2c

#define RKNN_OFFSET_CLR_ALL_RW_AMOUNT 0x8010
#define RKNN_OFFSET_DT_WR_AMOUNT 0x8034
#define RKNN_OFFSET_DT_RD_AMOUNT 0x8038
#define RKNN_OFFSET_WT_RD_AMOUNT 0x803c

#define RKNN_OFFSET_ENABLE_MASK 0xf008

#define RKNN_INT_CLEAR 0x1ffff

#define RKNN_CNA_FEATURE_GROUP_0	0
#define RKNN_CNA_FEATURE_GROUP_1	1
#define RKNN_CNA_WEIGHT_GROUP_0	2
#define RKNN_CNA_WEIGHT_GROUP_1	3
#define RKNN_CNA_CSC_GROUP_0		4
#define RKNN_CNA_CSC_GROUP_1		5
#define RKNN_CORE_GROUP_0		6
#define RKNN_CORE_GROUP_1		7
#define RKNN_DPU_GROUP_0		8
#define RKNN_DPU_GROUP_1		9
#define RKNN_PPU_GROUP_0		10
#define RKNN_PPU_GROUP_1		11
#define RKNN_DMA_READ_ERROR		12
#define RKNN_DMA_WRITE_ERROR		13

struct rocket_compatible {
	int num_resets;
	const char * const *reset_a_names;
	const char * const *reset_h_names;

	int num_pm_domains;
	const char * const *pm_domain_names;

	int num_irqs;
	const char * const *irq_names;

	int num_iomem;
};

struct rocket_queue_state {
       struct drm_gpu_scheduler sched;
       u64 fence_context;
       u64 emit_seqno;
};

struct rocket_device {
	struct device *dev;
	struct drm_device *ddev;
	struct platform_device *pdev;

	const struct rocket_compatible *comp;

	struct clk_bulk_data *clks;
	int num_clks;

	struct reset_control *srst_a[MAX_NUM_CORES];
	struct reset_control *srst_h[MAX_NUM_CORES];

	void __iomem *iomem[MAX_NUM_CORES];

	struct device *pm_domain_devs[MAX_NUM_CORES];
	struct device_link *pm_domain_links[MAX_NUM_CORES];

	struct mutex sched_lock;

	struct {
		struct workqueue_struct *wq;
		struct work_struct work;
		atomic_t pending;
	} reset;

	struct rocket_queue_state queue;
	spinlock_t job_lock;
	int irq[MAX_NUM_CORES];

	struct rocket_job *jobs[MAX_NUM_CORES];
};

int rocket_device_init(struct rocket_device *rdev);
void rocket_device_fini(struct rocket_device *rdev);
void rocket_device_reset(struct rocket_device *rdev);

#endif