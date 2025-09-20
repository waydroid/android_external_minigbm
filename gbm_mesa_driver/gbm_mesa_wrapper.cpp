/*
 * Copyright (C) 2021-2022 Roman Stratiienko (r.stratiienko@gmail.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "GBM-MESA-WRAPPER"

#include <gbm.h>

#include "../drv.h"
#include "gbm_mesa_wrapper.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <log/log.h>
#include <sys/mman.h>

#include <cutils/properties.h>

#include <sstream>
#include <vector>
#include <unordered_map>

#define ARRAY_SIZE(A) (sizeof(A) / sizeof(*(A)))
#define DRM_TO_GBM_FORMAT(A)                                                                       \
	{                                                                                          \
		DRM_##A, GBM_##A                                                                   \
	}

/* List of the formats that has mesa3d DRI_ equivalent, therefore can be allocated directly */
static const struct {
	uint32_t drm_format;
	uint32_t gbm_format;
} drm_to_gbm_image_formats[] = {
	DRM_TO_GBM_FORMAT(FORMAT_R8),
	DRM_TO_GBM_FORMAT(FORMAT_GR88),
	DRM_TO_GBM_FORMAT(FORMAT_ARGB1555),
	DRM_TO_GBM_FORMAT(FORMAT_RGB565),
	DRM_TO_GBM_FORMAT(FORMAT_XRGB8888),
	DRM_TO_GBM_FORMAT(FORMAT_ARGB8888),
	DRM_TO_GBM_FORMAT(FORMAT_XBGR8888),
	DRM_TO_GBM_FORMAT(FORMAT_ABGR8888),
	DRM_TO_GBM_FORMAT(FORMAT_XRGB2101010),
	DRM_TO_GBM_FORMAT(FORMAT_XBGR2101010),
	DRM_TO_GBM_FORMAT(FORMAT_ARGB2101010),
	DRM_TO_GBM_FORMAT(FORMAT_ABGR2101010),
	DRM_TO_GBM_FORMAT(FORMAT_XBGR16161616F),
	DRM_TO_GBM_FORMAT(FORMAT_ABGR16161616F),
	{DRM_FORMAT_YVU420_ANDROID, GBM_FORMAT_GR88},
};

static std::unordered_map<uint32_t, std::vector<uint64_t>> gbm_format_modifiers_map;

static std::vector<uint64_t> get_supported_modifiers(struct gbm_device *gbm, uint32_t format) {
	if (gbm_format_modifiers_map.find(format) != gbm_format_modifiers_map.end()) {
		return gbm_format_modifiers_map[format];
	}

	// Create empty default so we can match it next time
	std::vector<uint64_t> &modifiers = gbm_format_modifiers_map[format];

	std::stringstream prop_name_stream;
	prop_name_stream << "waydroid.modifiers." << std::hex << format << ".";
	std::string prop_name_base = prop_name_stream.str();
	char modifier_prop[PROPERTY_VALUE_MAX];
	int i = 0;
	while (true) {
		std::string prop_name = (std::stringstream() << prop_name_base << i).str();
		if (property_get(prop_name.c_str(), modifier_prop, NULL) < 1)
			break;
		std::stringstream ss(modifier_prop);
		uint64_t mod;
		ss >> std::hex >> mod;

		// Filter out multiplanar format-modifier combos
		if (gbm_device_get_format_modifier_plane_count(gbm, format, mod) < 2)
			modifiers.push_back(mod);
		i++;
	}

	return modifiers;
}

static uint32_t get_gbm_mesa_format(uint32_t drm_format)
{
	uint32_t i;
	for (i = 0; i < ARRAY_SIZE(drm_to_gbm_image_formats); i++) {
		if (drm_to_gbm_image_formats[i].drm_format == drm_format)
			return drm_to_gbm_image_formats[i].gbm_format;
	}

	return 0;
}

static struct gbm_device *gbm_mesa_dev_create(int fd)
{
	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm)
		ALOGE("Unable to create gbm device");

	return gbm;
}

static void gbm_mesa_dev_destroy(struct gbm_device *gbm)
{
	gbm_device_destroy(gbm);
}

