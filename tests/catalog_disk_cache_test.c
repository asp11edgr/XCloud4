/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#ifdef X4_DISK_CACHE_NATIVE_FS_TEST
#define _GNU_SOURCE
#endif
#include "../src/ui/catalog_disk_cache.h"
#include "../src/auth/http_client.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#ifdef X4_DISK_CACHE_NATIVE_FS_TEST
#include "../src/core/x4_log.h"
#include <errno.h>
#include <stdarg.h>
#include <sys/time.h>
#endif

static unsigned checks;
#define CHECK(v) do {++checks;if(!(v)){fprintf(stderr,"disk cache check %u failed %s:%d: %s\n",checks,__FILE__,__LINE__,#v);exit(1);}}while(0)
static uint64_t now=1000000;
static _Atomic int cancel;
static int clock_cancel;
static uint64_t clock_seconds(void *unused)
{ (void)unused;if(clock_cancel)atomic_store(&cancel,1);return now; }
static void path_join(char out[1024],const char *root,const char *suffix)
{
    size_t a=strlen(root),b=strlen(suffix);CHECK(a+b+2<=1024);
    memcpy(out,root,a);out[a]='/';memcpy(out+a+1,suffix,b+1);
}
static X4ProductMetadata metadata(const char *id)
{
    X4ProductMetadata m={0};snprintf(m.product_id,sizeof(m.product_id),"%s",id);
    snprintf(m.name,sizeof(m.name),"Edición pingüino ¿sí? ¡Sí! %s",id);
    strcpy(m.description,"Descripción pública, sin credenciales ni permisos.");
    strcpy(m.cover_url,"https://store-images.s-microsoft.com/image/apps.cache-test?w=356&h=200&q=85&format=jpg");return m;
}
static X4CatalogDiskCache *open_cache(const char *root,size_t budget,unsigned products)
{
    X4CatalogDiskCache *c=x4_catalog_disk_cache_open(root,budget,products,clock_seconds,NULL);CHECK(c);return c;
}
static unsigned char *read_image(const char *path,size_t *length)
{
    FILE *f=fopen(path,"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0);rewind(f);
    unsigned char *p=malloc((size_t)n);CHECK(p);CHECK(fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);*length=(size_t)n;return p;
}
static void one_file(const char *root,char out[1024])
{
    DIR *d=opendir(root);CHECK(d);struct dirent *e;out[0]=0;
    while((e=readdir(d)))if(strlen(e->d_name)==40&&!strcmp(e->d_name+36,".bin")){
        path_join(out,root,e->d_name);break;
    }
    closedir(d);CHECK(out[0]);
}
static void test_roundtrip(const char *root,const unsigned char *image,size_t n)
{
    X4CatalogDiskCache *c=open_cache(root,0,0);X4ProductMetadata m=metadata("P1"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"P1","MX","es-MX",&m,NULL)==1);
    CHECK(x4_catalog_disk_cache_put_image(c,"P1","MX","es-MX",m.cover_url,image,n,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"p1","mx","ES-mx",&out,NULL)==1);
    CHECK(!memcmp(&m,&out,sizeof(m)));
    CHECK(x4_catalog_disk_cache_get_metadata(c,"P1","US","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"P1","MX","en-US",&out,NULL)==0);
    unsigned char *bytes=malloc(X4_HTTP_IMAGE_MAX+1u);CHECK(bytes);size_t length;
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==1);
    CHECK(length==n&&!memcmp(image,bytes,n));
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,1,&length,NULL)==-1);
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==1);
    const char *other="https://store-images.s-microsoft.com/image/apps.cache-test?w=384&h=216&q=85&format=jpg";
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",other,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==0);
    CHECK(x4_catalog_disk_cache_remove_image(c,"P1","MX","es-MX",other)==0);
    X4CatalogDiskStats s;x4_catalog_disk_cache_stats(c,&s);CHECK(s.records==2&&s.products==1&&s.stores==2&&s.metadata_hits==1&&s.image_hits==2);
    CHECK(x4_catalog_disk_cache_put_image(c,"P1","MX","es-MX",other,image,n,NULL)==1);
    CHECK(x4_catalog_disk_cache_remove_image(c,"P1","MX","es-MX",other)==1);
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==1);
    x4_catalog_disk_cache_close(c);++now;c=open_cache(root,0,0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"P1","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==1);
    now+=X4_CATALOG_DISK_METADATA_TTL;
    CHECK(x4_catalog_disk_cache_get_metadata(c,"P1","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==1);
    now+=X4_CATALOG_DISK_IMAGE_TTL;
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==0);
    CHECK(!length);x4_catalog_disk_cache_close(c);free(bytes);
}
static void test_lru(const char *root)
{
    X4CatalogDiskCache *c=open_cache(root,0,2);X4ProductMetadata a=metadata("A"),b=metadata("B"),d=metadata("D"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"A","MX","es-MX",&a,NULL)==1);++now;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"B","MX","es-MX",&b,NULL)==1);++now;
    CHECK(x4_catalog_disk_cache_get_metadata(c,"A","MX","es-MX",&out,NULL)==1);
    x4_catalog_disk_cache_close(c);++now;c=open_cache(root,0,2);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"D","MX","es-MX",&d,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"A","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"B","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"D","MX","es-MX",&out,NULL)==1);
    X4CatalogDiskStats s;x4_catalog_disk_cache_stats(c,&s);CHECK(s.products==2&&s.evictions==1);
    x4_catalog_disk_cache_close(c);c=open_cache(root,sizeof(X4ProductMetadata)+100,2);
    x4_catalog_disk_cache_stats(c,&s);CHECK(s.records<=1&&s.bytes<=sizeof(X4ProductMetadata)+100);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);x4_catalog_disk_cache_close(c);
}
static void test_corruption(const char *root,const unsigned char *image,size_t n)
{
    X4CatalogDiskCache *c=open_cache(root,0,0);X4ProductMetadata m=metadata("CORRUPT"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"CORRUPT","MX","es-MX",&m,NULL)==1);
    char file[1024];one_file(root,file);int fd=open(file,O_RDWR);CHECK(fd>=0);
    CHECK(lseek(fd,-1,SEEK_END)>0);unsigned char bad=37;CHECK(write(fd,&bad,1)==1);close(fd);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"CORRUPT","MX","es-MX",&out,NULL)==0);CHECK(!out.name[0]);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"CORRUPT","MX","es-MX",&m,NULL)==1);
    one_file(root,file);CHECK(truncate(file,23)==0);x4_catalog_disk_cache_close(c);
    c=open_cache(root,0,0);X4CatalogDiskStats s;x4_catalog_disk_cache_stats(c,&s);CHECK(s.corrupt==1&&!s.records);
    CHECK(x4_catalog_disk_cache_put_image(c,"CORRUPT","MX","es-MX",m.cover_url,image,n,NULL)==1);
    one_file(root,file);fd=open(file,O_RDWR);CHECK(fd>=0);CHECK(lseek(fd,-1,SEEK_END)>0);CHECK(write(fd,&bad,1)==1);close(fd);
    unsigned char *bytes=malloc(X4_HTTP_IMAGE_MAX+1u);CHECK(bytes);size_t length;
    CHECK(x4_catalog_disk_cache_get_image(c,"CORRUPT","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==0);CHECK(!length);
    x4_catalog_disk_cache_stats(c,&s);CHECK(s.corrupt==2);free(bytes);
    CHECK(x4_catalog_disk_cache_put_image(c,"CORRUPT","MX","es-MX",m.cover_url,image,n,NULL)==1);
    CHECK(x4_catalog_disk_cache_remove_image(c,"CORRUPT","MX","es-MX",m.cover_url)==1);
    CHECK(x4_catalog_disk_cache_remove_image(c,"CORRUPT","MX","es-MX",m.cover_url)==0);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"CORRUPT","MX","es-MX",&m,NULL)==1);one_file(root,file);
    char temporary[1024];memcpy(temporary,file,strlen(file)+1);memcpy(temporary+strlen(temporary)-4,".tmp",5);
    fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);CHECK(write(fd,"incomplete",10)==10);close(fd);
    x4_catalog_disk_cache_close(c);c=open_cache(root,0,0);CHECK(access(temporary,F_OK)<0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"CORRUPT","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);x4_catalog_disk_cache_close(c);
}
static void test_record_bound_and_startup_expiry(const char *root)
{
    X4CatalogDiskCache *c=open_cache(root,0,1);X4ProductMetadata m=metadata("ONE"),out;
    enum { record_limit = X4_COVER_SURFACES + 2 };
    static const char *const languages[] = {
        "es-MX", "en-US", "fr-FR", "pt-BR", "de-DE", "it-IT", "ja-JP", "ko-KR", "pl-PL", "nl-NL"
    };
    _Static_assert(sizeof(languages) / sizeof(languages[0]) > record_limit, "Need one more key than the cache limit");
    for(unsigned i=0;i<=record_limit;++i){
        CHECK(x4_catalog_disk_cache_put_metadata(c,"ONE","MX",languages[i],&m,NULL)==1);
        ++now;
    }
    CHECK(x4_catalog_disk_cache_get_metadata(c,"ONE","MX",languages[0],&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"ONE","MX",languages[1],&out,NULL)==1);
    X4CatalogDiskStats s;x4_catalog_disk_cache_stats(c,&s);
    CHECK(s.records==record_limit&&s.products==1&&s.evictions==1);
    x4_catalog_disk_cache_close(c);now+=X4_CATALOG_DISK_METADATA_TTL;
    c=open_cache(root,0,1);x4_catalog_disk_cache_stats(c,&s);CHECK(!s.records&&s.expired==record_limit);
    x4_catalog_disk_cache_close(c);
    /* A byte budget smaller than a single typed record stores nothing. */
    c=open_cache(root,100,1);CHECK(x4_catalog_disk_cache_put_metadata(c,"ONE","MX","es-MX",&m,NULL)==0);
    x4_catalog_disk_cache_stats(c,&s);CHECK(!s.records&&!s.bytes);x4_catalog_disk_cache_close(c);
}
static void test_validation_cancel(const char *root,const unsigned char *image,size_t n)
{
    X4CatalogDiskCache *c=open_cache(root,0,0);X4ProductMetadata m=metadata("SAFE"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"../BAD","MX","es-MX",&m,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","../","es-MX",&m,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es/../../",&m,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"OTHER","MX","es-MX",&m,NULL)==-1);
    X4ProductMetadata bad=m;memset(bad.name,'a',sizeof(bad.name));CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&bad,NULL)==-1);
    bad=m;strcpy(bad.name,"\xc0\xaf");CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&bad,NULL)==-1);
    bad=m;strcpy(bad.cover_url,"https://example.org/auth?token=private");CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&bad,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_image(c,"SAFE","MX","es-MX",m.cover_url,(const unsigned char *)"corrupt",7,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_image(c,"SAFE","MX","es-MX","https://example.org/cover",image,n,NULL)==-1);
    atomic_store(&cancel,1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&m,&cancel)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"SAFE","MX","es-MX",&out,&cancel)==0);
    atomic_store(&cancel,0);clock_cancel=1;
    CHECK(x4_catalog_disk_cache_put_image(c,"SAFE","MX","es-MX",m.cover_url,image,n,&cancel)==0);
    clock_cancel=0;atomic_store(&cancel,0);X4CatalogDiskStats s;x4_catalog_disk_cache_stats(c,&s);CHECK(!s.records&&s.cancelled>=3);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&m,NULL)==1);
    X4ProductMetadata replacement=m;strcpy(replacement.name,"replacement must not publish");
    clock_cancel=1;CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&replacement,&cancel)==0);
    clock_cancel=0;atomic_store(&cancel,0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"SAFE","MX","es-MX",&out,NULL)==1&&!strcmp(out.name,m.name));
    atomic_store(&cancel,1);CHECK(x4_catalog_disk_cache_clear(c,&cancel)==0);
    atomic_store(&cancel,0);CHECK(x4_catalog_disk_cache_get_metadata(c,"SAFE","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);x4_catalog_disk_cache_close(c);
}
static void test_paths(const char *base,const char *root)
{
    CHECK(!x4_catalog_disk_cache_open(NULL,0,0,NULL,NULL));
    CHECK(!x4_catalog_disk_cache_open("relative/cache",0,0,NULL,NULL));
    CHECK(!x4_catalog_disk_cache_open("/tmp/../escape",0,0,NULL,NULL));
    CHECK(!x4_catalog_disk_cache_open(root,X4_CATALOG_DISK_BUDGET+1u,0,NULL,NULL));
    CHECK(!x4_catalog_disk_cache_open(root,0,X4_CATALOG_DISK_PRODUCTS+1u,NULL,NULL));
    char link[1024],linked_root[1024];path_join(link,base,"link");CHECK(!symlink(root,link));
    path_join(linked_root,base,"link/subdir");CHECK(!x4_catalog_disk_cache_open(linked_root,0,0,NULL,NULL));
    char outside[1024],owned[1024],unknown[1024];path_join(outside,base,"outside");
    int fd=open(outside,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);CHECK(write(fd,"keep",4)==4);close(fd);
    path_join(owned,root,"x4c-00000000000000000000000000000000.bin");CHECK(!symlink(outside,owned));
    path_join(unknown,root,"not-our-file");fd=open(unknown,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);close(fd);
    X4CatalogDiskCache *c=open_cache(root,0,0);CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);x4_catalog_disk_cache_close(c);
    struct stat s;CHECK(!lstat(owned,&s)&&S_ISLNK(s.st_mode));CHECK(!stat(outside,&s)&&s.st_size==4);CHECK(!access(unknown,F_OK));
    CHECK(!unlink(owned));CHECK(!unlink(unknown));CHECK(!unlink(link));CHECK(!unlink(outside));
}
static void test_root_identity(const char *base,const char *root)
{
    X4CatalogDiskCache *c=open_cache(root,0,0);X4ProductMetadata m=metadata("IDENTITY"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"IDENTITY","MX","es-MX",&m,NULL)==1);
    char old_root[1024];path_join(old_root,base,"old-cache");CHECK(!rename(root,old_root));CHECK(!mkdir(root,0700));
