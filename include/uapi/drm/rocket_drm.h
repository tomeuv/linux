/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2014-2018 Broadcom
 * Copyright © 2019 Collabora ltd.
 * Copyright © 2024 Tomeu Vizoso
 */
#ifndef _ROCKET_DRM_H_
#define _ROCKET_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_ROCKET_SUBMIT			0x00
#define DRM_ROCKET_CREATE_BO			0x01
#define DRM_ROCKET_MMAP_BO			0x02
#define DRM_ROCKET_GET_HW_VERSION		0x03

#define DRM_IOCTL_ROCKET_SUBMIT			DRM_IOW(DRM_COMMAND_BASE + DRM_ROCKET_SUBMIT, struct drm_rocket_submit)
#define DRM_IOCTL_ROCKET_CREATE_BO		DRM_IOWR(DRM_COMMAND_BASE + DRM_ROCKET_CREATE_BO, struct drm_rocket_create_bo)
#define DRM_IOCTL_ROCKET_MMAP_BO		DRM_IOWR(DRM_COMMAND_BASE + DRM_ROCKET_MMAP_BO, struct drm_rocket_mmap_bo)
#define DRM_IOCTL_ROCKET_GET_HW_VERSION		DRM_IOWR(DRM_COMMAND_BASE + DRM_ROCKET_GET_HW_VERSION, struct drm_rocket_get_hw_version)

/**
 * struct drm_rocket_submit - ioctl argument for submitting commands to the NPU.
 *
 * This asks the kernel to have the NPU execute a register command list.
 */
struct drm_rocket_submit {

	/** DMA address to NPU mapping of register command buffer */
	__u64 regcmd;

	/** An optional array of sync objects to wait on before starting this job. */
	__u64 in_syncs;

	/** Number of sync objects to wait on before starting this job. */
	__u32 in_sync_count;

	/** An optional sync object to place the completion fence in. */
	__u32 out_sync;

	/** Pointer to a u32 array of the BOs that are referenced by the job. */
	__u64 bo_handles;

	/** Number of BO handles passed in (size is that times 4). */
	__u32 bo_handle_count;
};

/* Valid flags to pass to drm_rocket_create_bo */
#define ROCKET_BO_NOEXEC	1
#define ROCKET_BO_HEAP	2

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
	/* Pad, must be zero-filled. */
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
};

/**
 * struct drm_rocket_mmap_bo - ioctl argument for mapping Rocket BOs.
 *
 * This doesn't actually perform an mmap.  Instead, it returns the
 * offset you need to use in an mmap on the DRM device node.  This
 * means that tools like valgrind end up knowing about the mapped
 * memory.
 *
 * There are currently no values for the flags argument, but it may be
 * used in a future extension.
 */
struct drm_rocket_mmap_bo {
	/** Handle for the object being mapped. */
	__u32 handle;
	__u32 flags;
	/** offset into the drm node to use for subsequent mmap call. */
	__u64 offset;
};

struct drm_rocket_get_hw_version {
	__u32 value;
};

#if defined(__cplusplus)
}
#endif

#endif /* _ROCKET_DRM_H_ */
