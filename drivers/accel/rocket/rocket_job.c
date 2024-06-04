// SPDX-License-Identifier: GPL-2.0
/* Copyright 2019 Linaro, Ltd, Rob Herring <robh@kernel.org> */
/* Copyright 2019 Collabora ltd. */
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include "drm/drm_utils.h"
#include "linux/kernel.h"
#include <drm/drm_syncobj.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <drm/rocket_drm.h>
#include <drm/drm_drv.h>

#include "rocket_job.h"
#include "rocket_drv.h"
#include "rocket_device.h"
#include "rocket_gem.h"

#define JOB_TIMEOUT_MS 500

#define job_write(dev, reg, data) writel(data, dev->iomem + (reg))
#define job_read(dev, reg) readl(dev->iomem + (reg))

static struct rocket_job *
to_rocket_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct rocket_job, base);
}

struct rocket_fence {
	struct dma_fence base;
	struct drm_device *dev;
	/* rocket seqno for signaled() test */
	u64 seqno;
	int queue;
};

static inline struct rocket_fence *
to_rocket_fence(struct dma_fence *fence)
{
	return (struct rocket_fence *)fence;
}

static const char *rocket_fence_get_driver_name(struct dma_fence *fence)
{
	return "rocket";
}

static const char *rocket_fence_get_timeline_name(struct dma_fence *fence)
{
	return "rockchip-npu";
}

static const struct dma_fence_ops rocket_fence_ops = {
	.get_driver_name = rocket_fence_get_driver_name,
	.get_timeline_name = rocket_fence_get_timeline_name,
};

static struct dma_fence *rocket_fence_create(struct rocket_device *rdev)
{
	struct rocket_fence *fence;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return ERR_PTR(-ENOMEM);

	fence->dev = rdev->ddev;
	fence->seqno = ++rdev->queue.emit_seqno;
	dma_fence_init(&fence->base, &rocket_fence_ops, &rdev->job_lock,
		       rdev->queue.fence_context, fence->seqno);

	return &fence->base;
}

static int
rocket_copy_tasks(struct drm_device *dev,
		  struct drm_file *file_priv,
		  struct drm_rocket_job *job,
		  struct rocket_job *rjob)
{
	struct drm_rocket_task *tasks;
	int ret = 0;
	int i;

	rjob->task_count = job->task_count;

	if (!rjob->task_count)
		return 0;

	tasks = kvmalloc_array(rjob->task_count, sizeof(*tasks), GFP_KERNEL);
	if (!tasks) {
		ret = -ENOMEM;
		DRM_DEBUG("Failed to allocate incoming tasks\n");
		goto fail;
	}

	if (copy_from_user(tasks,
			   (void __user *)(uintptr_t)job->tasks,
			   rjob->task_count * sizeof(*tasks))) {
		ret = -EFAULT;
		DRM_DEBUG("Failed to copy incoming tasks\n");
		goto fail;
	}

	rjob->tasks = kvmalloc_array(job->task_count, sizeof(*rjob->tasks), GFP_KERNEL);
	if (!rjob->tasks) {
		DRM_DEBUG("Failed to allocate task array\n");
		ret = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < rjob->task_count; i++) {
		if (tasks[i].regcmd_count == 0) {
			ret = -EINVAL;
			goto fail;
		}
		rjob->tasks[i].regcmd = tasks[i].regcmd;
		rjob->tasks[i].regcmd_count = tasks[i].regcmd_count;
	}

fail:
	kvfree(tasks);
	return ret;
}

static struct rocket_job *
rocket_dequeue_job(struct rocket_device *rdev, unsigned int core)
{
	struct rocket_job *job = rdev->jobs[core];

	WARN_ON(!job);

	rdev->jobs[core] = NULL;

	return job;
}

static unsigned int
rocket_enqueue_job(struct rocket_device *rdev, struct rocket_job *job)
{
	if (WARN_ON(!job))
		return 0;

	for (int i = 0; i < MAX_NUM_CORES; i++) {
		if (rdev->jobs[i] == NULL) {
			rdev->jobs[i] = job;
			return i;
		}
	}

	BUG_ON("All NPU cores are busy.");

	return 0;
}

