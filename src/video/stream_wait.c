/* SPDX-License-Identifier: GPL-3.0-only */
#if X4_STREAM_WAIT_HOST && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "stream_wait.h"
/* T26's realistic cell runs the unchanged streaming scheduler in the probe. */
#if (X4_STREAM_PROFILE && (!X4_VIDEO_PROBE || X4_PROBE_T26)) || X4_STREAM_WAIT_HOST
#include <string.h>
#include <errno.h>
#if X4_STREAM_WAIT_HOST
#include <time.h>
static int injected_init;
static unsigned injected_wait_destroy_ordinal,injected_mutex_destroy_ordinal;
static int injected_wait_destroy_error,injected_mutex_destroy_error;
static atomic_uint_fast64_t fixture_api_calls;
static _Atomic(X4StreamWait *) injected_notify_target,injected_wait_target;
static atomic_int injected_notify_error,injected_wait_error;
uint64_t x4_stream_wait_test_api_calls(void){return atomic_load_explicit(&fixture_api_calls,memory_order_relaxed);}
static void fixture_api_call(void){atomic_fetch_add_explicit(&fixture_api_calls,1,memory_order_relaxed);}
void x4_stream_wait_test_fail_notify(X4StreamWait *w,int error)
{atomic_store(&injected_notify_error,error);atomic_store_explicit(&injected_notify_target,w,memory_order_release);}
void x4_stream_wait_test_fail_wait(X4StreamWait *w,int error)
{atomic_store(&injected_wait_error,error);atomic_store_explicit(&injected_wait_target,w,memory_order_release);}
static int fixture_take_fault(_Atomic(X4StreamWait *) *target,atomic_int *error,X4StreamWait *w)
{X4StreamWait *expected=w;
 return atomic_compare_exchange_strong_explicit(target,&expected,NULL,memory_order_acq_rel,memory_order_relaxed)?atomic_load(error):0;}
void x4_stream_wait_test_fail_init(int error){injected_init=error;}
void x4_stream_wait_test_fail_destroy(unsigned ordinal,int error)
{injected_wait_destroy_ordinal=ordinal;injected_wait_destroy_error=error;}
void x4_stream_mutex_test_fail_destroy(unsigned ordinal,int error)
{injected_mutex_destroy_ordinal=ordinal;injected_mutex_destroy_error=error;}
static int create_wait(X4StreamWait *w,const char *name)
{fixture_api_call();(void)name;if(injected_init){int rc=injected_init;injected_init=0;return rc;}
 return sem_init(&w->handle,0,0)?errno:0;}
static int signal_wait(X4StreamWait *w){fixture_api_call();return sem_post(&w->handle)?errno:0;}
static int wait_wait(X4StreamWait *w,unsigned us)
{fixture_api_call();struct timespec deadline;if(clock_gettime(CLOCK_REALTIME,&deadline))return errno;
 deadline.tv_sec+=us/1000000u;deadline.tv_nsec+=(long)(us%1000000u)*1000L;
 if(deadline.tv_nsec>=1000000000L){++deadline.tv_sec;deadline.tv_nsec-=1000000000L;}
 return sem_timedwait(&w->handle,&deadline)?errno:0;}
static int destroy_wait(X4StreamWait *w)
{fixture_api_call();if(injected_wait_destroy_ordinal&&!--injected_wait_destroy_ordinal)return injected_wait_destroy_error;
 return sem_destroy(&w->handle)?errno:0;}
static bool timed_out(int rc){return rc==ETIMEDOUT;}
static bool interrupted(int rc){return rc==EINTR;}
int x4_stream_mutex_init(X4StreamMutex *m,const char *name){fixture_api_call();(void)name;return pthread_mutex_init(m,NULL);}
int x4_stream_mutex_lock(X4StreamMutex *m){fixture_api_call();return pthread_mutex_lock(m);}
int x4_stream_mutex_trylock(X4StreamMutex *m){fixture_api_call();return pthread_mutex_trylock(m);}
int x4_stream_mutex_unlock(X4StreamMutex *m){fixture_api_call();return pthread_mutex_unlock(m);}
int x4_stream_mutex_destroy(X4StreamMutex *m)
{fixture_api_call();if(injected_mutex_destroy_ordinal&&!--injected_mutex_destroy_ordinal)return injected_mutex_destroy_error;
 return pthread_mutex_destroy(m);}
#else
/* Native SDK libkernel.h: semaphore timeout is a relative microsecond value.
 * Do not use musl sem_timedwait, clock_gettime or Linux errno numerics here. */
