/* SPDX-License-Identifier: GPL-3.0-only */
/* Bridge SDK musl attribute storage (four bytes in precompiled libc++) to
 * native opaque eight-byte Sony attributes. Do not write native pointers
 * into the caller's SDK object. The attribute is represented by its address
 * in a bounded registry until pthread_mutexattr_destroy. */
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

extern int32_t scePthreadMutexattrInit(void **);
extern int32_t scePthreadMutexattrDestroy(void **);
extern int32_t scePthreadMutexattrSettype(void **, int);
extern int32_t scePthreadMutexInit(void *, const void *, const char *);
typedef struct { uint32_t state; void *mutex; } NativeOnce;
_Static_assert(sizeof(NativeOnce)==16 && offsetof(NativeOnce,mutex)==8,
               "Native pthread_once layout requires the PS4 x86_64 ABI");
extern int32_t scePthreadOnce(NativeOnce *, void (*)(void));
extern int32_t scePthreadGetthreadid(void);

/* The SDK's precompiled libc++/libc++abi cxa_guard uses syscall(20) as
 * SYS_gettid, but its musl syscall function returns ENOSYS. This is the
 * only observed syscall consumer in the linked archives. Resolve that
 * SDK-specific operation through the real native thread API; preserve
 * the SDK's explicit unsupported result for any other syscall number. */
long syscall(long number, ...)
{
    if(number==20)return (long)scePthreadGetthreadid();
    errno=ENOSYS;
    return -1;
}
typedef struct { const void *key; NativeOnce once; } OnceBinding;
/* libc++/libc++abi use process-lifetime static once objects. NativeOnce is
 * sixteen bytes; their precompiled SDK object is only four bytes. Keep the
 * native object in stable process-lifetime storage instead. */
static OnceBinding once_bindings[128];

typedef struct { const void *key; void *attribute; } Attribute;
static Attribute attributes[32];
static atomic_flag attribute_lock = ATOMIC_FLAG_INIT;
static void lock(void) { while (atomic_flag_test_and_set_explicit(&attribute_lock,memory_order_acquire)) {} }
static void unlock(void) { atomic_flag_clear_explicit(&attribute_lock,memory_order_release); }
static int posix_error(int32_t error) { return error < 0 ? (int)((uint32_t)error & 0xffffu) : error; }
static Attribute *lookup(const void *key)
{
    for(size_t i=0;i<32;++i)if(attributes[i].key==key)return &attributes[i];
    return NULL;
}
int pthread_mutexattr_init(pthread_mutexattr_t *key)
{
    if(!key)return EINVAL;
    lock();
    if(lookup(key)){unlock();return EBUSY;}
    Attribute *slot=NULL;
    for(size_t i=0;i<32;++i)if(!attributes[i].key){slot=&attributes[i];break;}
    if(!slot){unlock();return ENOMEM;}
    int rc=posix_error(scePthreadMutexattrInit(&slot->attribute));
    if(!rc)slot->key=key;
    unlock();return rc;
}
int pthread_mutexattr_settype(pthread_mutexattr_t *key,int type)
{
    /* SDK v0.5.4 already defines Sony's ErrorCheck1/Recursive2/Normal3.
     * Reject foreign enum values instead of silently choosing another type. */
    if(type!=PTHREAD_MUTEX_ERRORCHECK && type!=PTHREAD_MUTEX_RECURSIVE && type!=PTHREAD_MUTEX_NORMAL)return EINVAL;
    lock();Attribute *slot=lookup(key);
    int rc=slot?posix_error(scePthreadMutexattrSettype(&slot->attribute,type)):EINVAL;
    unlock();return rc;
}

int pthread_once(pthread_once_t *key,void (*initialize)(void))
{
    if(!key||!initialize)return EINVAL;
    lock();OnceBinding *slot=NULL;
    for(size_t i=0;i<128;++i)if(once_bindings[i].key==key){slot=&once_bindings[i];break;}
    if(!slot)for(size_t i=0;i<128;++i)if(!once_bindings[i].key){slot=&once_bindings[i];slot->key=key;break;}
    unlock();
    if(!slot)return ENOMEM;
    /* Do not hold the registry lock: initialize may itself use another once
     * object. Native pthread_once supplies waiting and single execution. */
    return posix_error(scePthreadOnce(&slot->once,initialize));
}
int pthread_mutexattr_destroy(pthread_mutexattr_t *key)
{
    lock();Attribute *slot=lookup(key);
    int rc=slot?posix_error(scePthreadMutexattrDestroy(&slot->attribute)):EINVAL;
    if(slot&&!rc)*slot=(Attribute){0};
    unlock();return rc;
}
int pthread_mutex_init(pthread_mutex_t *mutex,const pthread_mutexattr_t *key)
{
    if(!mutex)return EINVAL;
    lock();Attribute *slot=key?lookup(key):NULL;
    int rc=key&&!slot?EINVAL:posix_error(scePthreadMutexInit(mutex,slot?&slot->attribute:NULL,"x4-rtc"));
    unlock();return rc;
}