static void rocket_job_hw_submit(struct rocket_job *job)
{
	struct rocket_device *rdev = job->rdev;
	struct rocket_task *task;
	unsigned int core;
	bool task_pp_en = 1;
	bool task_count = 1;

	//trace_printk("job %p job->next_task_idx %d job->task_count %d task->regcmd_count 0x%x\n", job, job->next_task_idx, job->task_count, job->tasks[job->next_task_idx].regcmd_count);

	/* GO ! */

	core = rocket_enqueue_job(rdev, job);
	/* Don't queue the job if a reset is in progress */
	if (!atomic_read(&rdev->reset.pending)) {

		task = &job->tasks[job->next_task_idx];
		job->next_task_idx++;   /* TODO: Do this only after a succesful run? */

		rocket_write(rdev, 0, RKNN_OFFSET_PC_DATA_ADDR, 0x1);

		rocket_write(rdev, 0, 0x1004, 0xe);
		rocket_write(rdev, 0, 0x3004, 0xe);

		rocket_write(rdev, 0, RKNN_OFFSET_PC_DATA_ADDR, task->regcmd);
		rocket_write(rdev, 0, RKNN_OFFSET_PC_DATA_AMOUNT, (task->regcmd_count + 1) / 2 - 1);

		rocket_write(rdev, 0, RKNN_OFFSET_INT_MASK, (1 << RKNN_DPU_GROUP_0) | (1 << RKNN_DPU_GROUP_1));
		rocket_write(rdev, 0, RKNN_OFFSET_INT_CLEAR, (1 << RKNN_DPU_GROUP_0) | (1 << RKNN_DPU_GROUP_1));

		rocket_write(rdev, 0, RKNN_OFFSET_PC_TASK_CONTROL, ((0x6 | task_pp_en) << 12) | task_count);

		rocket_write(rdev, 0, RKNN_OFFSET_PC_DMA_BASE_ADDR, 0x0);

		rocket_write(rdev, 0, RKNN_OFFSET_PC_OP_EN, 0x1);

		//trace_printk("Submitted job\n");

		dev_dbg(rdev->dev,
			"Submitted regcmd at 0x%llx to core %d",
			task->regcmd, core);
	}
}

static int rocket_acquire_object_fences(struct drm_gem_object **bos,
					int bo_count,
					struct drm_sched_job *job,
					bool is_write)
{
	int i, ret;

	for (i = 0; i < bo_count; i++) {
		ret = dma_resv_reserve_fences(bos[i]->resv, 1);
		if (ret)
			return ret;

		ret = drm_sched_job_add_implicit_dependencies(job, bos[i],
							      is_write);
		if (ret)
			return ret;
	}

	return 0;
}

static void rocket_attach_object_fences(struct drm_gem_object **bos,
					  int bo_count,
					  struct dma_fence *fence)
{
	int i;

	for (i = 0; i < bo_count; i++)
		dma_resv_add_fence(bos[i]->resv, fence, DMA_RESV_USAGE_WRITE);
}

static int rocket_job_push(struct rocket_job *job)
{
	struct rocket_device *rdev = job->rdev;
	struct ww_acquire_ctx acquire_ctx;
	int ret = 0;

	ret = drm_gem_lock_reservations(job->in_bos, job->in_bo_count,
					    &acquire_ctx);
	if (ret)
		return ret;

	ret = drm_gem_lock_reservations(job->out_bos, job->out_bo_count,
					    &acquire_ctx);
	if (ret)
		goto unlock_in;

	mutex_lock(&rdev->sched_lock);
	drm_sched_job_arm(&job->base);

	job->inference_done_fence = dma_fence_get(&job->base.s_fence->finished);

	ret = rocket_acquire_object_fences(job->in_bos, job->in_bo_count, &job->base, false);
	if (ret) {
		mutex_unlock(&rdev->sched_lock);
		goto unlock_out;
	}

	ret = rocket_acquire_object_fences(job->out_bos, job->out_bo_count, &job->base, true);
	if (ret) {
		mutex_unlock(&rdev->sched_lock);
		goto unlock_out;
	}

	kref_get(&job->refcount); /* put by scheduler job completion */

	drm_sched_entity_push_job(&job->base);

	mutex_unlock(&rdev->sched_lock);

	rocket_attach_object_fences(job->out_bos, job->out_bo_count, job->inference_done_fence);

unlock_out:
	drm_gem_unlock_reservations(job->out_bos, job->out_bo_count, &acquire_ctx);
unlock_in:
	drm_gem_unlock_reservations(job->in_bos, job->in_bo_count, &acquire_ctx);

	return ret;
}