static int create_wait(X4StreamWait *w,const char *name)
{return sceKernelCreateSema(&w->handle,name,ORBIS_KERNEL_SEMA_ATTR_TH_FIFO,0,1,NULL);}
static int signal_wait(X4StreamWait *w){return sceKernelSignalSema(w->handle,1);}
static int wait_wait(X4StreamWait *w,unsigned us)
{OrbisKernelUseconds timeout=us;return sceKernelWaitSema(w->handle,1,&timeout);}
static int destroy_wait(X4StreamWait *w){return sceKernelDeleteSema(w->handle);}
/* FreeBSD/PS4 ETIMEDOUT=60 and EINTR=4, encoded by native libkernel. */
static bool timed_out(int rc){return (uint32_t)rc==UINT32_C(0x8002003c);}
static bool interrupted(int rc){return (uint32_t)rc==UINT32_C(0x80020004);}
int x4_stream_mutex_init(X4StreamMutex *m,const char *name){return scePthreadMutexInit(m,NULL,name);}
int x4_stream_mutex_lock(X4StreamMutex *m){return scePthreadMutexLock(m);}
int x4_stream_mutex_trylock(X4StreamMutex *m){return scePthreadMutexTrylock(m);}
int x4_stream_mutex_unlock(X4StreamMutex *m){return scePthreadMutexUnlock(m);}
int x4_stream_mutex_destroy(X4StreamMutex *m){return scePthreadMutexDestroy(m);}
#endif
static int remember_error(X4StreamWait *w,int rc)
{if(!rc)return 0;int first=0;atomic_fetch_add_explicit(&w->errors,1,memory_order_relaxed);
 (void)atomic_compare_exchange_strong_explicit(&w->first_error,&first,rc,memory_order_relaxed,memory_order_relaxed);
 return rc<0?rc:-rc;}
int x4_stream_wait_init(X4StreamWait *w,const char *name)
{if(!w||!name)return -EINVAL;memset(w,0,sizeof(*w));
 atomic_init(&w->posted,false);atomic_init(&w->closed,false);
 atomic_init(&w->wait_calls,0);atomic_init(&w->wakeups,0);atomic_init(&w->timeouts,0);
 atomic_init(&w->notifications,0);atomic_init(&w->coalesced,0);atomic_init(&w->errors,0);
 atomic_init(&w->first_error,0);int rc=create_wait(w,name);
 if(rc)return remember_error(w,rc);
 w->initialized=true;return 0;}
int x4_stream_wait_notify(X4StreamWait *w)
{if(!w||!w->initialized)return -EINVAL;
#if X4_STREAM_WAIT_HOST
 int injected=fixture_take_fault(&injected_notify_target,&injected_notify_error,w);
 if(injected)return remember_error(w,injected);
#endif
 atomic_fetch_add_explicit(&w->notifications,1,memory_order_relaxed);
 if(atomic_exchange_explicit(&w->posted,true,memory_order_acq_rel)){
    atomic_fetch_add_explicit(&w->coalesced,1,memory_order_relaxed);return 0;}
 int rc=signal_wait(w);if(rc)atomic_store_explicit(&w->posted,false,memory_order_release);
 return remember_error(w,rc);}
int x4_stream_wait_block(X4StreamWait *w,unsigned timeout_us)
{if(!w||!w->initialized)return -EINVAL;
#if X4_STREAM_WAIT_HOST
 int injected=fixture_take_fault(&injected_wait_target,&injected_wait_error,w);
 if(injected)return remember_error(w,injected);
#endif
 if(atomic_load_explicit(&w->closed,memory_order_acquire))return X4_STREAM_WAIT_CLOSED;
 int failure=atomic_load_explicit(&w->first_error,memory_order_relaxed);if(failure)return failure<0?failure:-failure;
 if(!timeout_us||timeout_us>X4_STREAM_WAIT_MAX_US)timeout_us=X4_STREAM_WAIT_MAX_US;
 atomic_fetch_add_explicit(&w->wait_calls,1,memory_order_relaxed);
 int rc=wait_wait(w,timeout_us);
 atomic_fetch_add_explicit(&w->wakeups,1,memory_order_relaxed);
 /* A producer may publish work and coalesce while the consumed token still
  * has posted=true. Acquire that producer's release RMW before the caller
  * rechecks its predicate; a release-only clear does not establish that edge. */
 if(!rc)(void)atomic_exchange_explicit(&w->posted,false,memory_order_acq_rel);
 if(atomic_load_explicit(&w->closed,memory_order_acquire))return X4_STREAM_WAIT_CLOSED;
 if(timed_out(rc)){atomic_fetch_add_explicit(&w->timeouts,1,memory_order_relaxed);return X4_STREAM_WAIT_TIMEOUT;}
 if(interrupted(rc))return X4_STREAM_WAIT_EVENT; /* predicate/deadline recheck */
 return remember_error(w,rc);}
int x4_stream_wait_close(X4StreamWait *w)
{if(!w||!w->initialized)return 0;atomic_store_explicit(&w->closed,true,memory_order_release);
 return x4_stream_wait_notify(w);}
int x4_stream_wait_destroy(X4StreamWait *w)
{if(!w||!w->initialized)return 0;int rc=destroy_wait(w);if(!rc)w->initialized=false;
 return remember_error(w,rc);}
void x4_stream_wait_snapshot(const X4StreamWait *w,X4StreamWaitStats *s)
{if(!s)return;memset(s,0,sizeof(*s));if(!w||!w->initialized)return;
#define COPY(field) s->field=atomic_load_explicit(&w->field,memory_order_relaxed)
 COPY(wait_calls);COPY(wakeups);COPY(timeouts);COPY(notifications);COPY(coalesced);COPY(errors);COPY(first_error);
#undef COPY
}
#endif
