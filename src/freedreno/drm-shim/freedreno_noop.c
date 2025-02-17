/*
 * Copyright © 2019 Google LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include "drm-uapi/msm_drm.h"
#include "drm-shim/drm_shim.h"

struct msm_bo {
	struct shim_bo base;
	uint32_t offset;
};

static struct msm_bo *
msm_bo(struct shim_bo *bo)
{
	return (struct msm_bo *)bo;
}

struct msm_device {
	uint32_t next_offset;
};

static struct msm_device msm = {
	.next_offset = 0x1000,
};

static int
msm_ioctl_noop(int fd, unsigned long request, void *arg)
{
	return 0;
}

static int
msm_ioctl_gem_new(int fd, unsigned long request, void *arg)
{
	struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
	struct drm_msm_gem_new *create = arg;
	struct msm_bo *bo = calloc(1, sizeof(*bo));

	drm_shim_bo_init(&bo->base, create->size);

	assert(UINT_MAX - msm.next_offset > create->size);

	bo->offset = msm.next_offset;
	msm.next_offset += create->size;

	create->handle = drm_shim_bo_get_handle(shim_fd, &bo->base);

	drm_shim_bo_put(&bo->base);

	return 0;
}

static int
msm_ioctl_gem_info(int fd, unsigned long request, void *arg)
{
	struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
	struct drm_msm_gem_info *args = arg;
	struct shim_bo *bo = drm_shim_bo_lookup(shim_fd, args->handle);

	switch (args->info) {
	case MSM_INFO_GET_OFFSET:
		args->value = drm_shim_bo_get_mmap_offset(shim_fd, bo);
		return 0;
	case MSM_INFO_GET_IOVA:
		args->value = msm_bo(bo)->offset;
		return 0;
	case MSM_INFO_SET_NAME:
		return 0;
	default:
		fprintf(stderr, "Unknown DRM_IOCTL_MSM_GEM_INFO %d\n", args->info);
		return -1;
	}

	drm_shim_bo_put(bo);

	return 0;
}

static int
msm_ioctl_get_param(int fd, unsigned long request, void *arg)
{
	struct drm_msm_param *gp = arg;

	switch (gp->param) {
	case MSM_PARAM_GPU_ID:
		gp->value = 630;
		return 0;
	case MSM_PARAM_GMEM_SIZE:
		gp->value = 1024 * 1024;
		return 0;
	case MSM_PARAM_GMEM_BASE:
		gp->value = 0x100000;
		return 0;
	case MSM_PARAM_CHIP_ID:
		gp->value = (6 << 24) | (3 << 16) | (0 << 8) | (0xff << 0);
		return 0;
	case MSM_PARAM_NR_RINGS:
		gp->value = 1;
		return 0;
	case MSM_PARAM_MAX_FREQ:
		gp->value = 1000000;
		return 0;
	case MSM_PARAM_TIMESTAMP:
		gp->value = 0;
		return 0;
	case MSM_PARAM_PP_PGTABLE:
		gp->value = 1;
		return 0;
	case MSM_PARAM_FAULTS:
		gp->value = 0;
		return 0;
	default:
		fprintf(stderr, "Unknown DRM_IOCTL_MSM_GET_PARAM %d\n",
				gp->param);
		return -1;
	}
}

static int
msm_ioctl_gem_madvise(int fd, unsigned long request, void *arg)
{
	struct drm_msm_gem_madvise *args = arg;

	args->retained = true;

	return 0;
}

static ioctl_fn_t driver_ioctls[] = {
	[DRM_MSM_GET_PARAM] = msm_ioctl_get_param,
	[DRM_MSM_GEM_NEW] = msm_ioctl_gem_new,
	[DRM_MSM_GEM_INFO] = msm_ioctl_gem_info,
	[DRM_MSM_GEM_CPU_PREP] = msm_ioctl_noop,
	[DRM_MSM_GEM_CPU_FINI] = msm_ioctl_noop,
	[DRM_MSM_GEM_SUBMIT] = msm_ioctl_noop,
	[DRM_MSM_WAIT_FENCE] = msm_ioctl_noop,
	[DRM_MSM_GEM_MADVISE] = msm_ioctl_gem_madvise,
	[DRM_MSM_SUBMITQUEUE_NEW] = msm_ioctl_noop,
	[DRM_MSM_SUBMITQUEUE_CLOSE] = msm_ioctl_noop,
	[DRM_MSM_SUBMITQUEUE_QUERY] = msm_ioctl_noop,
};

void
drm_shim_driver_init(void)
{
	shim_device.driver_name = "msm";
	shim_device.driver_ioctls = driver_ioctls;
	shim_device.driver_ioctl_count = ARRAY_SIZE(driver_ioctls);

	/* msm uses the DRM version to expose features, instead of getparam. */
	shim_device.version_major = 1;
	shim_device.version_minor = 5;
	shim_device.version_patchlevel = 0;

	drm_shim_override_file("OF_FULLNAME=/rdb/msm\n"
			"OF_COMPATIBLE_N=1\n"
			"OF_COMPATIBLE_0=qcom,adreno\n",
			"/sys/dev/char/%d:%d/device/uevent",
			DRM_MAJOR, render_node_minor);
}
