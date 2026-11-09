/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
int sceGnmAreSubmitsAllowed(void);
int sceGnmSubmitCommandBuffers(uint32_t,void **,uint32_t *,void **,uint32_t *);
int sceGnmSubmitDone(void);