static void rocket_job_cleanup(struct kref *ref)
{
	struct rocket_job *job = container_of(ref, struct rocket_job,
						refcount);
	unsigned int i;

	dma_fence_put(job->done_fence);
	dma_fence_put(job->inference_done_fence);

	if (job->in_bos) {
		for (i = 0; i < job->in_bo_count; i++)
			drm_gem_object_put(job->in_bos[i]);

		kvfree(job->in_bos);
	}

	if (job->out_bos) {
		for (i = 0; i < job->out_bo_count; i++)
			drm_gem_object_put(job->out_bos[i]);

		kvfree(job->out_bos);
	}

	kfree(job->tasks);

	kfree(job);
}

static void rocket_job_put(struct rocket_job *job)
{
	kref_put(&job->refcount, rocket_job_cleanup);
}

static void rocket_job_free(struct drm_sched_job *sched_job)
{
	struct rocket_job *job = to_rocket_job(sched_job);

	drm_sched_job_cleanup(sched_job);

	rocket_job_put(job);
}

static struct dma_fence *rocket_job_run(struct drm_sched_job *sched_job)
{
	struct rocket_job *job = to_rocket_job(sched_job);
	struct rocket_device *rdev = job->rdev;
	struct dma_fence *fence = NULL;
	int ret;

	//trace_printk("%d job->next_task_idx %d job->task_count %d\n", 1, job->next_task_idx, job->task_count);

	if (unlikely(job->base.s_fence->finished.error))
		return NULL;

	/* Nothing to execute: can happen if the job has finished while
	 * we were resetting the GPU.
	 */
	if (job->next_task_idx == job->task_count)
		return NULL;

	fence = rocket_fence_create(rdev);
	if (IS_ERR(fence))
		return fence;

	if (job->done_fence)
		dma_fence_put(job->done_fence);
	job->done_fence = dma_fence_get(fence);

	ret = pm_runtime_get_sync(rdev->dev);
	if (ret < 0)
		return fence;

	spin_lock(&rdev->job_lock);

	rocket_job_hw_submit(job);

	spin_unlock(&rdev->job_lock);

	return fence;
}

void rocket_job_enable_interrupts(struct rocket_device *rdev)
{
	int j;
	u32 irq_mask = 0;

#if 0
	clear_bit(ROCKET_COMP_BIT_JOB, rdev->is_suspended);

	for (j = 0; j < NUM_JOB_SLOTS; j++) {
		irq_mask |= MK_JS_MASK(j);
	}

	job_write(rdev, JOB_INT_CLEAR, irq_mask);
	job_write(rdev, JOB_INT_MASK, irq_mask);
#endif
}

void rocket_job_suspend_irq(struct rocket_device *rdev)
{
	int i;
#if 0
	set_bit(ROCKET_COMP_BIT_JOB, rdev->is_suspended);

	job_write(rdev, JOB_INT_MASK, 0);
#endif
	for (i = 0; i < rdev->comp->num_irqs; i++)
		synchronize_irq(rdev->irq[i]);
}

