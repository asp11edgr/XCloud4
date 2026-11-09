/* SPDX-License-Identifier: GPL-3.0-only */
#include "session_store.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef X4_SESSION_STORE_TEST
#include <orbis/SaveData.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>
#endif

enum { STORE_ERROR = -2070, STORE_CORRUPT = -2071, HEADER = 16 };
/* SaveData mount policy is shared verbatim with the host simulator. Values
 * are checked against the installed SDK below; never use CREATE2 (0x20). */
enum { STORE_RDWR=2, STORE_CREATE=4, STORE_BLOCKS=96, STORE_BLOCKS_FALLBACK=1024 };
#define STORE_PARAMETER UINT32_C(0x809f0000)
#define STORE_EXISTS UINT32_C(0x809f0007)
#define STORE_NOT_FOUND UINT32_C(0x809f0008)
#define STORE_DIRECTORY "XCLOUD4SESSION"
typedef int (*StoreSearch)(void *,bool *);
typedef int (*StoreMountAttempt)(void *,unsigned,unsigned);
static int mount_policy(void *context,bool writing,StoreSearch search,StoreMountAttempt attempt)
{
    bool exists=true;
    if(writing){int rc=search(context,&exists);if(rc)return rc;}
    unsigned mode=STORE_RDWR|((writing&&!exists)?STORE_CREATE:0),blocks=STORE_BLOCKS;
    bool retried_exists=false,retried_missing=false,retried_blocks=false;
    /* At most one retry per documented race and one block-size retry. */
    for(unsigned n=0;n<4;++n){
        int rc=attempt(context,mode,blocks);
        printf("XCloud4: SaveData montaje modo=0x%02x bloques=%u codigo=0x%08x\n",mode,blocks,(unsigned)rc);
        if(!rc)return 0;
        if(writing&&(mode&STORE_CREATE)&&(uint32_t)rc==STORE_EXISTS&&!retried_exists){
            retried_exists=true;mode=STORE_RDWR;continue;
        }
        if(writing&&!(mode&STORE_CREATE)&&(uint32_t)rc==STORE_NOT_FOUND&&!retried_missing){
            retried_missing=true;mode=STORE_RDWR|STORE_CREATE;continue;
        }
        if((mode&STORE_CREATE)&&(uint32_t)rc==STORE_PARAMETER&&blocks==STORE_BLOCKS&&!retried_blocks){
            retried_blocks=true;blocks=STORE_BLOCKS_FALLBACK;continue;
        }
        return rc;
    }
    return STORE_ERROR;
}
#ifdef X4_SESSION_STORE_TEST
/* Test-only bridge, deliberately not part of the production public header. */
int x4_session_store_test_mount_policy(void *context,bool writing,StoreSearch search,StoreMountAttempt attempt)
{return mount_policy(context,writing,search,attempt);}
const char *x4_session_store_test_directory(void) { return STORE_DIRECTORY; }
#endif

