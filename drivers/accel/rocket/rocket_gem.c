// SPDX-License-Identifier: GPL-2.0
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include "drm/drm_utils.h"
#include <drm/drm_device.h>
#include <drm/rocket_drm.h>

#include "rocket_gem.h"

/**
 * rocket_gem_create_object - Implementation of driver->gem_create_object.
 * @dev: DRM device
 * @size: Size in bytes of the memory the object will reference
 *
 * This lets the GEM helpers allocate object structs for us, and keep
 * our BO stats correct.
 */
struct drm_gem_object *rocket_gem_create_object(struct drm_device *dev, size_t size)
{
	struct rocket_gem_object *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	return &obj->base.base;
}

int rocket_ioctl_create_bo(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_rocket_create_bo *args = data;
	struct drm_gem_dma_object *dma_obj;
	struct rocket_gem_object *rkt_obj;
	struct drm_gem_object *gem_obj;
	int ret;

	dma_obj = drm_gem_dma_create(dev, args->size);
	if (IS_ERR(dma_obj))
		return PTR_ERR(dma_obj);

	gem_obj = &dma_obj->base;
	rkt_obj = to_rocket_bo(gem_obj);

	rkt_obj->size = args->size;
	rkt_obj->offset = 0;
	mutex_init(&rkt_obj->mutex);

	ret = drm_gem_handle_create(file, gem_obj, &args->handle);
	drm_gem_object_put(gem_obj);
	if (ret) {
		drm_gem_dma_object_free(gem_obj);
		return ret;
	}

	args->offset = drm_vma_node_offset_addr(&gem_obj->vma_node);
	args->dma_address = dma_obj->dma_addr;

	return 0;
}

int rocket_ioctl_wait_bo(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
	long ret;
	struct drm_rocket_wait_bo *args = data;
	struct drm_gem_object *gem_obj;
	unsigned long timeout = drm_timeout_abs_to_jiffies(args->timeout_ns);

	if (args->pad)
		return -EINVAL;

	gem_obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!gem_obj)
		return -ENOENT;

	ret = dma_resv_wait_timeout(gem_obj->resv, DMA_RESV_USAGE_READ,
				    true, timeout);
	if (!ret)
		ret = timeout ? -ETIMEDOUT : -EBUSY;

	drm_gem_object_put(gem_obj);

	return ret;
}