#ifdef X4_DISK_CACHE_NATIVE_FS_TEST
    CHECK(x4_catalog_disk_cache_get_metadata(c,"IDENTITY","MX","es-MX",&out,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"IDENTITY","MX","es-MX",&m,NULL)==-1);
#else
    /* Descriptor-relative host mutations stay inside the retained identity. */
    CHECK(x4_catalog_disk_cache_get_metadata(c,"IDENTITY","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"IDENTITY","MX","es-MX",&m,NULL)==1);
#endif
    CHECK(!rmdir(root));CHECK(!rename(old_root,root));
    CHECK(x4_catalog_disk_cache_get_metadata(c,"IDENTITY","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);x4_catalog_disk_cache_close(c);
}
static void test_protected_lru(const char *root,const unsigned char *image,size_t n)
{
    X4CatalogDiskCache *c=open_cache(root,0,2);
    X4ProductMetadata a=metadata("FRONT"),b=metadata("REGULAR"),d=metadata("NEW"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"FRONT","MX","es-MX",&a,NULL)==1);
    CHECK(x4_catalog_disk_cache_put_image(c,"FRONT","MX","es-MX",a.cover_url,image,n,NULL)==1);
    ++now;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"REGULAR","MX","es-MX",&b,NULL)==1);
    ++now;
    const char first[][X4_PRODUCT_ID_CAP]={"front","front","invalid/id"};
    x4_catalog_disk_cache_protect_products(c,first,3);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"NEW","MX","es-MX",&d,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"REGULAR","MX","es-MX",&out,NULL)==0);
    X4CatalogDiskStats stats;x4_catalog_disk_cache_stats(c,&stats);
    CHECK(stats.products==2&&stats.records==3&&stats.evictions==1&&stats.miss_evicted==1);
    x4_catalog_disk_cache_close(c);

    c=open_cache(root,0,2);
    unsigned char *readback=malloc(n);CHECK(readback);size_t length=0;
    CHECK(x4_catalog_disk_cache_get_metadata(c,"FRONT","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_image(c,"FRONT","MX","es-MX",a.cover_url,readback,n,&length,NULL)==1);
    CHECK(length==n&&!memcmp(readback,image,n));free(readback);
    const char both[][X4_PRODUCT_ID_CAP]={"FRONT","NEW"};
    x4_catalog_disk_cache_protect_products(c,both,2);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"REGULAR","MX","es-MX",&b,NULL)==0);
    x4_catalog_disk_cache_stats(c,&stats);
    CHECK(stats.products==2&&stats.records==3&&!stats.evictions);
    x4_catalog_disk_cache_protect_products(c,NULL,0);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"REGULAR","MX","es-MX",&b,NULL)==1);
    x4_catalog_disk_cache_stats(c,&stats);CHECK(stats.products==2&&stats.evictions>0);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);
    x4_catalog_disk_cache_close(c);
}
static void test_miss_reasons(const char *root)
{
    X4CatalogDiskCache *c=open_cache(root,0,0);
    X4ProductMetadata out,expired=metadata("OLD");
    CHECK(x4_catalog_disk_cache_put_metadata(c,"OLD","MX","es-MX",&expired,NULL)==1);
    x4_catalog_disk_cache_close(c);
    now+=X4_CATALOG_DISK_METADATA_TTL;
    c=open_cache(root,0,0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"OLD","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"MISSING","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"../INVALID","MX","es-MX",&out,NULL)==-1);
    X4CatalogDiskStats startup;x4_catalog_disk_cache_stats(c,&startup);
    CHECK(startup.miss_expired==1&&startup.miss_absent==1&&startup.miss_filtered==1);

    X4ProductMetadata other=metadata("OTHER"),bad=metadata("BROKEN");
    CHECK(x4_catalog_disk_cache_put_metadata(c,"OTHER","MX","es-MX",&other,NULL)==1);
    char wanted[1024],renamed[1024];one_file(root,wanted);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"BROKEN","MX","es-MX",&bad,NULL)==1);
    one_file(root,renamed);
    CHECK(!rename(renamed,wanted));
    x4_catalog_disk_cache_close(c);

    c=open_cache(root,0,0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"OTHER","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"BROKEN","MX","es-MX",&bad,NULL)==1);
    one_file(root,renamed);CHECK(!truncate(renamed,23));
    CHECK(x4_catalog_disk_cache_get_metadata(c,"BROKEN","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"OLD","MX","es-MX",&expired,NULL)==1);
    one_file(root,renamed);CHECK(!unlink(renamed));
    CHECK(x4_catalog_disk_cache_get_metadata(c,"OLD","MX","es-MX",&out,NULL)==0);
    X4CatalogDiskStats stats;x4_catalog_disk_cache_stats(c,&stats);
    CHECK(stats.misses==3&&stats.miss_key==1&&stats.miss_corrupt==1&&stats.miss_absent==1);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);
    x4_catalog_disk_cache_close(c);

    c=open_cache(root,0,0);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"OLD","MX","es-MX",&expired,NULL)==1);
    now+=X4_CATALOG_DISK_METADATA_TTL;
    CHECK(x4_catalog_disk_cache_get_metadata(c,"OLD","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"MISSING","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"../INVALID","MX","es-MX",&out,NULL)==-1);
    x4_catalog_disk_cache_stats(c,&stats);
    CHECK(stats.misses==3&&stats.miss_expired==1&&stats.miss_absent==1&&stats.miss_filtered==1);
    x4_catalog_disk_cache_close(c);
}
static uint64_t monotonic_ms(void)
{
    struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
}
static void test_full_scan(const char *root,const unsigned char *image,size_t n)
{
    enum { CARD_BYTES=26*1024, BACKGROUND_BYTES=320*1024 };
    CHECK(X4_CATALOG_DISK_RECORDS>2u*X4_CATALOG_DISK_PRODUCTS);
    CHECK(X4_CATALOG_DISK_RECORDS<3u*X4_CATALOG_DISK_PRODUCTS);
    CHECK(n<CARD_BYTES);
    /* Synthetic encoded payload sizes. The startup reader validates the
     * generated filename, lengths, key and header CRC without decoding. */
    unsigned char *encoded=calloc(1,BACKGROUND_BYTES);CHECK(encoded);
    memcpy(encoded,image,n);
    X4CatalogDiskCache *c=open_cache(root,0,0);
    for(unsigned i=0;i<X4_CATALOG_DISK_RECORDS;++i){
        unsigned variant=i/X4_CATALOG_DISK_PRODUCTS;
        char id[X4_PRODUCT_ID_CAP];
        snprintf(id,sizeof(id),"SCAN%04u",i%X4_CATALOG_DISK_PRODUCTS);
        if(!variant){
            X4ProductMetadata m=metadata(id);
            CHECK(x4_catalog_disk_cache_put_metadata(c,id,"MX","es-MX",&m,NULL)==1);
        }else{
            const char *url=variant==1?
                "https://store-images.s-microsoft.com/image/apps.scan?w=384&h=216&q=85&format=jpg":
                "https://store-images.s-microsoft.com/image/apps.scan?w=1920&h=1080&q=85&format=jpg";
            CHECK(x4_catalog_disk_cache_put_image(c,id,"MX","es-MX",url,encoded,
                variant==1?CARD_BYTES:BACKGROUND_BYTES,NULL)==1);
        }
    }
    free(encoded);
    X4CatalogDiskStats before,after;x4_catalog_disk_cache_stats(c,&before);
    CHECK(before.records==X4_CATALOG_DISK_RECORDS&&before.products==X4_CATALOG_DISK_PRODUCTS);
    CHECK(before.bytes<X4_CATALOG_DISK_BUDGET&&!before.evictions&&!before.io_errors);
    x4_catalog_disk_cache_close(c);
    uint64_t start=monotonic_ms();
    c=open_cache(root,0,0);
    uint64_t elapsed=monotonic_ms()-start;
    x4_catalog_disk_cache_stats(c,&after);
    CHECK(after.records==before.records&&after.products==before.products&&after.bytes==before.bytes);
    CHECK(!after.evictions&&!after.corrupt&&!after.expired&&!after.scan_omitted&&!after.io_errors);
    CHECK(after.scan_ms<=elapsed+1);
    printf("disk full scan records=%u products=%u bytes=%zu scan_ms=%llu outer_ms=%llu\n",
        after.records,after.products,after.bytes,(unsigned long long)after.scan_ms,(unsigned long long)elapsed);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);
    x4_catalog_disk_cache_close(c);
}
#ifdef X4_DISK_CACHE_NATIVE_FS_TEST
typedef struct {
    int enabled, error;
    unsigned long long duration;
    char failed_step[32];
    unsigned summaries, scan_open, scan_read, roundtrip;
} NativeSummary;

