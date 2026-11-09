/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/probe_t26_platform.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned calls;static int behavior;
static int32_t usage(int32_t who,X4T26Rusage *r)
{
    assert(who==(int32_t)(calls++%2));
    if(behavior==1)return -123;
    if(behavior==2)return 0; /* Native success stub without published data. */
    memset(r,0,sizeof(*r));r->user.seconds=who?2:4;r->user.microseconds=500;
    r->system.microseconds=700;return 0;
}
int main(void)
{
    X4T26Rusage r={.user={4,700},.system={1,800}};uint64_t us=99;
    assert(x4_t26_rusage_cpu_us(&r,&us)&&us==5001500);
    r.user.microseconds=1000000;us=99;
    assert(!x4_t26_rusage_cpu_us(&r,&us)&&us==99);
    r.user.microseconds=-1;assert(!x4_t26_rusage_cpu_us(&r,&us));
    r.user=(X4T26Timeval){INT64_MAX,0};assert(!x4_t26_rusage_cpu_us(&r,&us));
    r.user=(X4T26Timeval){-1,0};assert(!x4_t26_rusage_cpu_us(&r,&us));
    assert(!x4_t26_rusage_cpu_us(NULL,&us));assert(!x4_t26_rusage_cpu_us(&r,NULL));
    X4T26CpuSnapshot before,after;
    x4_t26_cpu_snapshot_with(NULL,-44,&before);
    assert(!before.process_available&&!before.thread_available&&before.resolve_rc==-44&&
        before.process_rc==X4_T26_PLATFORM_NOT_RUN&&before.thread_rc==X4_T26_PLATFORM_NOT_RUN);
    x4_t26_cpu_snapshot_with(usage,0,&before);
    assert(calls==2&&before.process_available&&before.thread_available&&before.process_us==4001200&&before.thread_us==2001200);
    behavior=1;x4_t26_cpu_snapshot_with(usage,0,&after);
    assert(calls==4&&!after.process_available&&!after.thread_available&&after.process_rc==-123&&after.thread_rc==-123);
    behavior=2;x4_t26_cpu_snapshot_with(usage,0,&after);
    assert(calls==6&&!after.process_available&&!after.thread_available&&after.process_rc==0&&after.thread_rc==0);
    after=before;after.process_us+=40000;after.thread_us+=10000;X4T26CpuDelta delta;
    x4_t26_cpu_delta(&before,&after,10000,&delta);
    assert(delta.process_available&&delta.process_us==40000&&delta.process_percent==400.0&&delta.thread_percent==100.0);
    after.process_us=before.process_us-1;x4_t26_cpu_delta(&before,&after,10000,&delta);
    assert(!delta.process_available&&delta.thread_available);
    x4_t26_cpu_delta(&before,&after,0,&delta);assert(!delta.process_available&&!delta.thread_available);
    X4T26Allocation allocation={.physical_offset=0x8000,.bytes=0x4000,.memory_type=0};
    X4T26DirectQuery query={.start=0x4000,.end=0x10000,.memory_type=0};
    assert(x4_t26_query_contains(&query,&allocation));
    query.end=0x9000;assert(!x4_t26_query_contains(&query,&allocation));
    query.end=0x10000;query.memory_type=3;assert(!x4_t26_query_contains(&query,&allocation));
    query.memory_type=0;query.start=0x9000;assert(!x4_t26_query_contains(&query,&allocation));
    query.start=-1;assert(!x4_t26_query_contains(&query,&allocation));
    query.start=0;allocation.bytes=UINT64_MAX;assert(!x4_t26_query_contains(&query,&allocation));
    allocation.bytes=0;assert(!x4_t26_query_contains(&query,&allocation));
    assert(x4_t26_query_release_evidence(true,(int32_t)UINT32_C(0x8002000d)));
    assert(!x4_t26_query_release_evidence(false,(int32_t)UINT32_C(0x8002000d)));
    assert(!x4_t26_query_release_evidence(true,0));
    assert(!x4_t26_query_release_evidence(true,(int32_t)UINT32_C(0x80020016)));
    assert(!x4_t26_query_release_evidence(true,-1));
    puts("PASS T26 typed CPU, unavailable snapshots, >100 percent CPU, and exact release evidence");return 0;
}
