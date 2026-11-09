/* SPDX-License-Identifier: GPL-3.0-only */
/* Host pipeline fixture declarations only. Never in native include paths. */
#pragma once
#include <stdint.h>
#include <sys/types.h>
#include <pthread.h>
typedef pthread_t OrbisPthread;
typedef pthread_attr_t OrbisPthreadAttr;
uint64_t sceKernelGetProcessTime(void);
int sceKernelUsleep(unsigned);
int scePthreadCreate(OrbisPthread *, const pthread_attr_t *, void *(*)(void *), void *, const char *);
int scePthreadJoin(OrbisPthread, void **);

OrbisPthread scePthreadSelf(void);
int32_t scePthreadSetprio(OrbisPthread,int);
int32_t scePthreadGetprio(OrbisPthread,int32_t*);
int32_t scePthreadAttrInit(OrbisPthreadAttr*);
int32_t scePthreadAttrSetstacksize(OrbisPthreadAttr*,size_t);
int32_t scePthreadAttrGet(OrbisPthread,OrbisPthreadAttr*);
int32_t scePthreadAttrGetaffinity(const OrbisPthreadAttr*,void*);
int32_t scePthreadAttrDestroy(OrbisPthreadAttr*);
