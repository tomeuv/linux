// SPDX-License-Identifier: GPL-2.0
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <drm/rocket_drm.h>

#include "rocket_drv.h"
#include "rocket_device.h"
#include "rocket_gem.h"

/**
 * rocket_lookup_bos() - Sets up job->bo[] with the GEM objects
 * referenced by the job.
 * @dev: DRM device
 * @file_priv: DRM file for this fd
 * @args: IOCTL args
 * @job: job being set up
 *
 * Resolve handles from userspace to BOs and attach them to job.
 *
 * Note that this function doesn't need to unreference the BOs on
 * failure, because that will happen at rocket_job_cleanup() time.
 */
static int
rocket_lookup_bos(struct drm_device *dev,
		  struct drm_file *file_priv,
		  struct drm_rocket_submit *args,
		  struct rocket_job *job)
{
	struct rocket_file_priv *priv = file_priv->driver_priv;
	struct rocket_gem_object *bo;
	unsigned int i;
	int ret;

	job->bo_count = args->bo_handle_count;

	if (!job->bo_count)
		return 0;

	ret = drm_gem_objects_lookup(file_priv,
				     (void __user *)(uintptr_t)args->bo_handles,
				     job->bo_count, &job->bos);
	if (ret)
		return ret;

	job->mappings = kvmalloc_array(job->bo_count,
				       sizeof(struct rocket_gem_mapping *),
				       GFP_KERNEL | __GFP_ZERO);
	if (!job->mappings)
		return -ENOMEM;

	for (i = 0; i < job->bo_count; i++) {
		struct rocket_gem_mapping *mapping;

		bo = to_rocket_bo(job->bos[i]);
		mapping = rocket_gem_mapping_get(bo, priv);
		if (!mapping) {
			ret = -EINVAL;
			break;
		}

		atomic_inc(&bo->gpu_usecount);
		job->mappings[i] = mapping;
	}

	return ret;
}

/**
 * rocket_copy_in_sync() - Sets up job->deps with the sync objects
 * referenced by the job.
 * @dev: DRM device
 * @file_priv: DRM file for this fd
 * @args: IOCTL args
 * @job: job being set up
 *
 * Resolve syncobjs from userspace to fences and attach them to job.
 *
 * Note that this function doesn't need to unreference the fences on
 * failure, because that will happen at rocket_job_cleanup() time.
 */
static int
rocket_copy_in_sync(struct drm_device *dev,
		  struct drm_file *file_priv,
		  struct drm_rocket_submit *args,
		  struct rocket_job *job)
{
	u32 *handles;
	int ret = 0;
	int i, in_fence_count;

	in_fence_count = args->in_sync_count;

	if (!in_fence_count)
		return 0;

	handles = kvmalloc_array(in_fence_count, sizeof(u32), GFP_KERNEL);
	if (!handles) {
		ret = -ENOMEM;
		DRM_DEBUG("Failed to allocate incoming syncobj handles\n");
		goto fail;
	}

	if (copy_from_user(handles,
			   (void __user *)(uintptr_t)args->in_syncs,
			   in_fence_count * sizeof(u32))) {
		ret = -EFAULT;
		DRM_DEBUG("Failed to copy in syncobj handles\n");
		goto fail;
	}

	for (i = 0; i < in_fence_count; i++) {
		ret = drm_sched_job_add_syncobj_dependency(&job->base, file_priv,
							   handles[i], 0);
		if (ret)
			goto fail;
	}

fail:
	kvfree(handles);
	return ret;
}

static int rocket_ioctl_submit(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct rocket_device *rdev = dev->dev_private;
	struct rocket_file_priv *file_priv = file->driver_priv;
	struct drm_rocket_submit *args = data;
	struct drm_syncobj *sync_out = NULL;
	struct rocket_job *job;
	int ret = 0, slot;

	if (!args->jc)
		return -EINVAL;

	if (args->requirements && args->requirements != rocket_JD_REQ_FS)
		return -EINVAL;

	if (args->out_sync > 0) {
		sync_out = drm_syncobj_find(file, args->out_sync);
		if (!sync_out)
			return -ENODEV;
	}

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto out_put_syncout;
	}

	kref_init(&job->refcount);

	job->rdev = rdev;
	job->jc = args->jc;
	job->requirements = args->requirements;
	job->flush_id = rocket_gpu_get_latest_flush_id(rdev);
	job->mmu = file_priv->mmu;
	job->engine_usage = &file_priv->engine_usage;

	slot = rocket_job_get_slot(job);

	ret = drm_sched_job_init(&job->base,
				 &file_priv->sched_entity[slot],
				 1, NULL);
	if (ret)
		goto out_put_job;

	ret = rocket_copy_in_sync(dev, file, args, job);
	if (ret)
		goto out_cleanup_job;

	ret = rocket_lookup_bos(dev, file, args, job);
	if (ret)
		goto out_cleanup_job;

	ret = rocket_job_push(job);
	if (ret)
		goto out_cleanup_job;

	/* Update the return sync object for the job */
	if (sync_out)
		drm_syncobj_replace_fence(sync_out, job->render_done_fence);

out_cleanup_job:
	if (ret)
		drm_sched_job_cleanup(&job->base);
out_put_job:
	rocket_job_put(job);
out_put_syncout:
	if (sync_out)
		drm_syncobj_put(sync_out);

	return ret;
}