typedef enum {
    SCAN_OK, SCAN_OPEN_DENIED, SCAN_WRONG_INODE, SCAN_READ_DENIED, SCAN_BAD_RECORD,
    SCAN_READ_INTERRUPTED
} ScanFault;
static NativeSummary native_summary;
static int reject_flag, futimes_error, collide_with_probe, check_nofollow;
static unsigned rejected_opens, failed_touches;
static ScanFault scan_fault;
static const char *scan_root;
static int scan_ready, scan_injected;
static unsigned read_injected, write_injected, corrupt_logs, summaries_with_short_header, partial_reports;
static unsigned wrong_stat_count;
static int report_blocks_as_size;
static enum { IO_OK, IO_RETRY_PARTIAL, IO_PERSISTENT_EAGAIN, IO_PARTIAL_EOF, IO_ROUNDTRIP_BAD_MAGIC } io_fault;
static char collision_path[1024];
static const char collision_contents[]="pre-existing probe sentinel";

int __real_open(const char *path,int flags,...);
int __real_futimes(int fd,const struct timeval times[2]);
int __real_fstat(int fd,struct stat *st);
ssize_t __real_read(int fd,void *bytes,size_t n);
ssize_t __real_write(int fd,const void *bytes,size_t n);

int __wrap_open(const char *path,int flags,...)
{
    mode_t mode=0;
    if(flags&O_CREAT){
        va_list args;
        va_start(args,flags);
        mode=va_arg(args,mode_t);
        va_end(args);
    }
    if(check_nofollow)CHECK(flags&O_NOFOLLOW);
    if(flags&reject_flag){++rejected_opens;errno=EINVAL;return -1;}
    if(scan_ready&&scan_root&&!strcmp(path,scan_root)){
        if(scan_fault==SCAN_OPEN_DENIED){++scan_injected;errno=EPERM;return -1;}
        if(scan_fault==SCAN_WRONG_INODE){++scan_injected;return __real_open("/",flags);}
    }
    const char *leaf=strrchr(path,'/');
    if(leaf&&!strncmp(leaf+1,"x4c-",4))CHECK(!(flags&O_NONBLOCK));
    if(collide_with_probe&&!collision_path[0]&&leaf&&
       !strncmp(leaf+1,"x4-cache-selftest-",18)&&
       (flags&(O_CREAT|O_EXCL))==(O_CREAT|O_EXCL)){
        size_t n=strlen(path);
        CHECK(n<sizeof(collision_path));
        memcpy(collision_path,path,n+1);
        int fd=__real_open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        CHECK(fd>=0);
        CHECK(write(fd,collision_contents,sizeof(collision_contents))==(ssize_t)sizeof(collision_contents));
        CHECK(!close(fd));
    }
    return flags&O_CREAT?__real_open(path,flags,mode):__real_open(path,flags);
}