static void rocket_job_handle_err(struct rocket_device *rdev,
				    struct rocket_job *job,
				    unsigned int js)
{
#if 0
	u32 js_status = job_read(rdev, JS_STATUS(js));
	const char *exception_name = rocket_exception_name(js_status);
	bool signal_fence = true;

	if (!rocket_exception_is_fault(js_status)) {
		dev_dbg(rdev->dev, "js event, js=%d, status=%s, head=0x%x, tail=0x%x",
			js, exception_name,
			job_read(rdev, JS_HEAD_LO(js)),
			job_read(rdev, JS_TAIL_LO(js)));
	} else {
		dev_err(rdev->dev, "js fault, js=%d, status=%s, head=0x%x, tail=0x%x",
			js, exception_name,
			job_read(rdev, JS_HEAD_LO(js)),
			job_read(rdev, JS_TAIL_LO(js)));
	}

	if (js_status == DRM_rocket_EXCEPTION_STOPPED) {
		/* Update the job head so we can resume */
		job->jc = job_read(rdev, JS_TAIL_LO(js)) |
			  ((u64)job_read(rdev, JS_TAIL_HI(js)) << 32);

		/* The job will be resumed, don't signal the fence */
		signal_fence = false;
	} else if (js_status == DRM_rocket_EXCEPTION_TERMINATED) {
		/* Job has been hard-stopped, flag it as canceled */
		dma_fence_set_error(job->done_fence, -ECANCELED);
		job->jc = 0;
	} else if (rocket_exception_is_fault(js_status)) {
		/* We might want to provide finer-grained error code based on
		 * the exception type, but unconditionally setting to EINVAL
		 * is good enough for now.
		 */
		dma_fence_set_error(job->done_fence, -EINVAL);
		job->jc = 0;
	}

	rocket_mmu_as_put(rdev, job->mmu);
	rocket_devfreq_record_idle(&rdev->rdevfreq);

	if (signal_fence)
		dma_fence_signal_locked(job->done_fence);

	pm_runtime_put_autosuspend(rdev->dev);

	if (rocket_exception_needs_reset(rdev, js_status)) {
		atomic_set(&rdev->reset.pending, 1);
		drm_sched_fault(&rdev->js->queue[js].sched);
	}
#endif
}

static void rocket_job_handle_done(struct rocket_device *rdev,
				   struct rocket_job *job)
{
	//trace_printk("job %p job->next_task_idx %d job->task_count %d\n", job, job->next_task_idx, job->task_count);
	if (job->next_task_idx < job->task_count) {
		rocket_job_hw_submit(job);
		return;
	}

	//rocket_devfreq_record_idle(&rdev->rdevfreq);

	dma_fence_signal_locked(job->done_fence);
	pm_runtime_put_autosuspend(rdev->dev);
}

static void rocket_job_handle_irq(struct rocket_device *rdev)
{
	uint32_t status, raw_status;
	unsigned int core = 0;
	struct rocket_job *job;

	pm_runtime_mark_last_busy(rdev->dev);

	status = rocket_read(rdev, 0, RKNN_OFFSET_INT_STATUS);
	raw_status = rocket_read(rdev, 0, RKNN_OFFSET_INT_RAW_STATUS);

	rocket_write(rdev, 0, RKNN_OFFSET_PC_OP_EN, 0x0);
	rocket_write(rdev, 0, RKNN_OFFSET_INT_CLEAR, 0x1ffff);

	spin_lock(&rdev->job_lock);

	job = rocket_dequeue_job(rdev, core);
	rocket_job_handle_done(rdev, job);

	spin_unlock(&rdev->job_lock);
}

static u32 rocket_active_slots(struct rocket_device *rdev,
				 u32 *js_state_mask, u32 js_state)
{
#if 0
	u32 rawstat;

	if (!(js_state & *js_state_mask))
		return 0;

	rawstat = job_read(rdev, JOB_INT_RAWSTAT);
	if (rawstat) {
		unsigned int i;

		for (i = 0; i < NUM_JOB_SLOTS; i++) {
			if (rawstat & MK_JS_MASK(i))
				*js_state_mask &= ~MK_JS_MASK(i);
		}
	}

	return js_state & *js_state_mask;
#else
	return 0;
#endif
}

static void
rocket_reset(struct rocket_device *rdev,
	       struct drm_sched_job *bad)
{
	u32 js_state, js_state_mask = 0xffffffff;
	unsigned int i, j;
	bool cookie;
	int ret;

	if (!atomic_read(&rdev->reset.pending))
		return;

	/* Stop the scheduler.
	 *
	 * FIXME: We temporarily get out of the dma_fence_signalling section
	 * because the cleanup path generate lockdep splats when taking locks
	 * to release job resources. We should rework the code to follow this
	 * pattern:
	 *
	 *	try_lock
	 *	if (locked)
	 *		release
	 *	else
	 *		schedule_work_to_release_later
	 */
	drm_sched_stop(&rdev->queue.sched, bad);