typedef struct {
    void *context;
    int (*exists)(void *, bool *), (*mount)(void *, bool);
    int (*read)(void *, void *, size_t, size_t *);
    int (*write)(void *, const void *, size_t), (*erase)(void *), (*unmount)(void *);
} Backend;
#ifndef X4_SESSION_STORE_TEST
typedef struct {
    int32_t user;
    OrbisSaveDataDirName directory;
    OrbisSaveDataMountPoint point;
    bool mounted, valid_path;
} Native;
#endif
struct X4SessionStore {
    Backend backend;
    atomic_bool enabled, available;
    atomic_int error;
    unsigned char record[HEADER + X4_SESSION_REFRESH_CAPACITY];
#ifndef X4_SESSION_STORE_TEST
    Native native;
#endif
};
static void clear(void *p, size_t n) { volatile unsigned char *v=p; while(n--) *v++=0; }
static int unavailable(X4SessionStore *s, int rc)
{
    atomic_store(&s->error,rc?rc:STORE_ERROR);
    atomic_store_explicit(&s->available,false,memory_order_release);
    printf("XCloud4: SaveData sesion no disponible codigo=0x%08x\n",(unsigned)(rc?rc:STORE_ERROR));
    return rc<0?rc:STORE_ERROR;
}
static size_t length(const char *p) {
    if(!p)return 0;
    size_t n=0;while(n<X4_SESSION_REFRESH_CAPACITY&&p[n]) {
        unsigned char c=(unsigned char)p[n++];if(c<0x21||c>0x7e)return 0;
    }
    return n<X4_SESSION_REFRESH_CAPACITY?n:0;
}
static uint32_t checksum(const unsigned char *p,size_t n) {
    uint32_t h=2166136261u;while(n--){h^=*p++;h*=16777619u;}return h;
}
static void put32(unsigned char *p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i)); }
static uint32_t get32(const unsigned char *p) { uint32_t v=0;for(unsigned i=0;i<4;++i)v|=(uint32_t)p[i]<<(8*i);return v; }
static int finish(X4SessionStore *s,int rc) {
    int u=s->backend.unmount(s->backend.context);
    clear(s->record,sizeof(s->record));
    if(u)return unavailable(s,u);
    return rc;
}
#ifndef X4_SESSION_STORE_TEST
static int native_exists(void *ctx,bool *yes) {
    Native *n=ctx;OrbisSaveDataDirName found={0};
    OrbisSaveDataDirNameSearchCond cond={0};cond.userId=n->user;cond.dirName=&n->directory;
    OrbisSaveDataDirNameSearchResult result={0};result.dirNames=&found;result.dirNamesNum=1;
    int rc=sceSaveDataDirNameSearch(&cond,&result);*yes=!rc&&result.hitNum>0;return rc;
}
_Static_assert(STORE_RDWR==ORBIS_SAVE_DATA_MOUNT_MODE_RDWR&&
    STORE_CREATE==ORBIS_SAVE_DATA_MOUNT_MODE_CREATE&&STORE_BLOCKS==ORBIS_SAVE_DATA_BLOCKS_MIN2,
    "session SaveData mount policy must match the installed SDK");
