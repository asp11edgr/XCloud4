/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#if defined(X4_OPENORBIS) && !defined(_BSD_SOURCE)
#define _BSD_SOURCE
#endif
#include "catalog_disk_cache.h"
#include "../auth/http_client.h"
#include "../core/x4_log.h"
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>
#endif

/* File names are derived from complete, validated public keys. Each file
 * stores its complete key as well: a hash collision is never a cache hit.
 * The host uses descriptor-relative operations. PS4 uses guarded paths and
 * checks the fixed root against its held directory descriptor. */
#define RECORDS_PER_PRODUCT (X4_COVER_SURFACES + 2u)
#define MISS_NOTES 256u
#define KEY_CAP 640u
#define FILE_CAP 41u
#define HEADER_SIZE 48u
#define USAGE_OFFSET 44u
#define IO_CHUNK 65536u
#define SCAN_BOUND (2u * X4_CATALOG_DISK_RECORDS + 64u)
#if !X4_PUBLIC && (defined(X4_OPENORBIS) || defined(X4_DISK_CACHE_PROBE_LOG_TEST))
#define X4_DISK_CORRUPTION_LOG 1
#endif
#if defined(X4_OPENORBIS) || defined(X4_DISK_CACHE_NATIVE_FS_TEST)
#define X4_DISK_PATH_MUTATIONS 1
#endif
/* The disk-cache harness supplies BSD records. The assets harness exercises
 * guarded paths with the host directory reader. */
#if defined(X4_OPENORBIS) || \
    (defined(X4_DISK_CACHE_NATIVE_FS_TEST) && defined(X4_DISK_CACHE_PROBE_LOG_TEST))
#define X4_DISK_RAW_SCAN 1
#endif
enum { KIND_METADATA = 1, KIND_IMAGE = 2 };
typedef enum {
    MISS_NONE, MISS_ABSENT, MISS_EXPIRED, MISS_CORRUPT, MISS_KEY, MISS_EVICTED, MISS_FILTERED
} MissReason;
typedef enum {
    CHECK_OK, CHECK_FSTAT, CHECK_FILE_TYPE, CHECK_FILE_SEEK, CHECK_HEADER_SIZE, CHECK_HEADER_READ, CHECK_MAGIC,
    CHECK_VERSION, CHECK_KIND, CHECK_HEADER_CRC, CHECK_KEY_SIZE, CHECK_PAYLOAD_SIZE,
    CHECK_FILE_SIZE, CHECK_METADATA_SIZE, CHECK_KEY_READ, CHECK_KEY_NUL, CHECK_KEY_CRC,
    CHECK_KEY_FIELDS, CHECK_KEY_POLICY, CHECK_FILENAME, CHECK_LOOKUP_KEY, CHECK_PAYLOAD_READ,
    CHECK_PAYLOAD_CRC, CHECK_PAYLOAD_METADATA, CHECK_PAYLOAD_IMAGE, CHECK_COUNT
} CacheCheck;
typedef struct {
    int64_t stat_size, file_size;
    uint32_t kn, pn;
    size_t transferred, expected;
    CacheCheck failed;
    int error;
} ReadCheck;
typedef struct {
    char name[FILE_CAP];
    MissReason reason;
} MissNote;
typedef struct {
    char id[X4_PRODUCT_ID_CAP];
    unsigned refs;
} Product;
typedef struct {
    char name[FILE_CAP], key[KEY_CAP], id[X4_PRODUCT_ID_CAP];
    size_t size;
    uint64_t created, used, order;
    unsigned kind;
    int occupied;
} Record;
struct X4CatalogDiskCache {
    int directory;
    char root[1024];
#ifdef X4_DISK_PATH_MUTATIONS
    struct stat root_identity;
    int dir_flags_unsupported, file_flags_unsupported;
    int unstable_device, unstable_inode, header_usage;
#endif
    size_t budget;
    unsigned max_products, capacity, first_free;
    X4CatalogDiskClock clock;
    void *clock_context;
    uint64_t order;
    uint32_t crc_table[256];
    unsigned protected_count, next_miss_note;
    char protected_id[X4_CATALOG_DISK_PROTECTED][X4_PRODUCT_ID_CAP];
    Product product[X4_CATALOG_DISK_PRODUCTS + 1u];
    MissNote misses[MISS_NOTES];
    Record record[X4_CATALOG_DISK_RECORDS];
    X4CatalogDiskStats stats;
#ifdef X4_DISK_PATH_MUTATIONS
    CacheCheck last_check;
#endif
#ifdef X4_DISK_CORRUPTION_LOG
    unsigned rejected[CHECK_COUNT];
#endif
};
#if defined(X4_DISK_PATH_MUTATIONS) || defined(X4_DISK_CORRUPTION_LOG)
static const char *const check_name[CHECK_COUNT]={
    "ninguno","fstat","tipo","lseek","cabecera_corta","leer_cabecera","magia","version","kind",
    "crc_cabecera","longitud_clave","longitud_payload","tamano_archivo","tamano_metadata","leer_clave",
    "nul_clave","crc_clave","campos_clave","politica_clave","nombre","clave","leer_payload",
    "crc_payload","contenido_metadata","contenido_imagen"
};
#endif
static int reject_record(ReadCheck *check,CacheCheck reason,int error)
{
    check->failed=reason;
    check->error=error;
    return 0;
}
static void note_rejection(X4CatalogDiskCache *c,const char *phase,const ReadCheck *check)
{
#ifdef X4_DISK_PATH_MUTATIONS
    c->last_check=check->failed;
#endif
#ifdef X4_DISK_CORRUPTION_LOG
    static _Atomic unsigned reported;
    if(c->rejected[check->failed]!=UINT32_MAX)++c->rejected[check->failed];
    unsigned count=atomic_load_explicit(&reported,memory_order_relaxed);
    while(count<32){
        if(atomic_compare_exchange_weak_explicit(&reported,&count,count+1,
                memory_order_relaxed,memory_order_relaxed)){
            x4_log_printf("XCloud4: cache registro rechazado fase=%s motivo=%s st_size=%lld file_size=%lld kn=%u pn=%u leido=%zu esperado=%zu errno=%d\n",
                phase,check_name[check->failed],(long long)check->stat_size,(long long)check->file_size,(unsigned)check->kn,
                (unsigned)check->pn,check->transferred,check->expected,check->error);
            break;
        }
    }
#else
    (void)c;(void)phase;(void)check;
#endif
}
static void rejection_summary(X4CatalogDiskCache *c,const char *phase)
{
#ifdef X4_DISK_CORRUPTION_LOG
    char text[832];size_t used=0;
    for(unsigned i=1;i<CHECK_COUNT;++i)if(c->rejected[i]){
        int n=snprintf(text+used,sizeof(text)-used," %s=%u",check_name[i],c->rejected[i]);
        if(n<0||(size_t)n>=sizeof(text)-used)break;
        used+=(size_t)n;c->rejected[i]=0;
    }
    if(used)x4_log_printf("XCloud4: cache rechazos resumen fase=%s%s\n",phase,text);
#else
    (void)c;(void)phase;
#endif
}
static uint64_t utc_clock(void *unused)
{
    (void)unused; time_t n = time(NULL); return n > 0 ? (uint64_t)n : 0;
}
static uint64_t monotonic_us(void)
{
#ifdef X4_OPENORBIS
    return sceKernelGetProcessTime();
#else
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;
    return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
#endif
}
static int cancelled(X4CatalogDiskCache *c, const _Atomic int *flag)
{
    if (flag && atomic_load(flag)) { if (c) ++c->stats.cancelled; return 1; }
    return 0;
}
static void put32(unsigned char *p, uint32_t x)
{ for (unsigned i=0;i<4;++i) p[i]=(unsigned char)(x>>(8*i)); }
static uint32_t get32(const unsigned char *p)
{ uint32_t n=0; for(unsigned i=0;i<4;++i)n|=(uint32_t)p[i]<<(8*i); return n; }
static void put64(unsigned char *p, uint64_t x)
{ for (unsigned i=0;i<8;++i) p[i]=(unsigned char)(x>>(8*i)); }
static uint64_t get64(const unsigned char *p)
{ uint64_t n=0;for(unsigned i=0;i<8;++i)n|=(uint64_t)p[i]<<(8*i);return n; }
static uint32_t crc(X4CatalogDiskCache *c,uint32_t v,const void *p,size_t n)
{
    const unsigned char *s=p;
    while(n--)v=c->crc_table[(v^*s++)&255]^(v>>8);
    return v;
}
static int exact_io(int fd,void *bytes,size_t n,int writing,
    X4CatalogDiskCache *c,const _Atomic int *cancel,size_t *transferred)
{
    unsigned char *p=bytes;
    unsigned retries=0;
    if(transferred)*transferred=0;
    while(n){
        if(cancelled(c,cancel))return 0;
        size_t chunk=n<IO_CHUNK?n:IO_CHUNK;
        ssize_t r=writing?write(fd,p,chunk):read(fd,p,chunk);
        if(r<0){
            if((errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)&&retries++<8)continue;
            return -1;
        }
        if(r==0){errno=writing?EIO:0;return -1;}
        if((size_t)r>chunk){errno=EIO;return -1;}
        retries=0;
        if(transferred)*transferred+=(size_t)r;
        p+=(size_t)r;n-=(size_t)r;
    }
    return 1;
}
static int locale(const char *s,size_t max,int language)
{
    if(!s)return 0;
    size_t n=0;
    while(n<max&&s[n]){
        unsigned char v=(unsigned char)s[n];
        if(!((v>='A'&&v<='Z')||(v>='a'&&v<='z')||
            (language&&((v>='0'&&v<='9')||v=='-'))))return 0;
        ++n;
    }
    return language?(n>=2&&n<max):(n==2);
}
static void canonical(char *dst,const char *src,int upper)
{
    while(*src){unsigned char c=(unsigned char)*src++;
        if(upper&&c>='a'&&c<='z')c-=32;
        if(!upper&&c>='A'&&c<='Z')c+=32;
        *dst++=(char)c;
    }
    *dst=0;
}
static int key_make(unsigned kind,const char *id,const char *market,const char *language,
    const char *url,char key[KEY_CAP],char normalized[X4_PRODUCT_ID_CAP])
{
    char m[3],l[24];
    if(!x4_product_id_valid(id)||!locale(market,3,0)||!locale(language,sizeof(l),1))return 0;
    if(kind==KIND_IMAGE&&(!url||!x4_http_public_image_url(url)))return 0;
    canonical(normalized,id,1);canonical(m,market,1);canonical(l,language,0);
    int n=snprintf(key,KEY_CAP,"%u|%s|%s|%s|%s",kind,normalized,m,l,kind==KIND_IMAGE?url:"");
    return n>0&&(unsigned)n<KEY_CAP;
}
static void file_name(const char *key,char name[FILE_CAP])
{
    uint64_t a=UINT64_C(14695981039346656037),b=UINT64_C(7809847782465536322);
    for(const unsigned char *s=(const unsigned char *)key;*s;++s){
        a=(a^*s)*UINT64_C(1099511628211);
        b=(b^*s)*UINT64_C(14029467366897019727);
    }
    snprintf(name,FILE_CAP,"x4c-%016llx%016llx.bin",(unsigned long long)a,(unsigned long long)b);
}
static int generated_name(const char *name,int *temporary)
{
    if(strlen(name)!=40||strncmp(name,"x4c-",4))return 0;
    for(unsigned i=4;i<36;++i)if(!((name[i]>='0'&&name[i]<='9')||(name[i]>='a'&&name[i]<='f')))return 0;
    if(!strcmp(name+36,".bin")){*temporary=0;return 1;}
    if(!strcmp(name+36,".tmp")){*temporary=1;return 1;}
    return 0;
}
#ifdef X4_DISK_PATH_MUTATIONS
typedef struct {
    uint64_t begin, elapsed;
    const char *failed;
    int error;
    int announce;
} NativeProbe;