	cookie = dma_fence_begin_signalling();

	if (bad)
		drm_sched_increase_karma(bad);

#if 0
	/* Mask job interrupts and synchronize to make sure we won't be
	 * interrupted during our reset.
	 */
	job_write(rdev, JOB_INT_MASK, 0);
	synchronize_irq(rdev->irq);

	for (i = 0; i < NUM_JOB_SLOTS; i++) {
		/* Cancel the next job and soft-stop the running job. */
		job_write(rdev, JS_COMMAND_NEXT(i), JS_COMMAND_NOP);
		job_write(rdev, JS_COMMAND(i), JS_COMMAND_SOFT_STOP);
	}

	/* Wait at most 10ms for soft-stops to complete */
	ret = readl_poll_timeout(rdev->iomem + JOB_INT_JS_STATE, js_state,
				 !rocket_active_slots(rdev, &js_state_mask, js_state),
				 10, 10000);
#endif

	if (ret)
		dev_err(rdev->dev, "Soft-stop failed\n");

	/* Handle the remaining interrupts before we reset. */
	rocket_job_handle_irq(rdev);

	/* Remaining interrupts have been handled, but we might still have
	 * stuck jobs. Let's make sure the PM counters stay balanced by
	 * manually calling pm_runtime_put_noidle() and
	 * rocket_devfreq_record_idle() for each stuck job.
	 * Let's also make sure the cycle counting register's refcnt is
	 * kept balanced to prevent it from running forever
	 */
	spin_lock(&rdev->job_lock);
	for (i = 0; i < ARRAY_SIZE(rdev->jobs) && rdev->jobs[i]; i++) {
		pm_runtime_put_noidle(rdev->dev);
		//rocket_devfreq_record_idle(&rdev->rdevfreq);
	}
	memset(rdev->jobs, 0, sizeof(rdev->jobs));
	spin_unlock(&rdev->job_lock);

	/* Proceed with reset now. */
	rocket_device_reset(rdev);

	/* rocket_device_reset() unmasks job interrupts, but we want to
	 * keep them masked a bit longer.
	 */
	//job_write(rdev, JOB_INT_MASK, 0);

	/* GPU has been reset, we can clear the reset pending bit. */
	atomic_set(&rdev->reset.pending, 0);

	/* Now resubmit jobs that were previously queued but didn't have a
	 * chance to finish.
	 * FIXME: We temporarily get out of the DMA fence signalling section
	 * while resubmitting jobs because the job submission logic will
	 * allocate memory with the GFP_KERNEL flag which can trigger memory
	 * reclaim and exposes a lock ordering issue.
	 */
	dma_fence_end_signalling(cookie);
	drm_sched_resubmit_jobs(&rdev->queue.sched);
	cookie = dma_fence_begin_signalling();

	/* Restart the scheduler */
	drm_sched_start(&rdev->queue.sched, true);

	/* Re-enable job interrupts now that everything has been restarted. */
	#if 0
	job_write(rdev, JOB_INT_MASK,
		  GENMASK(16 + NUM_JOB_SLOTS - 1, 16) |
		  GENMASK(NUM_JOB_SLOTS - 1, 0));
	#endif
	dma_fence_end_signalling(cookie);
}

static enum drm_gpu_sched_stat rocket_job_timedout(struct drm_sched_job
						     *sched_job)
{
	struct rocket_job *job = to_rocket_job(sched_job);
	struct rocket_device *rdev = job->rdev;
	int i;

	//trace_printk("%d\n", 1);

	/*
	 * If the GPU managed to complete this jobs fence, the timeout is
	 * spurious. Bail out.
	 */
	if (dma_fence_is_signaled(job->done_fence))
		return DRM_GPU_SCHED_STAT_NOMINAL;

	/*
	 * rocket IRQ handler may take a long time to process an interrupt
	 * if there is another IRQ handler hogging the processing.
	 * For example, the HDMI encoder driver might be stuck in the IRQ
	 * handler for a significant time in a case of bad cable connection.
	 * In order to catch such cases and not report spurious rocket
	 * job timeouts, synchronize the IRQ handler and re-check the fence
	 * status.
	 */
	for (i = 0; i < rdev->comp->num_irqs; i++)
		synchronize_irq(rdev->irq[i]);

	if (dma_fence_is_signaled(job->done_fence)) {
		dev_warn(rdev->dev, "unexpectedly high interrupt latency\n");
		return DRM_GPU_SCHED_STAT_NOMINAL;
	}

#if 0
	dev_err(rdev->dev, "gpu sched timeout, js=%d, config=0x%x, status=0x%x, head=0x%x, tail=0x%x, sched_job=%p",
		js,
		job_read(rdev, JS_CONFIG(js)),
		job_read(rdev, JS_STATUS(js)),
		job_read(rdev, JS_HEAD_LO(js)),
		job_read(rdev, JS_TAIL_LO(js)),
		sched_job);
#endif

	atomic_set(&rdev->reset.pending, 1);
	rocket_reset(rdev, sched_job);

	return DRM_GPU_SCHED_STAT_NOMINAL;
}

