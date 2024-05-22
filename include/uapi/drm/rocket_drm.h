/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Tomeu Vizoso
 */
#ifndef _ROCKET_DRM_H_
#define _ROCKET_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_ROCKET_CREATE_BO			0x00

#define DRM_IOCTL_ROCKET_CREATE_BO		DRM_IOWR(DRM_COMMAND_BASE + DRM_ROCKET_CREATE_BO, struct drm_rocket_create_bo)

/**
 * struct drm_rocket_create_bo - ioctl argument for creating Rocket BOs.
 *
 * The flags argument is a bit mask of ROCKET_BO_* flags.
 */
struct drm_rocket_create_bo {
	__u32 size;
	__u32 flags;

	/** Returned GEM handle for the BO. */
	__u32 handle;

	/** Pad, must be zero-filled. */
	__u32 pad;

	/**
	 * Returned DMA address for the BO in the NPU address space.  This address
	 * is private to the DRM fd and is valid for the lifetime of the GEM
	 * handle.
	 *
	 * This address value will always be nonzero, since various HW
	 * units treat 0 specially.
	 */
	__u64 dma_address;

	/** Offset into the drm node to use for subsequent mmap call. */
	__u64 offset;
};

#if defined(__cplusplus)
}
#endif

#endif /* _ROCKET_DRM_H_ */
