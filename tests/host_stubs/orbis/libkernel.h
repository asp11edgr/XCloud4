/* SPDX-License-Identifier: GPL-3.0-only */
/* Host pipeline fixture declarations only. Never in native include paths. */
#pragma once
#include <stdint.h>
#include <sys/types.h>
#include <pthread.h>
typedef pthread_t OrbisPthread;
uint64_t sceKernelGetProcessTime(void);
int sceKernelUsleep(unsigned);
int scePthreadCreate(OrbisPthread *, const pthread_attr_t *, void *(*)(void *), void *, const char *);
int scePthreadJoin(OrbisPthread, void **);
