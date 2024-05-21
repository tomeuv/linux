// SPDX-License-Identifier: GPL-2.0
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include "asm-generic/delay.h"
#include "linux/pm_runtime.h"
#include <linux/clk.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/reset.h>

#include "rocket_drv.h"
#include "rocket_device.h"

static int rocket_clk_init(struct rocket_device *rdev)
{
	int err;

	rdev->num_clks = devm_clk_bulk_get_all(rdev->dev, &rdev->clks);
	if (rdev->num_clks < 1) {
		dev_err(rdev->dev, "devm_clk_bulk_get_all failed %d\n", rdev->num_clks);
		return rdev->num_clks;
	}

	err = clk_bulk_prepare_enable(rdev->num_clks, rdev->clks);
	if (err) {
		dev_err(rdev->dev, "failed to enable clk for rknpu, ret: %d\n", err);
		return err;
	}

	return 0;
}

static void rocket_clk_fini(struct rocket_device *rdev)
{
	clk_bulk_disable_unprepare(rdev->num_clks, rdev->clks);
}

static int rocket_reset_init(struct rocket_device *rdev)
{
	struct reset_control *srst_a = NULL;
	struct reset_control *srst_h = NULL;
	int i = 0;

	for (i = 0; i < rdev->comp->num_resets; i++) {
		srst_a = devm_reset_control_get(
			rdev->dev,
			rdev->comp->reset_a_names[i]);
		if (IS_ERR(srst_a))
			return PTR_ERR(srst_a);

		rdev->srst_a[i] = srst_a;

		srst_h = devm_reset_control_get(
			rdev->dev,
			rdev->comp->reset_h_names[i]);
		if (IS_ERR(srst_h))
			return PTR_ERR(srst_h);

		rdev->srst_h[i] = srst_h;
	}

	return 0;
}

static void rocket_reset_fini(struct rocket_device *rdev)
{

}

static void rocket_pm_domain_fini(struct rocket_device *rdev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rdev->pm_domain_devs); i++) {
		if (!rdev->pm_domain_devs[i])
			break;

		if (rdev->pm_domain_links[i])
			device_link_del(rdev->pm_domain_links[i]);

		dev_pm_domain_detach(rdev->pm_domain_devs[i], true);
	}
}

static int rocket_pm_domain_init(struct rocket_device *rdev)
{
	int err;
	int i, num_domains;

	num_domains = of_count_phandle_with_args(rdev->dev->of_node,
						"power-domains",
						"#power-domain-cells");

	/*
	 * Single domain is handled by the core, and, if only a single power
	 * the power domain is requested, the property is optional.
	 */
	if (num_domains < 2 && rdev->comp->num_pm_domains < 2)
		return 0;

	if (num_domains != rdev->comp->num_pm_domains) {
		dev_err(rdev->dev,
			"Incorrect number of power domains: %d provided, %d needed\n",
			num_domains, rdev->comp->num_pm_domains);
		return -EINVAL;
	}

	if (WARN(num_domains > ARRAY_SIZE(rdev->pm_domain_devs),
			"Too many supplies in compatible structure.\n"))
		return -EINVAL;

	for (i = 0; i < num_domains; i++) {
		rdev->pm_domain_devs[i] =
			dev_pm_domain_attach_by_name(rdev->dev,
					rdev->comp->pm_domain_names[i]);
		if (IS_ERR_OR_NULL(rdev->pm_domain_devs[i])) {
			err = PTR_ERR(rdev->pm_domain_devs[i]) ? : -ENODATA;
			rdev->pm_domain_devs[i] = NULL;
			dev_err(rdev->dev,
				"failed to get pm-domain %s(%d): %d\n",
				rdev->comp->pm_domain_names[i], i, err);
			goto err;
		}

		rdev->pm_domain_links[i] = device_link_add(rdev->dev,
				rdev->pm_domain_devs[i], DL_FLAG_PM_RUNTIME |
				DL_FLAG_STATELESS | DL_FLAG_RPM_ACTIVE);
		if (!rdev->pm_domain_links[i]) {
			dev_err(rdev->pm_domain_devs[i],
				"adding device link failed!\n");
			err = -ENODEV;
			goto err;
		}

		/* TODO: Should we also add a link to the IOMMU so the PDs don't go off before it? */
	}

	return 0;

err:
	rocket_pm_domain_fini(rdev);
	return err;
}

int rocket_device_init(struct rocket_device *rdev)
{
	int i, err;
	uint32_t version;

	err = rocket_clk_init(rdev);
	if (err) {
		dev_err(rdev->dev, "clk init failed %d\n", err);
		return err;
	}

	err = rocket_reset_init(rdev);
	if (err) {
		dev_err(rdev->dev, "reset init failed %d\n", err);
		goto out_clk;
	}

	err = rocket_pm_domain_init(rdev);
	if (err)
		goto out_reset;

	for (i = 0; i < rdev->comp->num_iomem; i++) {
		rdev->iomem[i] = devm_platform_ioremap_resource(rdev->pdev, i);
		if (IS_ERR(rdev->iomem)) {
			err = PTR_ERR(rdev->iomem);
			goto out_pm_domain;
		}
	}

	err = rocket_job_init(rdev);
	if (err)
		goto out_pm_domain;

	version = rocket_read(rdev, 0, RKNN_OFFSET_VERSION) + (rocket_read(rdev, 0, RKNN_OFFSET_VERSION_NUM) & 0xffff);
	dev_info(rdev->dev, "Rockchip NPU version: %d\n", version);

	return 0;

out_pm_domain:
	rocket_pm_domain_fini(rdev);
out_reset:
	rocket_reset_fini(rdev);
out_clk:
	rocket_clk_fini(rdev);
	return err;
}

void rocket_device_fini(struct rocket_device *rdev)
{
	rocket_pm_domain_fini(rdev);
	rocket_reset_fini(rdev);
	rocket_clk_fini(rdev);
}

void rocket_device_reset(struct rocket_device *rdev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < rdev->comp->num_resets; i++) {
		ret = reset_control_assert(rdev->srst_a[i]);
		ret |= reset_control_assert(rdev->srst_h[i]);

		udelay(10);

		ret |= reset_control_deassert(rdev->srst_a[i]);
		ret |= reset_control_deassert(rdev->srst_h[i]);
	}

	rocket_job_enable_interrupts(rdev);
}

static int rocket_device_runtime_resume(struct device *dev)
{
	struct rocket_device *rdev = dev_get_drvdata(dev);

	rocket_device_reset(rdev);
	//rocket_devfreq_resume(rdev);

	return 0;
}

static int rocket_device_runtime_suspend(struct device *dev)
{
	struct rocket_device *rdev = dev_get_drvdata(dev);

	if (!rocket_job_is_idle(rdev))
		return -EBUSY;

	//rocket_devfreq_suspend(rdev);
	rocket_job_suspend_irq(rdev);
	//rocket_mmu_suspend_irq(rdev);
	//rocket_gpu_suspend_irq(rdev);
	//rocket_gpu_power_off(rdev);

	return 0;
}

static int rocket_device_resume(struct device *dev)
{
	return pm_runtime_force_resume(dev);
}

static int rocket_device_suspend(struct device *dev)
{
	return pm_runtime_force_suspend(dev);
}

EXPORT_GPL_DEV_PM_OPS(rocket_pm_ops) = {
	RUNTIME_PM_OPS(rocket_device_runtime_suspend, rocket_device_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(rocket_device_suspend, rocket_device_resume)
};