ssize_t __wrap_read(int fd,void *bytes,size_t n)
{
    if(io_fault==IO_PERSISTENT_EAGAIN){++read_injected;errno=EAGAIN;return -1;}
    if(io_fault==IO_RETRY_PARTIAL){
        unsigned call=read_injected++;
        if(call<2){errno=call?EAGAIN:EINTR;return -1;}
        if(n>997)n=997;
    }
    if(io_fault==IO_PARTIAL_EOF&&n>997){
        if(read_injected++)return 0;
        n=37;
    }
    ssize_t result=__real_read(fd,bytes,n);
    if(io_fault==IO_ROUNDTRIP_BAD_MAGIC&&result==48&&!memcmp(bytes,"X4PUBLIC",8)){
        ++read_injected;((unsigned char *)bytes)[0]^=1u;
    }
    return result;
}

ssize_t __wrap_write(int fd,const void *bytes,size_t n)
{
    if(io_fault==IO_RETRY_PARTIAL){
        unsigned call=write_injected++;
        if(call<2){errno=call?EINTR:EAGAIN;return -1;}
        if(n>997)n=997;
    }
    return __real_write(fd,bytes,n);
}

int __wrap_fstat(int fd,struct stat *st)
{
    int rc=__real_fstat(fd,st);
    if(!rc&&report_blocks_as_size&&S_ISREG(st->st_mode)){
        /* Native klog: 28,896-byte file reported st_size=64. */
        st->st_size=st->st_blocks;
        ++wrong_stat_count;
    }
    return rc;
}

