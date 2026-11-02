/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#ifndef SGX_DRM_PUBLIC_H
#define SGX_DRM_PUBLIC_H

struct pipe_screen;
struct pipe_screen_config;
struct renderonly;

/* The render node of drivers/gpu/drm/apple-sgx ("apple_sgx") */
struct pipe_screen *sgx_drm_screen_create(int fd, const struct pipe_screen_config *config);

/* The same, with a display device's kmsro pairing: scanout resources are
 * allocated here and imported into the display through renderonly. */
struct pipe_screen *sgx_drm_screen_create_renderonly(int fd, struct renderonly *ro,
                                                     const struct pipe_screen_config *config);

#endif
