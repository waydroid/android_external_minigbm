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

#pragma once

#include <stdbool.h>

#define MAX_PLANES 4

struct gbm_device;
struct gbm_bo;

struct alloc_args {
	struct gbm_device *gbm;
	uint32_t width;
	uint32_t height;
	uint32_t drm_format;
	bool use_scanout;
	bool force_linear;
	bool needs_map_stride;
	int out_fds[MAX_PLANES];
	uint32_t out_strides[MAX_PLANES];
	uint64_t out_modifier;
	uint32_t out_map_stride;
};

struct import_args {
	struct gbm_device *gbm;
	uint32_t width;
	uint32_t height;
	uint32_t drm_format;
	uint64_t modifier;
	size_t num_planes;
	const int *fds;
	const uint32_t *strides;
};

struct gbm_ops {
	uint32_t (*get_gbm_format)(uint32_t drm_format);

	struct gbm_device *(*dev_create)(int fd);

	void (*dev_destroy)(struct gbm_device *gbm);

	// ALLOCATOR ONLY
	int (*alloc)(struct alloc_args *args);

	// MAPPER ONLY
	struct gbm_bo *(*import)(struct import_args *args);

	void (*free)(struct gbm_bo *bo);

	void (*map)(struct gbm_bo *bo, int w, int h, void **addr, void **map_data);

	void (*unmap)(struct gbm_bo *bo, void *map_data);
};