int __wrap_futimes(int fd,const struct timeval times[2])
{
    if(futimes_error){++failed_touches;errno=futimes_error;return -1;}
    return __real_futimes(fd,times);
}

/* Translate real Linux getdents64 results to the OpenOrbis BSD ABI. This
 * exercises the same record validation and scan loop used on the console. */
int x4_disk_cache_test_getdents(int fd,void *output,size_t capacity)
{
    unsigned char *out=output;
    if(scan_ready&&scan_fault==SCAN_READ_DENIED){++scan_injected;errno=EPERM;return -1;}
    if(scan_ready&&scan_fault==SCAN_READ_INTERRUPTED&&!scan_injected){
        ++scan_injected;errno=EINTR;return -1;
    }
    if(scan_ready&&scan_fault==SCAN_BAD_RECORD){
        CHECK(capacity>=8);
        ++scan_injected;
        memset(out,0,8); /* A zero d_reclen must stop the scan. */
        return 8;
    }
    _Alignas(struct dirent64) unsigned char linux_entries[4096];
    const size_t linux_header=offsetof(struct dirent64,d_name);
    CHECK(capacity<=sizeof(linux_entries));
    ssize_t n=getdents64(fd,linux_entries,capacity);
    if(n<=0)return (int)n;
    size_t used=0;
    for(size_t offset=0;offset<(size_t)n;){
        unsigned short linux_size;
        CHECK((size_t)n-offset>linux_header);
        memcpy(&linux_size,linux_entries+offset+offsetof(struct dirent64,d_reclen),
            sizeof(linux_size));
        CHECK(linux_size>linux_header&&linux_size<=(size_t)n-offset);
        const char *name=(const char *)linux_entries+offset+linux_header;
        const char *end=memchr(name,0,linux_size-linux_header);
        CHECK(end);
        size_t length=(size_t)(end-name);
        size_t bsd_size=(8+length+1+3)&~(size_t)3;
        CHECK(length<=255&&bsd_size<=capacity-used);
        memset(out+used,0,bsd_size);
        out[used]=1; /* A nonzero inode is sufficient for the scanner. */
        out[used+4]=(unsigned char)bsd_size;
        out[used+5]=(unsigned char)(bsd_size>>8);
        out[used+7]=(unsigned char)length;
        memcpy(out+used+8,name,length);
        used+=bsd_size;
        offset+=linux_size;
    }
    return (int)used;
}