static int native_mount_attempt(void *ctx,unsigned mode,unsigned blocks)
{
    Native *n=ctx;OrbisSaveDataMount2 request={0};OrbisSaveDataMountResult result={0};
    request.userId=n->user;request.dirName=&n->directory;request.blocks=blocks;request.mountMode=mode;
    int rc=sceSaveDataMount2(&request,&result);if(rc)return rc;
    memcpy(n->point.data,result.mountPathName,sizeof(n->point.data));n->mounted=true;n->valid_path=false;
    return 0;
}
static int native_mount(void *ctx,bool create) {
    Native *n=ctx;
    if(n->mounted)return STORE_ERROR;
    int rc=mount_policy(n,create,native_exists,native_mount_attempt);if(rc)return rc;
    /* A SaveData result, never a caller-supplied fallback path. */
    size_t len=0;while(len<sizeof(n->point.data)&&n->point.data[len])++len;
    bool valid=len>1&&len<sizeof(n->point.data)&&n->point.data[0]=='/';
    for(size_t i=1;valid&&i<len;++i)valid=(n->point.data[i]>='a'&&n->point.data[i]<='z')||(n->point.data[i]>='A'&&n->point.data[i]<='Z')||(n->point.data[i]>='0'&&n->point.data[i]<='9')||n->point.data[i]=='_';
    if(!valid||!strncmp(n->point.data,"/data",5)) {
        int urc=sceSaveDataUmount(&n->point);if(!urc)n->mounted=false;return urc?urc:STORE_ERROR;
    }
    n->valid_path=true;
    return 0;
}
static int path(Native *n,char *out,size_t capacity,bool temp) {
    if(!n->mounted||!n->valid_path)return STORE_ERROR;
    int rc=snprintf(out,capacity,"%s/%s",n->point.data,temp?"ms-refresh.tmp":"ms-refresh.bin");
    return rc>0&&(size_t)rc<capacity?0:STORE_ERROR;
}
static int native_read(void *ctx,void *data,size_t cap,size_t *got) {
    Native *n=ctx;char p[64];*got=0;if(path(n,p,sizeof(p),false))return STORE_ERROR;
    errno=0;FILE *f=fopen(p,"rb");if(!f)return errno==ENOENT?1:STORE_ERROR;
    *got=fread(data,1,cap,f);int extra=fgetc(f);bool ok=!ferror(f)&&extra==EOF;
    if(fclose(f))ok=false;return ok?0:STORE_CORRUPT;
}
static int native_write(void *ctx,const void *data,size_t size) {
    Native *n=ctx;char target[64],temporary[64];if(path(n,target,sizeof(target),false)||path(n,temporary,sizeof(temporary),true))return STORE_ERROR;
    FILE *f=fopen(temporary,"wb");if(!f)return STORE_ERROR;
    bool ok=fwrite(data,1,size,f)==size&&fflush(f)==0;
    if(ok)ok=sceKernelFsync(fileno(f))==0;
    if(fclose(f))ok=false;
    if(ok)ok=sceKernelRename(temporary,target)==0;
    if(!ok)(void)sceKernelUnlink(temporary);
    return ok?0:STORE_ERROR;
}
static int native_erase(void *ctx) {
    Native *n=ctx;char p[64];
    for(unsigned i=0;i<2;++i) {
        if(path(n,p,sizeof(p),i!=0))return STORE_ERROR;
        (void)sceKernelUnlink(p);
        errno=0;FILE *f=fopen(p,"rb");if(f){fclose(f);return STORE_ERROR;}
        if(errno!=ENOENT)return STORE_ERROR;
    }
    return 0;
}
static int native_unmount(void *ctx) {
    Native *n=ctx;if(!n->mounted)return STORE_ERROR;
    int rc=sceSaveDataUmount(&n->point);
    if(rc) {
        /* Never leave a refresh credential in a failed open mount. Invalidate
         * the stored session, verify removal, then make one bounded retry. */
        int er=native_erase(n);int retry=sceSaveDataUmount(&n->point);
        if(!retry){n->mounted=false;n->valid_path=false;memset(&n->point,0,sizeof(n->point));}
        printf("XCloud4: SaveData desmontaje codigo=0x%08x borrado=0x%08x reintento=0x%08x\n",(unsigned)rc,(unsigned)er,(unsigned)retry);
        return rc;
    }
    n->mounted=false;n->valid_path=false;memset(&n->point,0,sizeof(n->point));return 0;
}
#endif
static X4SessionStore *allocate(void) {
    X4SessionStore *s=calloc(1,sizeof(*s));if(s){atomic_init(&s->enabled,false);atomic_init(&s->available,true);atomic_init(&s->error,0);}return s;
}
X4SessionStore *x4_session_store_new(void) {
#ifdef X4_SESSION_STORE_TEST
    return NULL;
#else
    X4SessionStore *s=allocate();if(!s)return NULL;
    Native *n=&s->native;
    _Static_assert(sizeof(STORE_DIRECTORY)<=sizeof(n->directory.data),"session SaveData directory must fit the SDK structure");
    memcpy(n->directory.data,STORE_DIRECTORY,sizeof(STORE_DIRECTORY));
    s->backend=(Backend){n,native_exists,native_mount,native_read,native_write,native_erase,native_unmount};
    int rc=sceUserServiceGetInitialUser(&n->user);
    if(rc==(int32_t)ORBIS_USER_SERVICE_ERROR_NOT_INITIALIZED) {
        OrbisUserServiceInitializeParams params={.priority=ORBIS_KERNEL_PRIO_FIFO_LOWEST};
        int init=sceUserServiceInitialize(&params);
        if(init&&init!=(int32_t)ORBIS_USER_SERVICE_ERROR_ALREADY_INITIALIZED) {
            unavailable(s,init);return s;
        }
        rc=sceUserServiceGetInitialUser(&n->user);
    }
    if(rc){unavailable(s,rc);return s;}
    rc=sceSaveDataInitialize3(0);if(rc)unavailable(s,rc);
    return s;
#endif
}
int x4_session_store_free(X4SessionStore *s)
{
    if(!s)return 0;
#ifndef X4_SESSION_STORE_TEST
    if(s->native.mounted) {
        /* Keep the mount descriptor and callbacks alive if the firmware still
         * refuses closure. The existing backend performs bounded cleanup. */
        int rc=s->backend.unmount(s->backend.context);
        if(rc)return unavailable(s,rc);
    }
#endif
    clear(s,sizeof(*s));free(s);return 0;
}
void x4_session_store_snapshot(const X4SessionStore *s,X4SessionStoreSnapshot *out) {
    if(!out)return;*out=(X4SessionStoreSnapshot){0};if(!s)return;
    out->available=atomic_load_explicit(&s->available,memory_order_acquire);
    out->enabled=atomic_load(&s->enabled);out->error=atomic_load(&s->error);
}
int x4_session_store_restore(X4SessionStore *s,char *refresh,size_t capacity) {
    if(!refresh||!capacity)return STORE_ERROR;clear(refresh,capacity);
    if(!s||!atomic_load(&s->available))return STORE_ERROR;
    bool exists=false;int rc=s->backend.exists(s->backend.context,&exists);
    if(rc)return unavailable(s,rc);if(!exists)return 0;
    rc=s->backend.mount(s->backend.context,false);if(rc)return unavailable(s,rc);
    size_t got=0;rc=s->backend.read(s->backend.context,s->record,sizeof(s->record),&got);
    uint32_t bytes=got>=HEADER?get32(s->record+4):0;
    bool valid=!rc&&got==HEADER+(size_t)bytes&&bytes>0&&bytes<capacity&&bytes<X4_SESSION_REFRESH_CAPACITY&&
        !memcmp(s->record,"X4R1",4)&&get32(s->record+8)==checksum(s->record+HEADER,bytes)&&get32(s->record+12)==0;
    if(valid){memcpy(refresh,s->record+HEADER,bytes);refresh[bytes]=0;valid=length(refresh)==bytes;}
    if(!valid) {
        clear(refresh,capacity);
        /* Corruption is not a credential. Remove and verify inside the mount. */
        int er=s->backend.erase(s->backend.context);
        rc=finish(s,er);if(rc)return unavailable(s,rc);return 0;
    }
    rc=finish(s,0);if(rc){clear(refresh,capacity);return rc;}
    atomic_store(&s->enabled,true);return 1;
}
int x4_session_store_rotate(X4SessionStore *s,const char *refresh) {
    if(!s||!atomic_load(&s->available))return STORE_ERROR;
    if(!atomic_load(&s->enabled))return 0;
    size_t n=length(refresh);if(!n)return unavailable(s,STORE_ERROR);
    memset(s->record,0,HEADER);memcpy(s->record,"X4R1",4);put32(s->record+4,(uint32_t)n);
    put32(s->record+8,checksum((const unsigned char *)refresh,n));memcpy(s->record+HEADER,refresh,n);
    int rc=s->backend.mount(s->backend.context,true);
    if(rc){clear(s->record,sizeof(s->record));return unavailable(s,rc);}
    rc=s->backend.write(s->backend.context,s->record,HEADER+n);rc=finish(s,rc);
    return rc?unavailable(s,rc):0;
}
int x4_session_store_erase(X4SessionStore *s) {
    if(!s)return STORE_ERROR;
    atomic_store(&s->enabled,false);
    /* Retry erasure even after an earlier failure; never claim removal on error. */
    if(!s->backend.exists)return STORE_ERROR;
    bool exists=false;int rc=s->backend.exists(s->backend.context,&exists);
    if(rc)return unavailable(s,rc);if(!exists)return 0;
    rc=s->backend.mount(s->backend.context,false);if(rc)return unavailable(s,rc);
    rc=s->backend.erase(s->backend.context);rc=finish(s,rc);return rc?unavailable(s,rc):0;
}
int x4_session_store_set_enabled(X4SessionStore *s,bool enabled,const char *refresh) {
    if(!s)return STORE_ERROR;if(!enabled)return x4_session_store_erase(s);
    if(!atomic_load(&s->available))return STORE_ERROR;
    atomic_store(&s->enabled,true);return refresh?x4_session_store_rotate(s,refresh):0;
}
#ifdef X4_SESSION_STORE_TEST
X4SessionStore *x4_session_store_test_new(const X4SessionStoreTestBackend *b) {
    if(!b||!b->exists||!b->mount||!b->read||!b->write||!b->erase||!b->unmount)return NULL;
    X4SessionStore *s=allocate();if(s)s->backend=(Backend){b->context,b->exists,b->mount,b->read,b->write,b->erase,b->unmount};return s;
}
#endif
