/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_t26_hooks.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "probe_t26_v1.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { X4_T26_PLATFORM_NOT_RUN=INT32_MIN, X4_T26_PLATFORM_CONTRACT=-7650 };
/* Independent LP64 ABI facts: installed SDK sys/resource.h and FreeBSD9
 * resource.h: two 16-byte timeval plus fourteen signed64 counters. */
typedef struct {int64_t seconds,microseconds;} X4T26Timeval;
typedef struct {X4T26Timeval user,system;int64_t counters[14];} X4T26Rusage;
_Static_assert(sizeof(X4T26Timeval)==16,"T26 timeval LP64");
_Static_assert(sizeof(X4T26Rusage)==144,"T26 rusage LP64");
_Static_assert(offsetof(X4T26Rusage,user)==0,"T26 ru_utime offset");
_Static_assert(offsetof(X4T26Rusage,system)==16,"T26 ru_stime offset");
_Static_assert(offsetof(X4T26Rusage,counters)==32,"T26 rusage counters offset");
typedef int32_t (*X4T26Getrusage)(int32_t,X4T26Rusage *);
typedef struct {
    int resolve_rc,process_rc,thread_rc;
    bool process_available,thread_available;
    uint64_t process_us,thread_us;
} X4T26CpuSnapshot;
typedef struct {
    bool process_available,thread_available;
    uint64_t process_us,thread_us,wall_us;
    double process_percent,thread_percent;
} X4T26CpuDelta;
bool x4_t26_rusage_cpu_us(const X4T26Rusage *,uint64_t *);
void x4_t26_cpu_snapshot_with(X4T26Getrusage,int resolve_rc,X4T26CpuSnapshot *);
void x4_t26_cpu_delta(const X4T26CpuSnapshot *,const X4T26CpuSnapshot *,uint64_t wall_us,X4T26CpuDelta *);

/* DirectMemoryQuery(offset,flags,info,size), pinned shadPS4 memory.h ABI.
 * No SDK AvailableDirectMemorySize declaration is used: its last two shipped
 * parameters are incorrectly declared by value instead of output pointers. */
typedef struct {int64_t start,end;int32_t memory_type;uint32_t reserved;} X4T26DirectQuery;
_Static_assert(sizeof(X4T26DirectQuery)==24,"T26 direct query size");
_Static_assert(offsetof(X4T26DirectQuery,start)==0,"T26 direct query start");
_Static_assert(offsetof(X4T26DirectQuery,end)==8,"T26 direct query end");
_Static_assert(offsetof(X4T26DirectQuery,memory_type)==16,"T26 direct query type");
bool x4_t26_query_contains(const X4T26DirectQuery *,const X4T26Allocation *);
bool x4_t26_query_release_evidence(bool before_valid,int after_rc);
typedef struct {
    unsigned module_id;
    bool before_called,load_called,after_called;
    int before_rc,load_rc,after_rc;
} X4T26ModuleObservation;
typedef struct {
    int64_t offset;uint64_t bytes;int memory_type;
    int allocate_rc,map_rc,unmap_rc,release_rc;
} X4T26MemoryObservation;
typedef struct {
    int kernel_handle,sysmodule_handle,decoder_handle;
    int usage_resolve_rc,query_resolve_rc,available_resolve_rc;
    X4T26ModuleObservation modules[2];
    X4T26MemoryObservation memory[16];
    unsigned memory_observations,memory_observations_dropped;
} X4T26PlatformInfo;
typedef struct X4T26Platform X4T26Platform;
int x4_t26_platform_create(X4T26Platform **,X4T26BeforeNative,void *);
void x4_t26_platform_v1_ops(X4T26Platform *,X4T26V1Ops *);
void x4_t26_platform_info(const X4T26Platform *,X4T26PlatformInfo *);
void x4_t26_platform_cpu(X4T26Platform *,X4T26CpuSnapshot *);
/* called is false if resolution failed. Query rc is always kept verbatim. */
int x4_t26_platform_query(X4T26Platform *,int64_t offset,X4T26DirectQuery *,bool *called);
int x4_t26_platform_largest(X4T26Platform *,uint64_t *bytes,bool *called);
void x4_t26_platform_free(X4T26Platform **);
#endif