int x4_log_printf(const char *format,...)
{
    int saved=errno;
    char line[1024];
    va_list args;
    va_start(args,format);
    int length=vsnprintf(line,sizeof(line),format,args);
    va_end(args);
    CHECK(length>=0&&(size_t)length<sizeof(line));
    int enabled,error;
    unsigned long long duration;
    char step[32];
    int rc;
    if(sscanf(line,"XCloud4: cache autoprueba paso=%31s componente=%*u flags=%*x rc=%d",
              step,&rc)==2){
        if(!strcmp(step,"roundtrip")){++native_summary.roundtrip;if(!rc)scan_ready=1;}
        if(!strcmp(step,"scan_open"))++native_summary.scan_open;
        if(!strcmp(step,"scan_read"))++native_summary.scan_read;
    }
    if(sscanf(line,"XCloud4: cache disco habilitada=%d paso_fallido=%31s errno=%d autoprueba_us=%llu",
              &enabled,step,&error,&duration)==4){
        ++native_summary.summaries;
        native_summary.enabled=enabled;
        native_summary.error=error;
        native_summary.duration=duration;
        memcpy(native_summary.failed_step,step,strlen(step)+1);
    }
    if(strstr(line,"XCloud4: cache registro rechazado")){
        CHECK(strstr(line,"st_size=")&&strstr(line,"file_size=")&&strstr(line,"kn=")&&strstr(line,"pn="));
        CHECK(strstr(line,"leido=")&&strstr(line,"esperado="));
        CHECK(!strstr(line,"https://")&&!strstr(line,"x4c-"));
        if(strstr(line,"motivo=leer_payload")&&strstr(line,"leido=37 ")){
            char expected[32];
            snprintf(expected,sizeof(expected),"esperado=%zu ",sizeof(X4ProductMetadata));
            CHECK(strstr(line,expected));
            ++partial_reports;
        }
        ++corrupt_logs;
    }
    if(strstr(line,"XCloud4: cache rechazos resumen")&&strstr(line,"cabecera_corta="))
        ++summaries_with_short_header;
    errno=saved;
    return length;
}

static void reset_native_summary(void)
{
    memset(&native_summary,0,sizeof(native_summary));
    scan_ready=scan_injected=0;
}

static void check_native_summary(int enabled,const char *step,int error)
{
    CHECK(native_summary.summaries==1);
    CHECK(native_summary.enabled==enabled);
    CHECK(!strcmp(native_summary.failed_step,step));
    CHECK(native_summary.error==error);
}

static void check_no_probe_files(const char *root)
{
    DIR *d=opendir(root);
    CHECK(d);
    struct dirent *entry;
    while((entry=readdir(d))){
        CHECK(strncmp(entry->d_name,"x4-cache-selftest-",18)!=0);
        CHECK(strncmp(entry->d_name,"x4-cache-roundtrip-",19)!=0);
    }
    CHECK(!closedir(d));
}

static void test_native_probe_and_flags(const char *root)
{
    static const int flags[]={0,O_DIRECTORY,O_CLOEXEC,O_NONBLOCK};
    static const char *const names[]={"normal","O_DIRECTORY","O_CLOEXEC","O_NONBLOCK"};
    X4ProductMetadata m=metadata("PROBE"),out;
    for(unsigned i=0;i<sizeof(flags)/sizeof(flags[0]);++i){
        reject_flag=flags[i];
        rejected_opens=0;
        check_nofollow=1;
        reset_native_summary();
        X4CatalogDiskCache *c=open_cache(root,0,0);
        check_native_summary(1,"ninguno",0);
        CHECK(native_summary.scan_open==1&&native_summary.scan_read==1);
        if(i)CHECK(rejected_opens>0);
        else CHECK(!rejected_opens);
        printf("cache native probe case=%s enabled=%d rejected_opens=%u autoprueba_us=%llu target_us=20000\n",
            names[i],native_summary.enabled,rejected_opens,native_summary.duration);
        CHECK(x4_catalog_disk_cache_put_metadata(c,"PROBE","MX","es-MX",&m,NULL)==1);
        CHECK(x4_catalog_disk_cache_get_metadata(c,"PROBE","MX","es-MX",&out,NULL)==1);
        CHECK(!memcmp(&m,&out,sizeof(m)));
        X4CatalogDiskStats stats;
        x4_catalog_disk_cache_stats(c,&stats);
        CHECK(!stats.io_errors);
        CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);
        x4_catalog_disk_cache_close(c);
        check_nofollow=0;
        reject_flag=0;
        check_no_probe_files(root);
    }
}