static void probe_step(const NativeProbe *probe,const char *step,unsigned component,
    int flags,int rc,int error)
{
#if !X4_PUBLIC && (defined(X4_OPENORBIS) || defined(X4_DISK_CACHE_PROBE_LOG_TEST))
    if(probe&&probe->announce){
        int saved=errno;
        x4_log_printf("XCloud4: cache autoprueba paso=%s componente=%u flags=0x%x rc=%d errno=%d\n",
            step,component,(unsigned)flags,rc,error);
        errno=saved;
    }
#else
    (void)probe;(void)step;(void)component;(void)flags;(void)rc;(void)error;
#endif
}
static void probe_failure(NativeProbe *probe,const char *step,int error)
{
    if(probe&&!probe->failed){probe->failed=step;probe->error=error?error:EIO;}
}
static void probe_done(NativeProbe *probe,int enabled)
{
    if(!probe->elapsed){
        uint64_t end=monotonic_us();
        if(end>=probe->begin)probe->elapsed=end-probe->begin;
    }
#if !X4_PUBLIC && (defined(X4_OPENORBIS) || defined(X4_DISK_CACHE_PROBE_LOG_TEST))
    if(probe->announce)x4_log_printf("XCloud4: cache disco habilitada=%d paso_fallido=%s errno=%d autoprueba_us=%llu\n",
        enabled,probe->failed?probe->failed:"ninguno",probe->error,(unsigned long long)probe->elapsed);
#else
    (void)enabled;
#endif
}
static int unsupported_open_flag(int error)
{
    return error==EINVAL||error==ENOSYS||error==ENOTSUP||error==EOPNOTSUPP;
}
static int native_open(X4CatalogDiskCache *c,const char *path,int required,int directory,
    NativeProbe *probe,unsigned component,const char *step)
{
    int second=directory?O_DIRECTORY:0;
    int *unsupported=directory?&c->dir_flags_unsupported:&c->file_flags_unsupported;
    int optional=(O_CLOEXEC|second)&~*unsupported;
    const int choices[]={0,O_CLOEXEC,second,O_CLOEXEC|second};
    for(unsigned i=0;i<(directory?4u:2u);++i){
        int removed=choices[i];
        if(removed&~optional)continue;
        int flags=required|O_NOFOLLOW|(optional&~removed);
        /* Keep guarded record I/O blocking on regular files. A substituted
         * FIFO must still not block before fstat: FreeBSD/Linux O_RDWR does
         * not wait for an endpoint; O_WRONLY is only used to create new files. */
        if(directory&&!(flags&O_DIRECTORY))flags|=O_NONBLOCK;
        if(!directory&&(flags&O_ACCMODE)==O_RDONLY)
            flags=(flags&~O_ACCMODE)|O_RDWR;
        int fd=open(path,flags,0600);
        int saved=fd<0?errno:0;
        probe_step(probe,step,component,flags,fd<0?-1:0,saved);
        if(fd>=0){*unsupported|=removed;return fd;}
        errno=saved;
        if(!unsupported_open_flag(saved))return -1;
    }
    return -1;
}
static int native_file_type(int fd,struct stat *out,int directory)
{
    if(fstat(fd,out)<0)return -1;
    if(directory?!S_ISDIR(out->st_mode):!S_ISREG(out->st_mode)){
        errno=directory?ENOTDIR:EPERM;
        return -1;
    }
    return 0;
}
/* The SDK's lstat uses an ENOSYS fstatat stub. Walk the fixed path without
 * following symlinks and retain a validated directory descriptor. */
static int directory_open(X4CatalogDiskCache *c,const char *path,struct stat *out,
    NativeProbe *probe,unsigned component)
{
    int fd=native_open(c,path,O_RDONLY,1,probe,component,"directory_open");
    if(fd<0)return -1;
    int rc=native_file_type(fd,out,1);
    int saved=rc<0?errno:0;
    probe_step(probe,"directory_fstat",component,0,rc,saved);
    if(!rc)return fd;
    close(fd);errno=saved;return -1;
}
static void probe_directory_flags(const char *path,NativeProbe *probe,unsigned component)
{
    if(!probe)return;
    static const struct {const char *step;int flags;} tests[]={
        {"directory_O_DIRECTORY",O_DIRECTORY},
        {"directory_O_CLOEXEC",O_CLOEXEC|O_NONBLOCK},
        {"directory_O_NONBLOCK",O_NONBLOCK},
        {"directory_O_NOFOLLOW",O_DIRECTORY},
    };
    for(unsigned i=0;i<sizeof(tests)/sizeof(tests[0]);++i){
        /* Never try a combination that could block on a substituted FIFO. */
        int flags=O_RDONLY|O_NOFOLLOW|tests[i].flags;
        int fd=open(path,flags,0600);
        int saved=fd<0?errno:0;
        if(fd>=0){
            struct stat st;
            if(native_file_type(fd,&st,1)<0)saved=errno;
            close(fd);
        }
        probe_step(probe,tests[i].step,component,flags,saved?-1:0,saved);
    }
}
static int path_tree_open(X4CatalogDiskCache *c,int create,struct stat *out,NativeProbe *probe)
{
    char path[1024];size_t length=strlen(c->root);memcpy(path,c->root,length+1);
    unsigned component=0;
    for(size_t i=1;i<=length;++i)if(path[i]=='/'||path[i]==0){
        size_t start=i;while(start>0&&path[start-1]!='/')--start;
        size_t n=i-start;
        if(!n||n>255||(n==1&&path[start]=='.')||
           (n==2&&path[start]=='.'&&path[start+1]=='.')){errno=EINVAL;return -1;}
        for(size_t j=start;j<i;++j)if((unsigned char)path[j]<32||(unsigned char)path[j]==127){errno=EINVAL;return -1;}
        char end=path[i];path[i]=0;++component;
        int fd=directory_open(c,path,out,probe,component);
        if(fd<0&&errno==ENOENT&&create){
            int rc=mkdir(path,0700),saved=rc<0?errno:0;
            probe_step(probe,"mkdir",component,0,rc,saved);
            if(!rc||saved==EEXIST)fd=directory_open(c,path,out,probe,component);
            else errno=saved;
        }
        if(fd>=0)probe_directory_flags(path,probe,component);
        path[i]=end;
        if(fd<0||!end)return fd;
        close(fd);
    }
    errno=EINVAL;return -1;
}
static int guarded_directory_open(X4CatalogDiskCache *c)
{
    struct stat path;
    int fd=path_tree_open(c,0,&path,NULL);
    if(fd<0)return -1;
    const struct stat *held=&c->root_identity;
    if((!c->unstable_device&&path.st_dev!=held->st_dev)||
       (!c->unstable_inode&&path.st_ino!=held->st_ino)||
       ((c->unstable_device||c->unstable_inode)&&
        (path.st_mode!=held->st_mode||path.st_uid!=held->st_uid||path.st_gid!=held->st_gid))){
        close(fd);
        errno=EPERM;
        return -1;
    }
    return fd;
}
static int root_guard(X4CatalogDiskCache *c)
{
    int fd=guarded_directory_open(c);
    if(fd<0)return 0;
    close(fd);
    return 1;
}
#ifdef X4_DISK_RAW_SCAN
/* OpenOrbis getdents returns variable-length BSD records. A record can be
 * smaller than struct dirent, so validate its length before reading the name. */