// ALLOCATOR_ONLY!
static int gbm_mesa_alloc(struct alloc_args *args)
{
	struct gbm_bo *bo = NULL;

	uint32_t gbm_format = get_gbm_mesa_format(args->drm_format);
	int usage = 0;
	if (args->force_linear)
		usage |= GBM_BO_USE_LINEAR;

	if (args->use_scanout)
		usage |= GBM_BO_USE_SCANOUT;

	std::vector<uint64_t> modifiers = get_supported_modifiers(args->gbm, args->drm_format);
	if (modifiers.size() > 0) {
		int usage2 = usage;
		if (usage2 & GBM_BO_USE_LINEAR) {
			usage2 &= ~GBM_BO_USE_LINEAR;
			modifiers = {DRM_FORMAT_MOD_LINEAR};
		}
		bo = gbm_bo_create_with_modifiers2(args->gbm, args->width, args->height, gbm_format, modifiers.data(), modifiers.size(), usage2);
	}
	if (!bo) {
		ALOGV("fallback to gbm_bo_create without modifiers");
		bo = gbm_bo_create(args->gbm, args->width, args->height, gbm_format, usage);
	}

	if (!bo) {
		ALOGE("Unable to create BO, size=%dx%d, fmt=%d", args->width, args->height,
		      args->drm_format);
		return -EINVAL;
	}

	/* gbm_mesa will create new fd, therefore it's our responsibility to close it once we don't
	 * need the buffer */
	args->out_fd = gbm_bo_get_fd(bo);

	args->out_stride = gbm_bo_get_stride(bo);
	args->out_modifier = gbm_bo_get_modifier(bo);

	/* Buffer is now handled through the system via out_fd, we can now destroy gbm_mesa bo */
	gbm_bo_destroy(bo);

	if (args->needs_map_stride) {
		/* At least on Intel and nouveau map_stride after gbm_create is different from
		 * map_stride after gbm_import, We care only about map_stride after importing. */
		struct gbm_import_fd_modifier_data data = {
			.width = args->width,
			.height = args->height,
			.format = gbm_format,
			.num_fds = 1,
			.fds[0] = args->out_fd,
			.strides[0] = (int)args->out_stride,
			.modifier = args->out_modifier,
		};
		void *addr = NULL;
		void *map_data = NULL;

		bo = gbm_bo_import(args->gbm, GBM_BO_IMPORT_FD_MODIFIER, &data, 0);

		int flags = GBM_BO_TRANSFER_READ | GBM_BO_TRANSFER_WRITE;

		addr = gbm_bo_map(bo, 0, 0, args->width, args->height, flags, &args->out_map_stride,
				  &map_data);
		if (addr == NULL) {
			ALOGE("Failed to map the buffer at %s:%d", __FILE__, __LINE__);
		} else {
			gbm_bo_unmap(bo, map_data);
		}

		gbm_bo_destroy(bo);
	}

	return 0;
}

// MAPPER ONLY!
static struct gbm_bo *gbm_import(struct gbm_device *gbm, int buf_fd, uint32_t width,
				 uint32_t height, uint32_t stride, uint64_t modifier,
				 uint32_t drm_format)
{
	struct gbm_bo *bo = NULL;
	struct gbm_import_fd_modifier_data data = {
		.width = width,
		.height = height,
		.format = get_gbm_mesa_format(drm_format),
		.num_fds = 1,
		.fds[0] = buf_fd,
		.strides[0] = (int)stride,
		.modifier = modifier,
	};

	bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &data, 0);

	return bo;
}

static void gbm_free(struct gbm_bo *bo)
{
	gbm_bo_destroy(bo);
}

static void gbm_map(struct gbm_bo *bo, int w, int h, void **addr, void **map_data)
{
	int flags = GBM_BO_TRANSFER_READ | GBM_BO_TRANSFER_WRITE;

	uint32_t stride = 0;
	*addr = gbm_bo_map(bo, 0, 0, w, h, flags, &stride, map_data);
	if (*addr == NULL) {
		*addr = MAP_FAILED;
		ALOGE("Failed to map the buffer at %s:%d", __FILE__, __LINE__);
	}
}

static void gbm_unmap(struct gbm_bo *bo, void *map_data)
{
	gbm_bo_unmap(bo, map_data);
}

extern "C" {

struct gbm_ops gbm_ops = {
	.get_gbm_format = get_gbm_mesa_format,
	.dev_create = gbm_mesa_dev_create,
	.dev_destroy = gbm_mesa_dev_destroy,
	.alloc = gbm_mesa_alloc,
	.import = gbm_import,
	.free = gbm_free,
	.map = gbm_map,
	.unmap = gbm_unmap,
};

__attribute__((visibility("default"))) struct gbm_ops *get_gbm_ops()
{
	return &gbm_ops;
}

}