static uint32_t native_header_usage(const unsigned char *header)
{
    uint32_t use=0;
    for(unsigned i=0;i<4;++i)use|=(uint32_t)header[44+i]<<(8*i);
    return use;
}

static void test_native_header_fallback(const char *root,const unsigned char *image,size_t n)
{
    enum { HEADER_BYTES=48, USAGE_OFFSET=44 };
    X4ProductMetadata a=metadata("KEEP"),b=metadata("OLD"),next=metadata("NEW"),out;
    futimes_error=ENOSYS;
    failed_touches=0;
    reset_native_summary();
    X4CatalogDiskCache *c=open_cache(root,0,2);
    check_native_summary(1,"ninguno",0);
    CHECK(failed_touches>0);
    CHECK(x4_catalog_disk_cache_put_image(c,"KEEP","MX","es-MX",a.cover_url,image,n,NULL)==1);
    char file[1024];
    one_file(root,file);
    size_t size_before,size_after,length;
    unsigned char *before=read_image(file,&size_before);
    CHECK(size_before>=HEADER_BYTES+n);
    CHECK(!memcmp(before+size_before-n,image,n));
    ++now;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"OLD","MX","es-MX",&b,NULL)==1);
    ++now;
    unsigned char *bytes=malloc(n);
    CHECK(bytes);
    CHECK(x4_catalog_disk_cache_get_image(c,"KEEP","MX","es-MX",a.cover_url,bytes,n,&length,NULL)==1);
    CHECK(length==n&&!memcmp(bytes,image,n));
    unsigned char *after=read_image(file,&size_after);
    CHECK(size_before==size_after);
    CHECK(native_header_usage(after)==now&&native_header_usage(before)!=now);
    CHECK(!memcmp(before,after,USAGE_OFFSET));
    CHECK(!memcmp(before+HEADER_BYTES,after+HEADER_BYTES,size_after-HEADER_BYTES));
    free(before);
    free(after);
    X4CatalogDiskStats stats;
    x4_catalog_disk_cache_stats(c,&stats);
    CHECK(!stats.io_errors);
    x4_catalog_disk_cache_close(c);

    /* Reverse the timestamp ordering so only the header can preserve KEEP. */
    struct timespec old_time[2]={{(time_t)(now-100),0},{(time_t)(now-100),0}};
    CHECK(!utimensat(AT_FDCWD,file,old_time,0));
    ++now;
    reset_native_summary();
    c=open_cache(root,0,2);
    check_native_summary(1,"ninguno",0);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"NEW","MX","es-MX",&next,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"OLD","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_image(c,"KEEP","MX","es-MX",a.cover_url,bytes,n,&length,NULL)==1);
    CHECK(length==n&&!memcmp(bytes,image,n));
    x4_catalog_disk_cache_stats(c,&stats);
    CHECK(!stats.io_errors&&stats.evictions==1);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);
    x4_catalog_disk_cache_close(c);
    futimes_error=0;
    free(bytes);
    check_no_probe_files(root);
    puts("cache native futimes ENOSYS fallback persists LRU in header, payload unchanged, io_errors=0");
}

static void test_native_create_failure(const char *root)
{
    collide_with_probe=1;
    collision_path[0]=0;
    reset_native_summary();
    X4CatalogDiskCache *c=x4_catalog_disk_cache_open(root,0,0,clock_seconds,NULL);
    collide_with_probe=0;
    CHECK(!c);
    CHECK(collision_path[0]);
    check_native_summary(0,"create",EEXIST);
    size_t n;
    unsigned char *bytes=read_image(collision_path,&n);
    CHECK(n==sizeof(collision_contents)&&!memcmp(bytes,collision_contents,n));
    free(bytes);
    CHECK(!unlink(collision_path));
    check_no_probe_files(root);
    puts("cache native O_CREAT|O_EXCL collision reports create/EEXIST, disabled, existing file unchanged");
}

static void test_native_scan(const char *root)
{
    X4ProductMetadata stored=metadata("SCAN"),out;
    X4CatalogDiskCache *c=open_cache(root,0,0);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"SCAN","MX","es-MX",&stored,NULL)==1);
    x4_catalog_disk_cache_close(c);
    static const struct {
        ScanFault fault;
        const char *step;
        int error;
        unsigned reads;
    } cases[]={
        {SCAN_OPEN_DENIED,"scan_open",EPERM,0},
        {SCAN_WRONG_INODE,"scan_open",EPERM,0},
        {SCAN_READ_DENIED,"scan_read",EPERM,1},
        {SCAN_BAD_RECORD,"scan_read",EIO,1},
    };
    scan_root=root;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
        scan_fault=cases[i].fault;
        reset_native_summary();
        c=x4_catalog_disk_cache_open(root,0,0,clock_seconds,NULL);
        CHECK(!c&&scan_injected==1);
        check_native_summary(0,cases[i].step,cases[i].error);
        CHECK(native_summary.scan_open==1&&native_summary.scan_read==cases[i].reads);
        check_no_probe_files(root);
    }
    scan_fault=SCAN_READ_INTERRUPTED;
    reset_native_summary();
    c=open_cache(root,0,0);
    CHECK(scan_injected==1);
    check_native_summary(1,"ninguno",0);
    CHECK(native_summary.scan_open==1&&native_summary.scan_read==1);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"SCAN","MX","es-MX",&out,NULL)==1);
    CHECK(!memcmp(&stored,&out,sizeof(stored)));
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);
    x4_catalog_disk_cache_close(c);
    scan_fault=SCAN_OK;scan_root=NULL;
    puts("cache native getdents: scan_open EPERM and inode guard, scan_read EPERM and malformed record, EINTR retry and reopen hit");
}