static void rocket_reset_work(struct work_struct *work)
{
	struct rocket_device *rdev;

	rdev = container_of(work, struct rocket_device, reset.work);
	rocket_reset(rdev, NULL);
}

static const struct drm_sched_backend_ops rocket_sched_ops = {
	.run_job = rocket_job_run,
	.timedout_job = rocket_job_timedout,
	.free_job = rocket_job_free
};

static irqreturn_t rocket_job_irq_handler_thread(int irq, void *data)
{
	struct rocket_device *rdev = data;

	rocket_job_handle_irq(rdev);

	return IRQ_HANDLED;
}

static irqreturn_t rocket_job_irq_handler(int irq, void *data)
{
	struct rocket_device *rdev = data;
	uint32_t status, raw_status;

	status = rocket_read(rdev, 0, RKNN_OFFSET_INT_STATUS);
	raw_status = rocket_read(rdev, 0, RKNN_OFFSET_INT_RAW_STATUS);

	//trace_printk("Finished job %x %x\n", status, raw_status);

	rocket_write(rdev, 0, RKNN_OFFSET_INT_MASK, 0x0);

	return IRQ_WAKE_THREAD;
}

int rocket_job_init(struct rocket_device *rdev)
{
	int ret, i;

	INIT_WORK(&rdev->reset.work, rocket_reset_work);
	spin_lock_init(&rdev->job_lock);

	for (i = 0; i < rdev->comp->num_irqs; i++) {
		rdev->irq[i] = platform_get_irq_byname(to_platform_device(rdev->dev),
						       rdev->comp->irq_names[i]);
		if (rdev->irq[i] < 0)
			return rdev->irq[i];
	}

	for (i = 0; i < rdev->comp->num_irqs; i++) {
		ret = devm_request_threaded_irq(rdev->dev, rdev->irq[i],
						rocket_job_irq_handler,
						rocket_job_irq_handler_thread,
						IRQF_SHARED, KBUILD_MODNAME "-job",
						rdev);
		if (ret) {
			dev_err(rdev->dev, "failed to request job irq");
			return ret;
		}
	}

	rdev->reset.wq = alloc_ordered_workqueue("rocket-reset", 0);
	if (!rdev->reset.wq)
		return -ENOMEM;

	rdev->queue.fence_context = dma_fence_context_alloc(1);

	ret = drm_sched_init(&rdev->queue.sched,
				&rocket_sched_ops, NULL,
				DRM_SCHED_PRIORITY_COUNT,
				1, 0,                    /* TODO: Schedule to the other cores */
				msecs_to_jiffies(JOB_TIMEOUT_MS),
				rdev->reset.wq,
				NULL, "rocket", rdev->dev);
	if (ret) {
		dev_err(rdev->dev, "Failed to create scheduler: %d.", ret);
		goto err_sched;
	}

	rocket_job_enable_interrupts(rdev);

	return 0;

err_sched:
	drm_sched_fini(&rdev->queue.sched);

	destroy_workqueue(rdev->reset.wq);
	return ret;
}

void rocket_job_fini(struct rocket_device *rdev)
{
	int j;

	//job_write(rdev, JOB_INT_MASK, 0);

	drm_sched_fini(&rdev->queue.sched);

	cancel_work_sync(&rdev->reset.work);
	destroy_workqueue(rdev->reset.wq);
}

