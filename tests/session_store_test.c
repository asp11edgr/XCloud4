/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/auth/session_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Entire SaveData layer stays in memory. No credential-shaped byte string is
 * printed or written to the host filesystem, even on an assertion failure. */
typedef struct {
    unsigned char bytes[16400];size_t size;
    unsigned mounts,unmounts,writes,erases;
    bool directory,mounted;int mount_error;
    bool mount_policy;unsigned searches,attempts;
    unsigned modes[4],blocks[4];int results[4];unsigned result_count;
} Memory;
static const char fake_a[]="FAKE_REFRESH_ALPHA_29";
static const char fake_b[]="FAKE_REFRESH_BETA_29";
/* This extern exposes only the real shared policy under the test define. */
extern int x4_session_store_test_mount_policy(void *,bool,int (*)(void *,bool *),int (*)(void *,unsigned,unsigned));
extern const char *x4_session_store_test_directory(void);
static int exists(void *c,bool *yes){Memory *m=c;++m->searches;*yes=m->directory;return 0;}
static int mount_attempt(void *c,unsigned mode,unsigned blocks)
{
    Memory *m=c;assert(!m->mounted);
    /* Emulate the old compiledSdkVer rejection even if a future regression
     * tries the unsupported bit. No actual credential is logged. */
    if(mode&0x20u)return (int32_t)UINT32_C(0x809f0000);
    assert(m->attempts<4);unsigned n=m->attempts++;
    m->modes[n]=mode;m->blocks[n]=blocks;++m->mounts;
    int rc=n<m->result_count?m->results[n]:
        (mode&4u)?(m->directory?(int32_t)UINT32_C(0x809f0007):0):
        (!m->directory?(int32_t)UINT32_C(0x809f0008):0);
    if((uint32_t)rc==UINT32_C(0x809f0007))m->directory=true;
    if((uint32_t)rc==UINT32_C(0x809f0008))m->directory=false;
    if(!rc){m->mounted=true;if(mode&4u)m->directory=true;}
    return rc;
}
static int mount_save(void *c,bool create){Memory *m=c;if(m->mount_policy)return x4_session_store_test_mount_policy(m,create,exists,mount_attempt);assert(!m->mounted);++m->mounts;if(m->mount_error)return m->mount_error;if(!m->directory&&!create)return -1;m->mounted=true;if(create)m->directory=true;return 0;}
static int read_save(void *c,void *out,size_t cap,size_t *got){Memory *m=c;assert(m->mounted);if(!m->size){*got=0;return 1;}if(m->size>cap)return -1;memcpy(out,m->bytes,m->size);*got=m->size;return 0;}
static int write_save(void *c,const void *in,size_t size){Memory *m=c;assert(m->mounted&&size<=sizeof(m->bytes));memcpy(m->bytes,in,size);m->size=size;++m->writes;return 0;}
static int erase_save(void *c){Memory *m=c;assert(m->mounted);memset(m->bytes,0,sizeof(m->bytes));m->size=0;++m->erases;assert(m->size==0);return 0;}
static int unmount_save(void *c){Memory *m=c;assert(m->mounted);m->mounted=false;++m->unmounts;return 0;}
static X4SessionStore *fresh(Memory *m){X4SessionStoreTestBackend b={m,exists,mount_save,read_save,write_save,erase_save,unmount_save};return x4_session_store_test_new(&b);}
static void view(X4SessionStore *s,bool enabled,bool available){X4SessionStoreSnapshot v={0};x4_session_store_snapshot(s,&v);assert(v.enabled==enabled&&v.available==available);}
static void mount_case(bool directory,int first,int second,unsigned attempts,unsigned mode0,unsigned mode1,unsigned blocks1,bool succeeds)
{
    Memory m={0};m.directory=directory;m.mount_policy=true;
    if(attempts>1){m.results[0]=first;m.results[1]=second;m.result_count=attempts;}
    X4SessionStore *s=fresh(&m);assert(s);
    int rc=x4_session_store_set_enabled(s,true,fake_a);
    assert(m.searches==1&&m.attempts==attempts&&m.modes[0]==mode0&&m.blocks[0]==96);
    for(unsigned i=0;i<m.attempts;++i)assert(!(m.modes[i]&0x20u));
    if(attempts>1)assert(m.modes[1]==mode1&&m.blocks[1]==blocks1);
    if(succeeds){assert(!rc&&m.writes==1&&!m.mounted&&m.unmounts==1);view(s,true,true);}
    else {assert((uint32_t)rc==UINT32_C(0x809f0000)&&!m.writes&&!m.mounted&&!m.unmounts);view(s,true,false);}
    assert(!x4_session_store_free(s));memset(&m,0,sizeof(m));
}
static void test_real_mount_policy(void)
{
    Memory legacy={0};assert((uint32_t)mount_attempt(&legacy,0x22,96)==UINT32_C(0x809f0000));
    mount_case(false,0,0,1,6,0,0,true);
    puts("PASS real mount policy first use: DirNameSearch absent -> RDWR|CREATE(4),96blocks,no CREATE2");
    mount_case(true,0,0,1,2,0,0,true);
    puts("PASS real mount policy existing directory: RDWR,96blocks");
    mount_case(false,(int32_t)UINT32_C(0x809f0007),0,2,6,2,96,true);
    puts("PASS real mount policy CREATE/EXISTS race: one RDWR retry");
    mount_case(true,(int32_t)UINT32_C(0x809f0008),0,2,2,6,96,true);
    puts("PASS real mount policy RDWR/NOT_FOUND race: one CREATE retry");
    mount_case(false,(int32_t)UINT32_C(0x809f0000),0,2,6,6,1024,true);
    puts("PASS real mount policy CREATE PARAMETER at96blocks: one1024block retry");
    mount_case(false,(int32_t)UINT32_C(0x809f0000),(int32_t)UINT32_C(0x809f0000),2,6,6,1024,false);
    puts("PASS real mount policy both block sizes fail: unavailable,no write or plaintext fallback");
}
int main(void)
{
    const char *directory=x4_session_store_test_directory();
    assert(!strcmp(directory,"XCLOUD4SESSION"));
    for(const char *p=directory;*p;++p)assert((*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9'));
    puts("PASS SaveData directory XCLOUD4SESSION contains only uppercase letters and digits");
    test_real_mount_policy();
    Memory m={0};char restored[X4_SESSION_REFRESH_CAPACITY]={0};X4SessionStore *s=fresh(&m);assert(s);
    view(s,false,true);assert(x4_session_store_restore(s,restored,sizeof(restored))==0);
    assert(x4_session_store_rotate(s,fake_a)==0&&m.writes==0&&m.mounts==0);
    /* A cancelled warning never invokes set_enabled(true). */
    bool warning_accepted=false;if(warning_accepted)assert(x4_session_store_set_enabled(s,true,fake_a)==0);
    view(s,false,true);assert(m.writes==0);
    puts("PASS default disabled and cancelled consent: no save or mount");
    warning_accepted=true;if(warning_accepted)assert(x4_session_store_set_enabled(s,true,fake_a)==0);
    view(s,true,true);assert(m.writes==1&&!m.mounted&&m.mounts==m.unmounts);
    x4_session_store_free(s);s=fresh(&m);
    assert(x4_session_store_restore(s,restored,sizeof(restored))==1);
    assert(strcmp(restored,fake_a)==0&&!m.mounted);view(s,true,true);
    puts("PASS accepted consent stores only in simulated SaveData; reopening restores");
    assert(x4_session_store_rotate(s,fake_b)==0&&m.writes==2);
    x4_session_store_free(s);s=fresh(&m);
    assert(x4_session_store_restore(s,restored,sizeof(restored))==1&&strcmp(restored,fake_b)==0);
    puts("PASS rotated refresh replaces previous credential");
    assert(x4_session_store_erase(s)==0);view(s,false,true);assert(m.size==0&&!m.mounted);
    x4_session_store_free(s);s=fresh(&m);assert(x4_session_store_restore(s,restored,sizeof(restored))==0);
    assert(x4_session_store_set_enabled(s,true,fake_a)==0);
    assert(x4_session_store_set_enabled(s,false,NULL)==0);assert(m.size==0&&!m.mounted);
    puts("PASS logout and disabling erase and verify absence");
    x4_session_store_free(s);memset(&m,0,sizeof(m));m.mount_error=-99;s=fresh(&m);
    assert(x4_session_store_set_enabled(s,true,fake_a)==-99);view(s,true,false);
    assert(m.writes==0&&!m.mounted);x4_session_store_free(s);
    puts("PASS mount failure marks unavailable without writing or plaintext fallback");
    memset(&m,0,sizeof(m));s=fresh(&m);assert(x4_session_store_set_enabled(s,true,fake_a)==0);x4_session_store_free(s);
    m.bytes[16]^=1;s=fresh(&m);assert(x4_session_store_restore(s,restored,sizeof(restored))==0);
    assert(m.size==0&&restored[0]==0&&!m.mounted);view(s,false,true);
    puts("PASS corrupt record discarded and no remembered sign-in remains");
    x4_session_store_free(s);memset(restored,0,sizeof(restored));memset(&m,0,sizeof(m));
    puts("PASS session store: all fake credentials remained inside memory SaveData fixture");
    return 0;
}