enum { SCAN_DIRENT_HEADER=8, SCAN_DIRENT_BUFFER=4096 };
typedef struct {
    union {
        struct dirent aligned;
        unsigned char bytes[SCAN_DIRENT_BUFFER];
    } buffer;
    size_t next, count;
} NativeScan;
#ifdef X4_OPENORBIS
/* dirent.h aliases d_fileno to d_ino, while bits/dirent.h declares d_fileno. */
#undef d_fileno
_Static_assert(offsetof(struct dirent,d_fileno)==0&&offsetof(struct dirent,d_reclen)==4&&
    offsetof(struct dirent,d_namlen)==7&&offsetof(struct dirent,d_name)==SCAN_DIRENT_HEADER,
    "unexpected OpenOrbis getdents ABI");
#else
int x4_disk_cache_test_getdents(int fd,void *bytes,size_t capacity);
#endif
static int native_scan_next(int fd,NativeScan *scan,const char **name)
{
    for(;;){
        if(scan->next==scan->count){
            int n;
            do{
                errno=0;
#ifdef X4_OPENORBIS
                n=getdents(fd,&scan->buffer.aligned,sizeof(scan->buffer.bytes));
#else
                n=x4_disk_cache_test_getdents(fd,scan->buffer.bytes,sizeof(scan->buffer.bytes));
#endif
            }while(n<0&&errno==EINTR);
            if(n<0&&!errno)errno=EIO;
            if(n<=0)return n;
            if((size_t)n>sizeof(scan->buffer.bytes))break;
            scan->next=0;scan->count=(size_t)n;
        }
        size_t available=scan->count-scan->next;
        if(available<SCAN_DIRENT_HEADER)break;
        const unsigned char *entry=scan->buffer.bytes+scan->next;
        size_t size=entry[4]|((size_t)entry[5]<<8);
        if(size<SCAN_DIRENT_HEADER||(size&3)||size>available)break;
        scan->next+=size;
        if(!get32(entry))continue; /* Unused directory slot. */
        size_t length=entry[7];
        const unsigned char *text=entry+SCAN_DIRENT_HEADER;
        if(!length||length>=size-SCAN_DIRENT_HEADER||text[length]||
            memchr(text,0,length))break;
        *name=(const char *)text;
        return 1;
    }
    errno=EIO;
    return -1;
}
#endif
static int guarded_path(X4CatalogDiskCache *c,const char *name,char out[1080])
{
    if(!root_guard(c))return 0;
    size_t n=strlen(c->root);memcpy(out,c->root,n);out[n]='/';
    memcpy(out+n+1,name,strlen(name)+1);return 1;
}
static int owned_path(X4CatalogDiskCache *c,const char *name,char out[1080])
{
    int temporary;
    if(!generated_name(name,&temporary)){errno=EPERM;return 0;}
    return guarded_path(c,name,out);
}
#endif
static int record_open(X4CatalogDiskCache *c,const char *name,int flags)
{
#ifdef X4_DISK_PATH_MUTATIONS
    char path[1080];
    if(!owned_path(c,name,path))return -1;
    int fd=native_open(c,path,flags,0,NULL,0,NULL);
    if(fd<0)return -1;
    struct stat st;
    if(native_file_type(fd,&st,0)==0&&root_guard(c))return fd;
    int saved=errno;
    close(fd);errno=saved;return -1;
#else
    return openat(c->directory,name,flags|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC,0600);
#endif
}
static int record_stat(X4CatalogDiskCache *c,const char *name,struct stat *out)
{
#ifdef X4_DISK_PATH_MUTATIONS
    int fd=record_open(c,name,O_RDONLY);
    if(fd<0)return -1;
    int rc=fstat(fd,out);int saved=errno;close(fd);errno=saved;return rc;
#else
    return fstatat(c->directory,name,out,AT_SYMLINK_NOFOLLOW);
#endif
}
static int record_unlink(X4CatalogDiskCache *c,const char *name)
{
#ifdef X4_DISK_PATH_MUTATIONS
    char path[1080];if(!owned_path(c,name,path))return -1;
    int rc=unlink(path);if(!root_guard(c)){errno=EPERM;return -1;}return rc;
#else
    return unlinkat(c->directory,name,0);
#endif
}
static int record_rename(X4CatalogDiskCache *c,const char *from,const char *to)
{
#ifdef X4_DISK_PATH_MUTATIONS
    char source[1080],dest[1080];
    if(!owned_path(c,from,source)||!owned_path(c,to,dest))return -1;
    int rc=rename(source,dest);if(!root_guard(c)){errno=EPERM;return -1;}return rc;
#else
    return renameat(c->directory,from,c->directory,to);
#endif
}
#ifdef X4_DISK_PATH_MUTATIONS
static int native_futimes(int fd,uint64_t now)
{
    struct timeval times[2]={{(time_t)now,0},{(time_t)now,0}};
    return futimes(fd,times);
}
static int write_header_usage(X4CatalogDiskCache *c,int fd,uint64_t now)
{
    if(now>UINT32_MAX)return 0;
    unsigned char bytes[4];put32(bytes,(uint32_t)now);
    if(lseek(fd,USAGE_OFFSET,SEEK_SET)!=(off_t)USAGE_OFFSET)return -1;
    errno=0;
    if(exact_io(fd,bytes,sizeof(bytes),1,c,NULL,NULL)==1)return 0;
    if(!errno)errno=EIO;
    return -1;
}
static int native_probe_same_file(int fd,int other)
{
    struct stat held,opened;
    if(native_file_type(fd,&held,0)<0||native_file_type(other,&opened,0)<0)return 0;
    if(held.st_dev!=opened.st_dev||held.st_ino!=opened.st_ino){
        errno=EPERM;
        return 0;
    }
    return 1;
}
static int native_probe_owns_path(X4CatalogDiskCache *c,const char *path,int held)
{
    if(!root_guard(c))return 0;
    int fd=native_open(c,path,O_RDONLY,0,NULL,0,NULL);
    if(fd<0)return 0;
    int same=native_probe_same_file(held,fd),saved=errno;
    close(fd);
    errno=saved;
    return same&&root_guard(c);
}
static int native_probe_unlink(X4CatalogDiskCache *c,const char *path,int held)
{
    if(!native_probe_owns_path(c,path,held)||unlink(path)<0)return -1;
    return root_guard(c)?0:-1;
}
static int native_autoprobe(X4CatalogDiskCache *c,NativeProbe *probe)
{
    struct stat held;
    int rc=native_file_type(c->directory,&held,1),saved=rc<0?errno:0;
    probe_step(probe,"root_fstat",0,0,rc,saved);
    if(rc<0){probe_failure(probe,"root_fstat",saved);return 0;}
    /* Relax only a field that disagrees with itself on the same held fd.
     * A stable descriptor that disagrees with a reopened path still fails.
     * On an unstable filesystem, revalidate all components and retain the
     * stable identity fields and the final directory's mode/owner checks. */
    c->unstable_device=held.st_dev!=c->root_identity.st_dev;
    c->unstable_inode=held.st_ino!=c->root_identity.st_ino;
    int stable=!c->unstable_device&&!c->unstable_inode;
    probe_step(probe,"root_dev_ino_stable",0,0,stable?0:-1,0);
    rc=root_guard(c)?0:-1;saved=rc<0?errno:0;
    probe_step(probe,"root_guard",0,0,rc,saved);
    if(rc<0){probe_failure(probe,"root_guard",saved);return 0;}

    char source[FILE_CAP],target[FILE_CAP],src_path[1080],dst_path[1080];
    const char *step="create";
    int source_fd=-1,target_fd=-1,read_fd=-1,source_owned=0,target_owned=0,ok=0;
    unsigned char sample[HEADER_SIZE]="X4CACHE-PROBE",received[HEADER_SIZE];
    (void)snprintf(source,sizeof(source),"x4-cache-selftest-%016llx.tmp",(unsigned long long)probe->begin);
    (void)snprintf(target,sizeof(target),"x4-cache-selftest-%016llx.bin",(unsigned long long)probe->begin);
    if(!guarded_path(c,source,src_path)){
        probe_step(probe,step,0,0,-1,errno);
        goto done;
    }
    source_fd=native_open(c,src_path,O_RDWR|O_CREAT|O_EXCL,0,probe,0,step);
    if(source_fd<0)goto done;
    source_owned=1;
    step="create_fstat";
    rc=native_file_type(source_fd,&held,0);
    if(!rc&&!root_guard(c))rc=-1;
    saved=rc<0?errno:0;
    probe_step(probe,step,0,0,rc,saved);
    if(rc<0){errno=saved;goto done;}

    step="write";
    errno=0;
    rc=exact_io(source_fd,sample,sizeof(sample),1,c,NULL,NULL)==1?0:-1;
    saved=rc<0?(errno?errno:EIO):0;
    probe_step(probe,step,0,0,rc,saved);
    if(rc<0){errno=saved;goto done;}
    step="fsync";
    rc=fsync(source_fd);saved=rc<0?errno:0;
    probe_step(probe,step,0,0,rc,saved);
    if(rc<0){errno=saved;goto done;}

    step="reserve_rename";
    if(!guarded_path(c,target,dst_path)){
        probe_step(probe,step,0,0,-1,errno);
        goto done;
    }
    target_fd=native_open(c,dst_path,O_RDWR|O_CREAT|O_EXCL,0,probe,0,step);
    if(target_fd<0)goto done;
    target_owned=1;
    step="rename";
    rc=-1;
    if(native_probe_owns_path(c,src_path,source_fd)&&native_probe_owns_path(c,dst_path,target_fd)){
        rc=rename(src_path,dst_path);
        if(!rc){
            source_owned=0;
            if(!root_guard(c))rc=-1;
        }
    }
    saved=rc<0?errno:0;
    probe_step(probe,step,0,0,rc,saved);
    if(rc<0){errno=saved;goto done;}

    step="futimes";
    rc=native_futimes(source_fd,utc_clock(NULL));saved=rc<0?errno:0;
    probe_step(probe,step,0,0,rc,saved);
    if(rc<0){
        c->header_usage=1;
        step="header_usage";
        rc=write_header_usage(c,source_fd,1);saved=rc<0?errno:0;
        probe_step(probe,step,0,0,rc,saved);
        if(rc<0){errno=saved;goto done;}
        put32(sample+USAGE_OFFSET,1);
    }

    step="reopen";
    if(!root_guard(c)){
        probe_step(probe,step,0,0,-1,errno);
        goto done;
    }
    read_fd=native_open(c,dst_path,O_RDONLY,0,probe,0,step);
    if(read_fd<0)goto done;
    step="reopen_fstat";
    rc=native_probe_same_file(source_fd,read_fd)&&root_guard(c)?0:-1;
    saved=rc<0?errno:0;
    probe_step(probe,step,0,0,rc,saved);
    if(rc<0){errno=saved;goto done;}
    step="read";
    errno=0;
    rc=exact_io(read_fd,received,sizeof(received),0,c,NULL,NULL)==1&&
        !memcmp(received,sample,sizeof(sample))?0:-1;
    saved=rc<0?(errno?errno:EIO):0;
    probe_step(probe,step,0,0,rc,saved);
    if(rc<0){errno=saved;goto done;}
    ok=1;
done:
    if(!ok)probe_failure(probe,step,errno);
    if(source_owned){
        rc=native_probe_unlink(c,src_path,source_fd);saved=rc<0?errno:0;
        probe_step(probe,"unlink",0,0,rc,saved);
        if(rc<0){probe_failure(probe,"unlink",saved);ok=0;}
    }
    if(target_owned){
        int held_target=source_owned?target_fd:source_fd;
        rc=native_probe_unlink(c,dst_path,held_target);saved=rc<0?errno:0;
        probe_step(probe,"unlink",0,0,rc,saved);
        if(rc<0){probe_failure(probe,"unlink",saved);ok=0;}
    }
    if(read_fd>=0)close(read_fd);
    if(target_fd>=0)close(target_fd);
    if(source_fd>=0)close(source_fd);
    return ok;
}
#endif
static int record_touch(X4CatalogDiskCache *c,int fd,uint64_t now)
{
#ifdef X4_DISK_PATH_MUTATIONS
    if(!c->header_usage){
        if(native_futimes(fd,now)==0)return 0;
        c->header_usage=1;
    }
    return write_header_usage(c,fd,now);
#else
    (void)c;
    struct timespec times[2]={{(time_t)now,0},{(time_t)now,0}};return futimens(fd,times);
#endif
}
static int utf8_display(const char *s,size_t cap)
{
    size_t end=0;while(end<cap&&s[end])++end;if(end==cap)return 0;
    for(size_t i=0;i<end;){
        unsigned char a=(unsigned char)s[i++];
        if(a<128){if(a<32||a==127)return 0;continue;}
        unsigned count;uint32_t value;
        if(a>=0xc2&&a<=0xdf){count=1;value=a&31;}
        else if(a>=0xe0&&a<=0xef){count=2;value=a&15;}
        else if(a>=0xf0&&a<=0xf4){count=3;value=a&7;}else return 0;
        if(end-i<count)return 0;
        for(unsigned k=0;k<count;++k){unsigned char b=(unsigned char)s[i++];if((b&0xc0)!=0x80)return 0;value=(value<<6)|(b&63);}
        if((count==1&&value<128)||(count==2&&value<2048)||(count==3&&value<65536)||
            value>0x10ffff||(value>=0xd800&&value<=0xdfff))return 0;
    }
    return 1;
}
static int metadata_normalize(const X4ProductMetadata *in,const char *id,X4ProductMetadata *out)
{
    char normalized[X4_PRODUCT_ID_CAP];
    if(!in||!x4_product_id_valid(in->product_id))return 0;
    canonical(normalized,in->product_id,1);if(strcmp(normalized,id))return 0;
    if(!utf8_display(in->name,sizeof(in->name))||!utf8_display(in->publisher,sizeof(in->publisher))||
        !utf8_display(in->developer,sizeof(in->developer))||!utf8_display(in->category,sizeof(in->category))||
        !utf8_display(in->description,sizeof(in->description)))return 0;
    size_t n=0;while(n<sizeof(in->cover_url)&&in->cover_url[n])++n;
    if(n==sizeof(in->cover_url)||(n&&!x4_http_public_image_url(in->cover_url)))return 0;
    size_t hn=0;while(hn<sizeof(in->hero_url)&&in->hero_url[hn])++hn;
    if(hn==sizeof(in->hero_url)||(hn&&!x4_http_public_image_url(in->hero_url)))return 0;
    size_t rn=0;while(rn<sizeof(in->recent_url)&&in->recent_url[rn])++rn;
    if(rn==sizeof(in->recent_url)||(rn&&!x4_http_public_image_url(in->recent_url)))return 0;
    memset(out,0,sizeof(*out));
    memcpy(out->product_id,id,strlen(id)+1);
#define COPY_FIELD(f) memcpy(out->f,in->f,strlen(in->f)+1)
    COPY_FIELD(name);COPY_FIELD(publisher);COPY_FIELD(developer);COPY_FIELD(category);
    COPY_FIELD(description);COPY_FIELD(cover_url);COPY_FIELD(hero_url);COPY_FIELD(recent_url);
#undef COPY_FIELD
    return 1;
}
static unsigned be16(const unsigned char *p){return ((unsigned)p[0]<<8)|p[1];}
static uint32_t be32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int image_valid(const unsigned char *p,size_t n)
{
    if(!p||n<10||n>X4_HTTP_IMAGE_MAX)return 0;
    static const unsigned char png[]={137,80,78,71,13,10,26,10};
    if(n>=33&&!memcmp(p,png,8)){
        if(be32(p+8)!=13||memcmp(p+12,"IHDR",4))return 0;
        uint32_t w=be32(p+16),h=be32(p+20);
        return w>0&&h>0&&w<=2048&&h<=2048;
    }
    if(p[0]!=255||p[1]!=216)return 0;
    size_t offset=2;
    while(offset+4<=n){
        if(p[offset++]!=255)return 0;
        while(offset<n&&p[offset]==255)++offset;
        if(offset>=n)return 0;
        unsigned marker=p[offset++];
        if(marker==0xd9||marker==0xda)return 0;
        if(marker==0x01||(marker>=0xd0&&marker<=0xd8))continue;
        if(offset+2>n)return 0;
        unsigned len=be16(p+offset);if(len<2||len>n-offset)return 0;
        if((marker>=0xc0&&marker<=0xc3)||(marker>=0xc5&&marker<=0xc7)||
            (marker>=0xc9&&marker<=0xcb)||(marker>=0xcd&&marker<=0xcf)){
            if(len<8)return 0;
            unsigned h=be16(p+offset+3),w=be16(p+offset+5);
            return w>0&&h>0&&w<=2048&&h<=2048;
        }
        offset+=len;
    }
    return 0;
}
static Product *product_slot(X4CatalogDiskCache *c,const char *id,int add)
{
    Product *free_slot=NULL;
    for(unsigned i=0;i<=c->max_products;++i){
        Product *p=&c->product[i];
        if(p->refs){
            if(!strcmp(p->id,id))return p;
        }else if(!free_slot)free_slot=p;
    }
    return add?free_slot:NULL;
}
static int assign_record(X4CatalogDiskCache *c,unsigned index,const Record *r)
{
    Record *slot=&c->record[index];
    if(slot->occupied){
        /* Replacement uses exactly the same full key and product. */
        c->stats.bytes-=slot->size;
    }else{
        Product *p=product_slot(c,r->id,1);
        if(!p)return 0;
        if(!p->refs){
            memcpy(p->id,r->id,strlen(r->id)+1);
            ++c->stats.products;
        }
        ++p->refs;
        ++c->stats.records;
    }
    c->stats.bytes+=r->size;
    *slot=*r;
    while(c->first_free<c->capacity&&c->record[c->first_free].occupied)++c->first_free;
    return 1;
}
static void forget_record(X4CatalogDiskCache *c,unsigned index)
{
    Record *r=&c->record[index];
    Product *p=product_slot(c,r->id,0);
    if(p&&!--p->refs)--c->stats.products;
    c->stats.bytes-=r->size;
    --c->stats.records;
    if(index<c->first_free)c->first_free=index;
    memset(r,0,sizeof(*r));
}
static void remember_miss(X4CatalogDiskCache *c,const char *name,MissReason reason)
{
    if(reason==MISS_NONE)return;
    MissNote *note=&c->misses[c->next_miss_note];
    c->next_miss_note=(c->next_miss_note+1)%MISS_NOTES;
    memcpy(note->name,name,FILE_CAP);
    note->reason=reason;
}
static MissReason prior_miss(X4CatalogDiskCache *c,const char *key)
{
    char name[FILE_CAP];file_name(key,name);
    unsigned index=c->next_miss_note;
    for(unsigned i=0;i<MISS_NOTES;++i){
        index=(index+MISS_NOTES-1)%MISS_NOTES;
        const MissNote *note=&c->misses[index];
        if(note->reason!=MISS_NONE&&!strcmp(note->name,name))return note->reason;
    }
    return MISS_ABSENT;
}
static void count_miss(X4CatalogDiskCache *c,MissReason reason)
{
    if(!c)return;
    ++c->stats.misses;
    switch(reason){
    case MISS_ABSENT: ++c->stats.miss_absent;break;
    case MISS_EXPIRED: ++c->stats.miss_expired;break;
    case MISS_CORRUPT: ++c->stats.miss_corrupt;break;
    case MISS_KEY: ++c->stats.miss_key;break;
    case MISS_EVICTED: ++c->stats.miss_evicted;break;
    case MISS_FILTERED: ++c->stats.miss_filtered;break;
    case MISS_NONE: break;
    }
}
static int protected_product(const X4CatalogDiskCache *c,const char *id)
{
    for(unsigned i=0;i<c->protected_count;++i)
        if(!strcmp(c->protected_id[i],id))return 1;
    return 0;
}
void x4_catalog_disk_cache_protect_products(X4CatalogDiskCache *c,
    const char (*ids)[X4_PRODUCT_ID_CAP],unsigned count)
{
    if(!c)return;
    c->protected_count=0;
    for(unsigned i=0;ids&&i<count&&i<X4_CATALOG_DISK_PROTECTED;++i){
        if(!memchr(ids[i],0,X4_PRODUCT_ID_CAP)||!x4_product_id_valid(ids[i]))continue;
        char id[X4_PRODUCT_ID_CAP];canonical(id,ids[i],1);
        if(!protected_product(c,id))memcpy(c->protected_id[c->protected_count++],id,strlen(id)+1);
    }
}
static int erase_file(X4CatalogDiskCache *c,const char *name)
{
    struct stat s;
    if(record_stat(c,name,&s)<0)return errno==ENOENT?1:0;
    if(!S_ISREG(s.st_mode))return 0;
    return record_unlink(c,name)==0||errno==ENOENT;
}
static void remove_record(X4CatalogDiskCache *c,unsigned index,MissReason reason)
{
    Record *r=&c->record[index];if(!r->occupied)return;
    if(!erase_file(c,r->name)){++c->stats.io_errors;return;}
    remember_miss(c,r->name,reason);
    forget_record(c,index);
    if(reason==MISS_EVICTED)++c->stats.evictions;
    if(reason==MISS_EXPIRED)++c->stats.expired;
    if(reason==MISS_CORRUPT)++c->stats.corrupt;
}
static int oldest(const X4CatalogDiskCache *c,int except)
{
    int best=-1;
    for(unsigned i=0;i<c->capacity;++i){
        const Record *r=&c->record[i];
        if(!r->occupied||(int)i==except||protected_product(c,r->id))continue;
        if(best<0||r->used<c->record[best].used||
            (r->used==c->record[best].used&&r->order<c->record[best].order))best=(int)i;
    }
    return best;
}
static int find(const X4CatalogDiskCache *c,const char *key)
{
    for(unsigned i=0;i<c->capacity;++i)if(c->record[i].occupied&&!strcmp(c->record[i].key,key))return (int)i;
    return -1;
}
static int header_read(X4CatalogDiskCache *c,int fd,Record *r,uint32_t *payload_size,uint32_t *checksum,
    ReadCheck *check)
{
    unsigned char h[HEADER_SIZE];struct stat st;
    *check=(ReadCheck){.stat_size=-1,.file_size=-1,.expected=sizeof(h)};
    if(fstat(fd,&st)<0)return reject_record(check,CHECK_FSTAT,errno?errno:EIO);
    check->stat_size=(int64_t)st.st_size;
    if(!S_ISREG(st.st_mode))return reject_record(check,CHECK_FILE_TYPE,0);
#ifdef X4_DISK_PATH_MUTATIONS
    /* Native PS4 fstat reports an allocation-like st_size for files
     * on /data. Keep it for diagnostics, and verify the actual byte
     * length from the open regular descriptor before trusting any record. */
    off_t length=lseek(fd,0,SEEK_END);
    if(length<0||lseek(fd,0,SEEK_SET)!=0)return reject_record(check,CHECK_FILE_SEEK,errno?errno:EIO);
#else
    off_t length=st.st_size;
#endif
    check->file_size=(int64_t)length;
    if(length<HEADER_SIZE)return reject_record(check,CHECK_HEADER_SIZE,0);
    if(exact_io(fd,h,sizeof(h),0,c,NULL,&check->transferred)!=1)return reject_record(check,CHECK_HEADER_READ,errno);
    uint32_t kn=check->kn=get32(h+24),pn=check->pn=get32(h+28),kind=get32(h+12);
    if(memcmp(h,"X4PUBLIC",8))return reject_record(check,CHECK_MAGIC,0);
    if(get32(h+8)!=1)return reject_record(check,CHECK_VERSION,0);
    if(kind!=KIND_METADATA&&kind!=KIND_IMAGE)return reject_record(check,CHECK_KIND,0);
    if(get32(h+40)!=(crc(c,UINT32_MAX,h,40)^UINT32_MAX))return reject_record(check,CHECK_HEADER_CRC,0);
    if(!kn||kn>=KEY_CAP)return reject_record(check,CHECK_KEY_SIZE,0);
    if(!pn||pn>X4_HTTP_IMAGE_MAX)return reject_record(check,CHECK_PAYLOAD_SIZE,0);
    if((uint64_t)length!=HEADER_SIZE+(uint64_t)kn+pn)return reject_record(check,CHECK_FILE_SIZE,0);
    if(kind==KIND_METADATA&&pn!=sizeof(X4ProductMetadata))return reject_record(check,CHECK_METADATA_SIZE,0);
    memset(r,0,sizeof(*r));
    check->expected=kn;
    if(exact_io(fd,r->key,kn,0,c,NULL,&check->transferred)!=1)return reject_record(check,CHECK_KEY_READ,errno);
    if(memchr(r->key,0,kn))return reject_record(check,CHECK_KEY_NUL,0);
    r->key[kn]=0;r->kind=kind;r->created=get64(h+16);
    if(get32(h+36)!=(crc(c,UINT32_MAX,r->key,kn)^UINT32_MAX))return reject_record(check,CHECK_KEY_CRC,0);
    char key[KEY_CAP],id[X4_PRODUCT_ID_CAP],market[3],lang[24];
    /* Parse the stored key through the same public input policy. */
    const char *a=strchr(r->key,'|'),*b=a?strchr(a+1,'|'):NULL,
        *d=b?strchr(b+1,'|'):NULL,*e=d?strchr(d+1,'|'):NULL;
    if(!a||!b||!d||!e||a-r->key!=1||r->key[0]!=(char)('0'+r->kind)||
        b-a-1<=0||b-a-1>=X4_PRODUCT_ID_CAP||d-b-1!=2||e-d-1<=0||e-d-1>=(ptrdiff_t)sizeof(lang))
        return reject_record(check,CHECK_KEY_FIELDS,0);
    memcpy(id,a+1,(size_t)(b-a-1));id[b-a-1]=0;
    memcpy(market,b+1,2);market[2]=0;memcpy(lang,d+1,(size_t)(e-d-1));lang[e-d-1]=0;
    char normalized[X4_PRODUCT_ID_CAP];
    if((r->kind==KIND_METADATA&&e[1])||!key_make(r->kind,id,market,lang,e+1,key,normalized)||strcmp(key,r->key))
        return reject_record(check,CHECK_KEY_POLICY,0);
    memcpy(r->id,normalized,strlen(normalized)+1);r->size=(size_t)length;
    r->used=st.st_mtime>0?(uint64_t)st.st_mtime:r->created;
    uint64_t saved_use=get32(h+USAGE_OFFSET);
    if(saved_use&&saved_use>=r->created&&saved_use<=c->clock(c->clock_context)){
#ifdef X4_DISK_PATH_MUTATIONS
        if(c->header_usage||saved_use>r->used)r->used=saved_use;
#else
        if(saved_use>r->used)r->used=saved_use;
#endif
    }
    r->occupied=1;
    *payload_size=pn;*checksum=get32(h+32);return 1;
}
static int expired(const Record *r,uint64_t now)
{
    uint64_t ttl=r->kind==KIND_METADATA?X4_CATALOG_DISK_METADATA_TTL:X4_CATALOG_DISK_IMAGE_TTL;
    return !now||!r->created||r->created>now||now-r->created>=ttl;
}
static int prune(X4CatalogDiskCache *c,int except)
{
    while(c->stats.bytes>c->budget||c->stats.products>c->max_products){
        int v=oldest(c,except);if(v<0)return 0;
        unsigned before=c->stats.records;remove_record(c,(unsigned)v,MISS_EVICTED);
        if(c->stats.records==before)return 0;
    }
    return 1;
}
#ifndef X4_DISK_PATH_MUTATIONS
static int open_directory_tree(const char *root)
{
    if(!root||root[0]!='/'||strlen(root)>1023||root[1]==0)return -1;
    int fd=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    const char *at=root+1;
    while(*at){
        size_t n=strcspn(at,"/");char name[256];
        if(!n||n>=sizeof(name)||(n==1&&at[0]=='.')||(n==2&&at[0]=='.'&&at[1]=='.')){close(fd);return -1;}
        memcpy(name,at,n);name[n]=0;
        for(size_t i=0;i<n;++i)if((unsigned char)name[i]<32||(unsigned char)name[i]==127){close(fd);return -1;}
        if(mkdirat(fd,name,0700)<0&&errno!=EEXIST){close(fd);return -1;}
        int next=openat(fd,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(fd);if(next<0)return -1;fd=next;
        at+=n;if(*at=='/'){++at;if(!*at){close(fd);return -1;}}
    }
    return fd;
}
#endif
static X4CatalogDiskCache *cache_open_internal(const char *root,size_t budget,unsigned products,
    X4CatalogDiskClock clock,void *context,const struct stat *private_identity);

#ifdef X4_DISK_PATH_MUTATIONS
static void roundtrip_png(X4CatalogDiskCache *c,unsigned char *image,size_t length)
{
    /* Complete one-pixel RGBA PNG with a checksummed 300 KiB test payload.
     * The private ancillary chunk does not alter the decoded pixel. */
    static const unsigned char prefix[]={
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,
        0x89,0x00,0x00,0x00,0x0d,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,0xdf,0xe0,0xf0,
        0x1f,0x00,0x07,0x00,0x02,0xbf,0x2b,0xd7,0xc7,0xe2
    };
    static const unsigned char suffix[]={0,0,0,0,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
    memcpy(image,prefix,sizeof(prefix));
    unsigned char *chunk=image+sizeof(prefix);
    size_t payload=length-sizeof(prefix)-sizeof(suffix)-12u;
    memcpy(chunk+4,"raNd",4);
    for(size_t i=0;i<payload;++i)chunk[8+i]=(unsigned char)(i^(i>>8));
    uint32_t sum=crc(c,UINT32_MAX,chunk+4,4+payload)^UINT32_MAX;
    for(unsigned i=0;i<4;++i){
        chunk[i]=(unsigned char)(payload>>(24-8*i));
        chunk[8+payload+i]=(unsigned char)(sum>>(24-8*i));
    }
    memcpy(image+length-sizeof(suffix),suffix,sizeof(suffix));
}
static int roundtrip_identity(X4CatalogDiskCache *outer,const char *path,const struct stat *expected)
{
    struct stat st;
    if(!root_guard(outer))return 0;
    int fd=directory_open(outer,path,&st,NULL,0);
    if(fd<0)return 0;
    close(fd);
    if(st.st_dev==expected->st_dev&&st.st_ino==expected->st_ino&&root_guard(outer))return 1;
    errno=EPERM;return 0;
}
static int roundtrip_cleanup(X4CatalogDiskCache *outer,const char *path,const struct stat *identity,
    char names[2][FILE_CAP])
{
    for(unsigned i=0;i<2;++i)for(unsigned temporary=0;temporary<2;++temporary){
        if(temporary)memcpy(names[i]+36,".tmp",5);
        if(!roundtrip_identity(outer,path,identity))return 0;
        char file[1080];int n=snprintf(file,sizeof(file),"%s/%s",path,names[i]);
        if(n<0||(size_t)n>=sizeof(file)){errno=ENAMETOOLONG;return 0;}
        int fd=native_open(outer,file,O_RDWR,0,NULL,0,NULL);
        if(fd<0){if(errno==ENOENT)continue;return 0;}
        int rc=native_probe_unlink(outer,file,fd),error=errno;
        close(fd);errno=error;
        if(rc<0)return 0;
    }
    return roundtrip_identity(outer,path,identity)&&rmdir(path)==0&&root_guard(outer);
}
static int roundtrip_read(X4CatalogDiskCache *c,const X4ProductMetadata *sample,
    const unsigned char *image,unsigned char *result,size_t bytes,const char **step)
{
    X4ProductMetadata returned;
    *step="roundtrip_get_metadata";
    if(x4_catalog_disk_cache_get_metadata(c,sample->product_id,"US","en-US",&returned,NULL)!=1||
        memcmp(&returned,sample,sizeof(returned)))return 0;
    size_t length;
    *step="roundtrip_get_imagen";
    return x4_catalog_disk_cache_get_image(c,sample->product_id,"US","en-US",
        sample->cover_url,result,bytes,&length,NULL)==1&&length==bytes&&!memcmp(image,result,bytes);
}
static int native_roundtrip(X4CatalogDiskCache *outer,NativeProbe *probe,char failure[64])
{
    enum { IMAGE_BYTES=300*1024 };
    const char *step="roundtrip_namespace";
    char name[FILE_CAP],path[1080],names[2][FILE_CAP],key[KEY_CAP],id[X4_PRODUCT_ID_CAP];
    X4ProductMetadata sample={.product_id="X4PROBE",.name="XCloud4 test",
        .cover_url="https://store-images.s-microsoft.com/image/apps.cache-probe?w=356&h=200&q=85&format=jpg"};
    (void)snprintf(name,sizeof(name),"x4-cache-roundtrip-%016llx",(unsigned long long)probe->begin);
    if(!key_make(KIND_METADATA,sample.product_id,"US","en-US",NULL,key,id))goto setup_failed;
    file_name(key,names[0]);
    if(!key_make(KIND_IMAGE,sample.product_id,"US","en-US",sample.cover_url,key,id))goto setup_failed;
    file_name(key,names[1]);
    if(!guarded_path(outer,name,path))goto setup_failed;
    if(strlen(path)>1023){errno=ENAMETOOLONG;goto setup_failed;}
    if(mkdir(path,0700)<0)goto setup_failed;
    struct stat identity;
    int owned=directory_open(outer,path,&identity,NULL,0);
    if(owned<0){
        int saved=errno;
        /* Creation above was exclusive, and an empty-directory removal
         * cannot follow a substituted symlink or erase any contents. */
        if(root_guard(outer))(void)rmdir(path);
        errno=saved;
        goto setup_failed;
    }
    X4CatalogDiskCache *c=NULL;
    unsigned char *memory=NULL;
    int ok=0,error=0;
    step="roundtrip_open";
    if(!root_guard(outer))goto done;
    c=cache_open_internal(path,1024u*1024u,1,outer->clock,outer->clock_context,&identity);
    if(!c)goto done;
    step="roundtrip_alloc";
    memory=malloc(2u*IMAGE_BYTES);
    if(!memory)goto done;
    roundtrip_png(outer,memory,IMAGE_BYTES);
    step="roundtrip_put_metadata";
    errno=0;
    if(x4_catalog_disk_cache_put_metadata(c,sample.product_id,"US","en-US",&sample,NULL)!=1)goto done;
    step="roundtrip_put_imagen";
    errno=0;
    if(x4_catalog_disk_cache_put_image(c,sample.product_id,"US","en-US",sample.cover_url,
            memory,IMAGE_BYTES,NULL)!=1)goto done;
    errno=0;
    if(!roundtrip_read(c,&sample,memory,memory+IMAGE_BYTES,IMAGE_BYTES,&step))goto done;
    x4_catalog_disk_cache_close(c);
    step="roundtrip_reopen";
    c=cache_open_internal(path,1024u*1024u,1,outer->clock,outer->clock_context,&identity);
    if(!c)goto done;
    errno=0;
    if(!roundtrip_read(c,&sample,memory,memory+IMAGE_BYTES,IMAGE_BYTES,&step))goto done;
    ok=1;
done:
    if(!ok){
        error=errno?errno:EIO;
        if(c&&c->last_check!=CHECK_OK){
            (void)snprintf(failure,64,"roundtrip_%s",check_name[c->last_check]);
            step=failure;
        }
    }
    x4_catalog_disk_cache_close(c);
    free(memory);
    if(!roundtrip_cleanup(outer,path,&identity,names)&&ok){
        step="roundtrip_cleanup";error=errno?errno:EIO;ok=0;
    }
    close(owned);
    probe_step(probe,"roundtrip",0,0,ok?0:-1,error);
    if(!ok)probe_failure(probe,step,error);
    return ok;
setup_failed:
    probe_step(probe,"roundtrip",0,0,-1,errno?errno:EIO);
    probe_failure(probe,step,errno?errno:EIO);
    return 0;
}
#endif

static X4CatalogDiskCache *cache_open_internal(const char *root,size_t budget,unsigned products,
    X4CatalogDiskClock clock,void *context,const struct stat *private_identity)
{
    if(!root)return NULL;
    uint64_t start=monotonic_us();
    if(!budget)budget=X4_CATALOG_DISK_BUDGET;
    if(!products)products=X4_CATALOG_DISK_PRODUCTS;
#ifdef X4_DISK_PATH_MUTATIONS
    NativeProbe probe={.begin=start,.announce=!private_identity};
    int valid_path=root[0]=='/'&&root[1]&&strlen(root)<=1023;
#ifdef X4_OPENORBIS
    valid_path=valid_path&&(private_identity||!strcmp(root,X4_CATALOG_DISK_ROOT));
#endif
    if(!valid_path||budget>X4_CATALOG_DISK_BUDGET||products>X4_CATALOG_DISK_PRODUCTS){
        probe_failure(&probe,valid_path?"cache_limits":"root_path",EINVAL);
        probe_done(&probe,0);
        return NULL;
    }
    X4CatalogDiskCache *c=calloc(1,sizeof(*c));
    if(!c){
        probe_failure(&probe,"cache_alloc",ENOMEM);
        probe_done(&probe,0);
        return NULL;
    }
    memcpy(c->root,root,strlen(root)+1);
    int fd=path_tree_open(c,!private_identity,&c->root_identity,&probe);
    c->directory=fd;
    if(fd<0)probe_failure(&probe,"directory_open",errno);
    if(fd<0||(private_identity&&(c->root_identity.st_dev!=private_identity->st_dev||
            c->root_identity.st_ino!=private_identity->st_ino))||(!private_identity&&!native_autoprobe(c,&probe))){
        probe_done(&probe,0);
        x4_catalog_disk_cache_close(c);
        return NULL;
    }
#else
    (void)private_identity;
    if(budget>X4_CATALOG_DISK_BUDGET||products>X4_CATALOG_DISK_PRODUCTS)return NULL;
    int fd=open_directory_tree(root);if(fd<0)return NULL;
    X4CatalogDiskCache *c=calloc(1,sizeof(*c));if(!c){close(fd);return NULL;}
    c->directory=fd;memcpy(c->root,root,strlen(root)+1);
#endif
    c->budget=budget;c->max_products=products;c->capacity=products*RECORDS_PER_PRODUCT;
    if(c->capacity>X4_CATALOG_DISK_RECORDS)c->capacity=X4_CATALOG_DISK_RECORDS;
    c->clock=clock?clock:utc_clock;c->clock_context=context;
    for(unsigned i=0;i<256;++i){uint32_t v=i;for(unsigned k=0;k<8;++k)v=(v>>1)^((v&1)?UINT32_C(0xedb88320):0);c->crc_table[i]=v;}
#ifdef X4_DISK_PATH_MUTATIONS
    char failure[64];
    if(!private_identity&&!native_roundtrip(c,&probe,failure)){
        probe_done(&probe,0);
        x4_catalog_disk_cache_close(c);
        return NULL;
    }
    uint64_t scan_start=monotonic_us();
    if(scan_start>=start)probe.elapsed=scan_start-start;
#else
    uint64_t scan_start=start;
#endif
#ifdef X4_DISK_PATH_MUTATIONS
    int scan_fd=guarded_directory_open(c);
#else
    int scan_fd=openat(fd,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
#endif
#ifdef X4_DISK_RAW_SCAN
    NativeScan scan={0};
    int scan_opened=scan_fd>=0;
#else
    DIR *dir=scan_fd<0?NULL:fdopendir(scan_fd);
    int scan_opened=dir!=NULL;
#endif
#ifdef X4_DISK_PATH_MUTATIONS
    int scan_error=scan_opened?0:errno,read_error=0;
    probe_step(&probe,"scan_open",0,0,scan_opened?0:-1,scan_error);
#endif
    if(!scan_opened){
#ifdef X4_DISK_PATH_MUTATIONS
        probe_failure(&probe,"scan_open",scan_error);
        probe_done(&probe,0);
#endif
        if(scan_fd>=0)close(scan_fd);
        x4_catalog_disk_cache_close(c);
        return NULL;
    }
    uint64_t now=c->clock(context);unsigned scanned=0;int safe=1;
    if(!now){
#ifdef X4_DISK_PATH_MUTATIONS
        probe_failure(&probe,"clock",EIO);
        probe_done(&probe,0);
#endif
#ifdef X4_DISK_RAW_SCAN
        close(scan_fd);
#else
        closedir(dir);
#endif
        x4_catalog_disk_cache_close(c);return NULL;
    }
    for(;;){
        const char *entry_name;
#ifdef X4_DISK_RAW_SCAN
        int read_result=native_scan_next(scan_fd,&scan,&entry_name);
        if(read_result<=0){
            if(read_result<0){
                read_error=scan_error=errno;
                ++c->stats.io_errors;safe=0;
            }
            break;
        }
#else
        errno=0;
        struct dirent *entry=readdir(dir);
        if(!entry){
            if(errno){
#ifdef X4_DISK_PATH_MUTATIONS
                read_error=scan_error=errno;
#endif
                ++c->stats.io_errors;safe=0;
            }
            break;
        }
        entry_name=entry->d_name;
#endif
        if(!strcmp(entry_name,".")||!strcmp(entry_name,".."))continue;
        if(++scanned>SCAN_BOUND){
#ifdef X4_DISK_PATH_MUTATIONS
            scan_error=EOVERFLOW;
#endif
            ++c->stats.scan_omitted;safe=0;break;
        }
        int temporary;if(!generated_name(entry_name,&temporary))continue;
        if(temporary){if(!erase_file(c,entry_name))++c->stats.io_errors;continue;}
        int file=record_open(c,entry_name,O_RDONLY);
        if(file<0){
            int reason=errno;
            if(reason==ENOENT)remember_miss(c,entry_name,MISS_ABSENT);
            else{
                remember_miss(c,entry_name,MISS_FILTERED);
                ++c->stats.io_errors;
            }
            continue;
        }
        Record r;uint32_t size,sum;ReadCheck check;
        int valid=header_read(c,file,&r,&size,&sum,&check);
        close(file);
        MissReason reason=MISS_NONE;
        if(!valid){
            note_rejection(c,"scan",&check);
            if(check.error){
                remember_miss(c,entry_name,MISS_FILTERED);
                ++c->stats.io_errors;continue;
            }
            reason=MISS_CORRUPT;
        }
        else{
            char name[FILE_CAP];file_name(r.key,name);
            if(strcmp(name,entry_name)){
                check.failed=CHECK_FILENAME;note_rejection(c,"scan",&check);reason=MISS_KEY;
            }
            else if(expired(&r,now))reason=MISS_EXPIRED;
        }
        if(reason!=MISS_NONE){
            remember_miss(c,entry_name,reason);
            if(erase_file(c,entry_name)){
                if(reason==MISS_CORRUPT)++c->stats.corrupt;
                if(reason==MISS_EXPIRED)++c->stats.expired;
            }else ++c->stats.io_errors;
            continue;
        }
        memcpy(r.name,entry_name,FILE_CAP);r.order=++c->order;
        int slot=c->first_free<c->capacity?(int)c->first_free:-1;
        if(slot<0){
            slot=oldest(c,-1);
            if(slot<0||r.used<c->record[slot].used){
                if(!erase_file(c,r.name))safe=0;
                else{remember_miss(c,r.name,MISS_EVICTED);++c->stats.evictions;}
                continue;
            }
            remove_record(c,(unsigned)slot,MISS_EVICTED);
            if(c->record[slot].occupied){safe=0;break;}
        }
        if(!assign_record(c,(unsigned)slot,&r)||!prune(c,-1)){safe=0;break;}
    }
#ifdef X4_DISK_PATH_MUTATIONS
    probe_step(&probe,"scan_read",0,0,read_error?-1:0,read_error);
    if(read_error)probe_failure(&probe,"scan_read",read_error);
    if(!safe&&!scan_error)scan_error=EIO;
    probe_step(&probe,"scan",0,0,safe?0:-1,scan_error);
    if(!safe)probe_failure(&probe,"scan",scan_error);
#ifdef X4_DISK_RAW_SCAN
    int close_rc=close(scan_fd);
#else
    int close_rc=closedir(dir);
#endif
    int close_error=close_rc<0?errno:0;
    probe_step(&probe,"scan_close",0,0,close_rc,close_error);
    if(close_rc<0){
        probe_failure(&probe,"scan_close",close_error);
        safe=0;
    }
    probe_done(&probe,safe);
#else
    closedir(dir);
#endif
    rejection_summary(c,"scan");
    uint64_t finish=monotonic_us();
    if(finish>=scan_start)c->stats.scan_ms=(finish-scan_start)/1000u;
    if(!safe){x4_catalog_disk_cache_close(c);return NULL;}
    return c;
}
X4CatalogDiskCache *x4_catalog_disk_cache_open(const char *root,size_t budget,unsigned products,
    X4CatalogDiskClock clock,void *context)
{ return cache_open_internal(root,budget,products,clock,context,NULL); }
void x4_catalog_disk_cache_close(X4CatalogDiskCache *c)
{ if(c){rejection_summary(c,"get");if(c->directory>=0)close(c->directory);free(c);} }
static int cache_get(X4CatalogDiskCache *c,const char *key,unsigned kind,void *out,size_t capacity,
    size_t *length,const _Atomic int *cancel)
{
    *length=0;
    if(!c)return 0;
    if(cancelled(c,cancel))return 0;
#ifdef X4_DISK_PATH_MUTATIONS
    if(!root_guard(c)){++c->stats.io_errors;return -1;}
#endif
    int i=find(c,key);
    if(i<0){count_miss(c,prior_miss(c,key));return 0;}
    Record *known=&c->record[i];uint64_t now=c->clock(c->clock_context);
    if(!now){count_miss(c,MISS_FILTERED);return 0;}
    if(expired(known,now)){
        remove_record(c,(unsigned)i,MISS_EXPIRED);
        count_miss(c,MISS_EXPIRED);return 0;
    }
#ifdef X4_DISK_PATH_MUTATIONS
    /* A verified hit may persist the advisory usage word on this fd. */
    int fd=record_open(c,known->name,O_RDWR);
#else
    int fd=record_open(c,known->name,O_RDONLY);
#endif
    if(fd<0){
        MissReason reason=errno==ENOENT?MISS_ABSENT:MISS_FILTERED;
        if(reason==MISS_ABSENT)remove_record(c,(unsigned)i,reason);
        else ++c->stats.io_errors;
        count_miss(c,reason);return 0;
    }
    Record r;uint32_t n,sum;ReadCheck check;
    MissReason reason=MISS_NONE;
    if(!header_read(c,fd,&r,&n,&sum,&check))reason=MISS_CORRUPT;
    else if(r.kind!=kind||strcmp(r.key,key)){check.failed=CHECK_LOOKUP_KEY;reason=MISS_KEY;}
    else if(expired(&r,now))reason=MISS_EXPIRED;
    else if(n>capacity){close(fd);count_miss(c,MISS_FILTERED);return -1;}
    if(reason==MISS_NONE){
        check.expected=n;
        int result=exact_io(fd,out,n,0,c,cancel,&check.transferred);
        if(result==0){close(fd);memset(out,0,capacity);return 0;}
        if(result<0){reject_record(&check,CHECK_PAYLOAD_READ,errno);reason=MISS_CORRUPT;}
        else{
            uint32_t actual=crc(c,UINT32_MAX,key,strlen(key));actual=crc(c,actual,out,n)^UINT32_MAX;
            if(actual!=sum)check.failed=CHECK_PAYLOAD_CRC;
            else if(kind==KIND_METADATA){
                X4ProductMetadata normalized;
                if(!metadata_normalize(out,r.id,&normalized)||memcmp(&normalized,out,sizeof(normalized)))
                    check.failed=CHECK_PAYLOAD_METADATA;
            }else if(!image_valid(out,n))check.failed=CHECK_PAYLOAD_IMAGE;
            if(check.failed!=CHECK_OK)reason=MISS_CORRUPT;
        }
    }
    if(reason!=MISS_NONE){
        close(fd);memset(out,0,capacity);
        if(check.failed!=CHECK_OK)note_rejection(c,"get",&check);
        if(check.error){++c->stats.io_errors;count_miss(c,MISS_FILTERED);return 0;}
        remove_record(c,(unsigned)i,reason);count_miss(c,reason);return 0;
    }
    /* Usage is advisory, independent of the immutable header/key/payload
     * CRCs. Never rewrite the image, metadata or creation time on a hit. */
    if(record_touch(c,fd,now)<0)++c->stats.io_errors;
    close(fd);known->used=now;known->order=++c->order;*length=n;
    if(kind==KIND_METADATA)++c->stats.metadata_hits;else ++c->stats.image_hits;
    return 1;
}
static int cache_put(X4CatalogDiskCache *c,const char *key,const char *id,unsigned kind,
    const void *payload,size_t length,const _Atomic int *cancel)
{
    if(!c)return 0;
    if(cancelled(c,cancel))return 0;
#ifdef X4_DISK_PATH_MUTATIONS
    if(!root_guard(c)){++c->stats.io_errors;return -1;}
#endif
    uint64_t now=c->clock(c->clock_context);if(!now)return 0;
    size_t total=HEADER_SIZE+strlen(key)+length;if(total>c->budget)return 0;
    char name[FILE_CAP],temporary[FILE_CAP];file_name(key,name);memcpy(temporary,name,FILE_CAP);memcpy(temporary+36,".tmp",5);
    /* A filename collision with another complete key must remain a miss. */
    for(unsigned i=0;i<c->capacity;++i)if(c->record[i].occupied&&!strcmp(c->record[i].name,name)&&strcmp(c->record[i].key,key))return -1;
    if(!erase_file(c,temporary)){++c->stats.io_errors;return -1;}
    int fd=record_open(c,temporary,O_WRONLY|O_CREAT|O_EXCL);
    if(fd<0){++c->stats.io_errors;return -1;}
    unsigned char h[HEADER_SIZE]={0};memcpy(h,"X4PUBLIC",8);put32(h+8,1);put32(h+12,kind);put64(h+16,now);
    put32(h+24,(uint32_t)strlen(key));put32(h+28,(uint32_t)length);
    uint32_t key_crc=crc(c,UINT32_MAX,key,strlen(key));put32(h+32,crc(c,key_crc,payload,length)^UINT32_MAX);
    put32(h+36,key_crc^UINT32_MAX);put32(h+40,crc(c,UINT32_MAX,h,40)^UINT32_MAX);
    int rc=exact_io(fd,h,sizeof(h),1,c,cancel,NULL);
    if(rc==1)rc=exact_io(fd,(void *)key,strlen(key),1,c,cancel,NULL);
    if(rc==1)rc=exact_io(fd,(void *)payload,length,1,c,cancel,NULL);
    if(rc==1&&!cancelled(c,cancel)){if(fsync(fd)<0)rc=-1;}else if(rc==1)rc=0;
    if(close(fd)<0&&rc==1)rc=-1;
    if(rc!=1){erase_file(c,temporary);if(rc<0)++c->stats.io_errors;return rc;}
    /* Make room before the final rename. This permits at most one bounded
     * temporary <=2MiB above the configured committed-file budget. */
    int slot=find(c,key);
    if(slot<0&&c->first_free<c->capacity)slot=(int)c->first_free;
    if(slot<0){
        slot=oldest(c,-1);
        if(slot<0){erase_file(c,temporary);return 0;}
        remove_record(c,(unsigned)slot,MISS_EVICTED);
    }
    if(slot<0||(c->record[slot].occupied&&strcmp(c->record[slot].key,key))){erase_file(c,temporary);return -1;}
    size_t previous=c->record[slot].occupied?c->record[slot].size:0;
    int present=product_slot(c,id,0)!=NULL;
    while(c->stats.bytes-previous+total>c->budget||(!present&&c->stats.products>=c->max_products)){
        int victim=oldest(c,slot);if(victim<0){erase_file(c,temporary);return 0;}
        unsigned before=c->stats.records;remove_record(c,(unsigned)victim,MISS_EVICTED);
        if(before==c->stats.records){erase_file(c,temporary);return -1;}
        present=product_slot(c,id,0)!=NULL;
    }
    if(cancelled(c,cancel)){erase_file(c,temporary);return 0;}
    struct stat st;int existing=record_stat(c,name,&st);
    if((existing<0&&errno!=ENOENT)||(existing==0&&!S_ISREG(st.st_mode))){erase_file(c,temporary);++c->stats.io_errors;return -1;}
    if(record_rename(c,temporary,name)<0){erase_file(c,temporary);++c->stats.io_errors;return -1;}
#ifdef X4_DISK_PATH_MUTATIONS
    int saved=record_open(c,name,O_RDWR);
#else
    int saved=record_open(c,name,O_RDONLY);
#endif
    if(saved>=0){if(record_touch(c,saved,now)<0)++c->stats.io_errors;close(saved);}
    /* Directory sync is best effort (some consoles/filesystems reject it).
     * Atomic visibility is guaranteed by rename; power-loss durability is
     * not claimed. Startup discards incomplete/corrupt files regardless. */
    if(fsync(c->directory)<0)++c->stats.io_errors;
    Record r={.size=total,.kind=kind,.created=now,.used=now,.order=++c->order,.occupied=1};
    memcpy(r.name,name,FILE_CAP);memcpy(r.key,key,strlen(key)+1);memcpy(r.id,id,strlen(id)+1);
    if(!assign_record(c,(unsigned)slot,&r)){++c->stats.io_errors;return -1;}
    ++c->stats.stores;return 1;
}
int x4_catalog_disk_cache_get_metadata(X4CatalogDiskCache *c,const char *id,const char *market,const char *language,
    X4ProductMetadata *out,const _Atomic int *cancel)
{
    if(!out)return -1;
    memset(out,0,sizeof(*out));char key[KEY_CAP],normalized[X4_PRODUCT_ID_CAP];
    if(!key_make(KIND_METADATA,id,market,language,NULL,key,normalized)){count_miss(c,MISS_FILTERED);return -1;}
    size_t length;return cache_get(c,key,KIND_METADATA,out,sizeof(*out),&length,cancel);
}
int x4_catalog_disk_cache_put_metadata(X4CatalogDiskCache *c,const char *id,const char *market,const char *language,
    const X4ProductMetadata *in,const _Atomic int *cancel)
{
    char key[KEY_CAP],id_normal[X4_PRODUCT_ID_CAP];X4ProductMetadata normalized;
    if(!key_make(KIND_METADATA,id,market,language,NULL,key,id_normal)||!metadata_normalize(in,id_normal,&normalized))return -1;
    return cache_put(c,key,id_normal,KIND_METADATA,&normalized,sizeof(normalized),cancel);
}
int x4_catalog_disk_cache_get_image(X4CatalogDiskCache *c,const char *id,const char *market,const char *language,
    const char *url,unsigned char *bytes,size_t capacity,size_t *length,const _Atomic int *cancel)
{
    if(!length)return -1;
    *length=0;if(!bytes||!capacity||capacity>X4_HTTP_IMAGE_MAX+1u)return -1;
    char key[KEY_CAP],normalized[X4_PRODUCT_ID_CAP];
    if(!key_make(KIND_IMAGE,id,market,language,url,key,normalized)){count_miss(c,MISS_FILTERED);return -1;}
    return cache_get(c,key,KIND_IMAGE,bytes,capacity,length,cancel);
}
int x4_catalog_disk_cache_put_image(X4CatalogDiskCache *c,const char *id,const char *market,const char *language,
    const char *url,const unsigned char *bytes,size_t length,const _Atomic int *cancel)
{
    char key[KEY_CAP],normalized[X4_PRODUCT_ID_CAP];
    if(!key_make(KIND_IMAGE,id,market,language,url,key,normalized)||!image_valid(bytes,length))return -1;
    return cache_put(c,key,normalized,KIND_IMAGE,bytes,length,cancel);
}
int x4_catalog_disk_cache_remove_image(X4CatalogDiskCache *c,const char *id,const char *market,const char *language,const char *url)
{
    char key[KEY_CAP],normalized[X4_PRODUCT_ID_CAP];
    if(!key_make(KIND_IMAGE,id,market,language,url,key,normalized))return -1;
    if(!c)return 0;
    int i=find(c,key);if(i<0)return 0;
    remove_record(c,(unsigned)i,MISS_CORRUPT);return c->record[i].occupied?-1:1;
}
int x4_catalog_disk_cache_clear(X4CatalogDiskCache *c,const _Atomic int *cancel)
{
    if(!c)return 0;
    for(unsigned i=0;i<c->capacity;++i){if(cancelled(c,cancel))return 0;remove_record(c,i,MISS_NONE);}
    memset(c->misses,0,sizeof(c->misses));
    c->next_miss_note=0;
    return c->stats.records? -1:1;
}
void x4_catalog_disk_cache_stats(const X4CatalogDiskCache *c,X4CatalogDiskStats *out)
{ if(out){memset(out,0,sizeof(*out));if(c)*out=c->stats;} }
