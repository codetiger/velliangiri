/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#ifndef SGX_SCREEN_H
#define SGX_SCREEN_H

#include "pipe/p_screen.h"
#include "util/simple_mtx.h"
#include "util/slab.h"

#include "sgx_device.h"

struct renderonly;
struct sgx_frame;

struct sgx_screen {
   struct pipe_screen base;
   struct sgx_device dev;

   /* The display's side of a kmsro pairing: scanout resources are made as
    * dma-bufs here and imported there, and their KMS handles belong to the
    * display's fd (sgx_resource.c).  NULL when there is no display. */
   struct renderonly *ro;

   /* the template frame, for clears; NULL without one */
   struct sgx_frame *frame;
   simple_mtx_t frame_lock;

   /* the part's name, as many cores as the kernel found (this port's SoC
    * has one) */
   char name[24];

   struct slab_parent_pool transfer_pool;
};

static inline struct sgx_screen *
sgx_screen(struct pipe_screen *p)
{
   return (struct sgx_screen *)p;
}

struct pipe_screen *sgx_screen_create(int fd, const struct pipe_screen_config *config,
                                      struct renderonly *ro);

#endif