int rocket_job_open(struct rocket_file_priv *rocket_priv)
{
	struct rocket_device *rdev = rocket_priv->rdev;
	struct drm_gpu_scheduler *sched;
	int ret;

	sched = &rdev->queue.sched;
	ret = drm_sched_entity_init(&rocket_priv->sched_entity,
				    DRM_SCHED_PRIORITY_NORMAL, &sched,
				    1, NULL);
	if (WARN_ON(ret))
		return ret;

	return 0;
}

void rocket_job_close(struct rocket_file_priv *rocket_priv)
{
	struct rocket_device *rdev = rocket_priv->rdev;
	struct drm_sched_entity *entity = &rocket_priv->sched_entity;
	int i;

	drm_sched_entity_destroy(entity);

	/* Kill in-flight jobs */
	spin_lock(&rdev->job_lock);

	for (i = ARRAY_SIZE(rdev->jobs) - 1; i >= 0; i--) {
		struct rocket_job *job = rdev->jobs[i];

		if (!job || job->base.entity != entity)
			continue;

#if 0
		job_write(rdev, JS_COMMAND(i), JS_COMMAND_HARD_STOP);

		/* Jobs can outlive their file context */
		job->engine_usage = NULL;
#endif
	}
	spin_unlock(&rdev->job_lock);
}

int rocket_job_is_idle(struct rocket_device *rdev)
{
	/* If there are any jobs in the HW queue, we're not idle */
	if (atomic_read(&rdev->queue.sched.credit_count))
		return false;

	return true;
}

static int rocket_ioctl_submit_job(struct drm_device *dev, struct drm_file *file, struct drm_rocket_job *job)
{
	struct rocket_device *rdev = dev->dev_private;
	struct rocket_file_priv *file_priv = file->driver_priv;
	struct rocket_job *rjob = NULL;
	int ret = 0;

	//trace_printk("%d\n", 1);

	if (job->task_count == 0)
		return -EINVAL;

	rjob = kzalloc(sizeof(*rjob), GFP_KERNEL);
	if (!rjob)
		return -ENOMEM;

	kref_init(&rjob->refcount);

	rjob->rdev = rdev;

	ret = drm_sched_job_init(&rjob->base,
				 &file_priv->sched_entity,
				 1, NULL);
	if (ret)
		goto out_put_job;

	ret = rocket_copy_tasks(dev, file, job, rjob);
	if (ret)
		goto out_cleanup_job;

	ret = drm_gem_objects_lookup(file,
				     (void __user *)(uintptr_t)job->in_bo_handles,
				     job->in_bo_handle_count, &rjob->in_bos);
	if (ret)
		goto out_cleanup_job;

	rjob->in_bo_count = job->in_bo_handle_count;

	ret = drm_gem_objects_lookup(file,
				     (void __user *)(uintptr_t)job->out_bo_handles,
				     job->out_bo_handle_count, &rjob->out_bos);
	if (ret)
		goto out_cleanup_job;

	rjob->out_bo_count = job->out_bo_handle_count;

	ret = rocket_job_push(rjob);
	if (ret)
		goto out_cleanup_job;

out_cleanup_job:
	if (ret)
		drm_sched_job_cleanup(&rjob->base);
out_put_job:
	rocket_job_put(rjob);

	return ret;
}

int rocket_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_rocket_submit *args = data;
	struct drm_rocket_job *jobs;
	int ret = 0;
	unsigned int i = 0;

	//trace_printk("%d\n", 1);

	jobs = kvmalloc_array(args->job_count, sizeof(*jobs), GFP_KERNEL);
	if (!jobs) {
		DRM_DEBUG("Failed to allocate incoming job array\n");
		return -ENOMEM;
	}

	if (copy_from_user(jobs,
			   (void __user *)(uintptr_t)args->jobs,
			   args->job_count * sizeof(*jobs))) {
		ret = -EFAULT;
		DRM_DEBUG("Failed to copy incoming job array\n");
		goto exit;
	}

	for (i = 0; i < args->job_count; i++)
		rocket_ioctl_submit_job(dev, file, &jobs[i]);

exit:
	kfree(jobs);

	return ret;
}