static void test_native_io_and_roundtrip(const char *root)
{
    reset_native_summary();
    io_fault=IO_RETRY_PARTIAL;read_injected=write_injected=0;
    X4CatalogDiskCache *c=open_cache(root,0,0);
    io_fault=IO_OK;
    CHECK(read_injected>300&&write_injected>300);
    CHECK(native_summary.roundtrip==1);
    check_native_summary(1,"ninguno",0);
    check_no_probe_files(root);

    X4ProductMetadata original=metadata("EAGAIN"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"EAGAIN","MX","es-MX",&original,NULL)==1);
    io_fault=IO_PERSISTENT_EAGAIN;read_injected=0;
    CHECK(x4_catalog_disk_cache_get_metadata(c,"EAGAIN","MX","es-MX",&out,NULL)==0);
    io_fault=IO_OK;
    CHECK(read_injected==9);
    X4CatalogDiskStats stats;x4_catalog_disk_cache_stats(c,&stats);
    CHECK(stats.records==1&&stats.io_errors==1&&!stats.corrupt);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"EAGAIN","MX","es-MX",&out,NULL)==1);
    CHECK(!memcmp(&out,&original,sizeof(out)));
    io_fault=IO_PARTIAL_EOF;read_injected=0;
    CHECK(x4_catalog_disk_cache_get_metadata(c,"EAGAIN","MX","es-MX",&out,NULL)==0);
    io_fault=IO_OK;
    CHECK(read_injected==2&&partial_reports==1);
    x4_catalog_disk_cache_stats(c,&stats);
    CHECK(!stats.records&&stats.corrupt==1);
    x4_catalog_disk_cache_close(c);

    reset_native_summary();
    io_fault=IO_ROUNDTRIP_BAD_MAGIC;read_injected=0;
    c=x4_catalog_disk_cache_open(root,0,0,clock_seconds,NULL);
    io_fault=IO_OK;
    CHECK(!c&&read_injected==1&&native_summary.roundtrip==1&&native_summary.scan_open==0);
    check_native_summary(0,"roundtrip_magia",EIO);
    check_no_probe_files(root);

    c=open_cache(root,0,0);
    for(unsigned i=0;i<40;++i){
        CHECK(x4_catalog_disk_cache_put_metadata(c,"EAGAIN","MX","es-MX",&original,NULL)==1);
        char file[1024];one_file(root,file);CHECK(!truncate(file,17));
        CHECK(x4_catalog_disk_cache_get_metadata(c,"EAGAIN","MX","es-MX",&out,NULL)==0);
    }
    x4_catalog_disk_cache_close(c);
    CHECK(corrupt_logs==32&&summaries_with_short_header>0);
    puts("cache native roundtrip, partial I/O, EINTR/EAGAIN and bounded sanitized rejection log: PASS");
}

static void test_native_stat_size_is_not_bytes(const char *root)
{
    report_blocks_as_size=1;wrong_stat_count=0;
    X4CatalogDiskCache *c=open_cache(root,0,0);
    X4ProductMetadata original=metadata("STAT"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"STAT","MX","es-MX",&original,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"STAT","MX","es-MX",&out,NULL)==1);
    CHECK(!memcmp(&out,&original,sizeof(out)));
    x4_catalog_disk_cache_close(c);
    c=open_cache(root,0,0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"STAT","MX","es-MX",&out,NULL)==1);
    CHECK(!memcmp(&out,&original,sizeof(out)));
    X4CatalogDiskStats stats;x4_catalog_disk_cache_stats(c,&stats);
    CHECK(stats.records==1&&!stats.corrupt&&wrong_stat_count>0);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);
    x4_catalog_disk_cache_close(c);
    report_blocks_as_size=0;
    check_no_probe_files(root);
    puts("cache native allocation-like st_size with verified byte extent survives get and reopen: PASS");
}
#endif
int main(int argc,char **argv)
{
    CHECK(argc==2);char base[]="/tmp/x4-public-cache-test-XXXXXX";CHECK(mkdtemp(base));
    char root[1024];path_join(root,base,"cache");size_t n;unsigned char *image=read_image(argv[1],&n);
    test_roundtrip(root,image,n);test_lru(root);test_corruption(root,image,n);
    test_record_bound_and_startup_expiry(root);test_validation_cancel(root,image,n);
    test_paths(base,root);test_root_identity(base,root);
    test_protected_lru(root,image,n);test_miss_reasons(root);test_full_scan(root,image,n);
#ifdef X4_DISK_CACHE_NATIVE_FS_TEST
    test_native_probe_and_flags(root);
    test_native_header_fallback(root,image,n);
    test_native_create_failure(root);
    test_native_scan(root);
    test_native_io_and_roundtrip(root);
    test_native_stat_size_is_not_bytes(root);
#endif
    CHECK(!rmdir(root));CHECK(!rmdir(base));free(image);
    printf("catalog disk cache: %u checks passed\n",checks);return 0;
}
