/* SPDX-License-Identifier: GPL-3.0-only */
/* Real release worker/parser; transport returns only an in-process fixture. */
#include "../src/auth/release_check.c"
#include <assert.h>

struct X4Http { int value; } fixture_http;
static const char *fixture_body;
static unsigned requests,opens,closes;
X4Http *x4_http_open(int *error){++opens;*error=0;return &fixture_http;}
void x4_http_close(X4Http *http){assert(http==&fixture_http);++closes;}
int x4_http_releases_request(X4Http *http,const char *url,char *out,size_t capacity,
    size_t *length,int *status,const _Atomic int *cancel)
{
    assert(http==&fixture_http&&cancel==&check_state.cancel);
#if X4_PUBLIC
    assert(!strcmp(url,X4_HTTP_RELEASES_URL_PUBLIC));
#else
    assert(!strcmp(url,X4_HTTP_RELEASES_URL_LAB));
#endif
    ++requests;*length=strlen(fixture_body);assert(*length<capacity);
    memcpy(out,fixture_body,*length+1u);*status=200;return 0;
}
static void worker_fixture(const char *body,enum X4UpdateStatus expected,const char *tag)
{
    fixture_body=body;requests=opens=closes=0;check_state.mock_active=false;
    atomic_store(&check_state.status,X4_UPDATE_CHECKING);
    release_check_worker(NULL);
    assert(requests==1&&opens==1&&closes==1&&x4_release_check_status(NULL,0)==expected);
    char output[64];memset(output,'x',sizeof(output));
    bool valid=x4_release_check_tag(output,sizeof(output));
    if(tag)assert(valid&&!strcmp(output,tag));else assert(!valid&&!output[0]);
}
int main(void)
{
    char out[80];memset(out,'x',sizeof(out));
    assert(!x4_release_check_tag(out,sizeof(out))&&!out[0]);
    const char *good[]={"v1.2.1-beta","lab-0.7.58","A_Z-09.1"};
    for(unsigned i=0;i<sizeof(good)/sizeof(*good);++i){
        x4_release_check_set_mock_tag(X4_UPDATE_AVAILABLE,"9.8.7",good[i]);
        assert(x4_release_check_tag(out,sizeof(out))&&!strcmp(out,good[i]));
        size_t length=strlen(good[i]);char tiny[80];memset(tiny,'x',sizeof(tiny));
        assert(!x4_release_check_tag(tiny,length)&&!tiny[0]);
        assert(x4_release_check_tag(tiny,length+1)&&!strcmp(tiny,good[i]));
    }
    char longest[64];memset(longest,'x',63);longest[63]=0;
    x4_release_check_set_mock_tag(X4_UPDATE_AVAILABLE,"9.8.7",longest);
    assert(x4_release_check_tag(out,sizeof(out))&&strlen(out)==63);
    char too_long[65];memset(too_long,'x',64);too_long[64]=0;
    const char *bad[]={NULL,"",too_long,"v9.8.7/extra","v9.8.7?x=1","v9.8.7#fragment",
        "v9.8.7%2Fextra","v9.8.7\\extra","v9.8.7\r\nHeader","v9.8.7 beta","v9.8.7\xc3\xa9"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i){
        x4_release_check_set_mock_tag(X4_UPDATE_AVAILABLE,"9.8.7",bad[i]);
        memset(out,'x',sizeof(out));assert(!x4_release_check_tag(out,sizeof(out))&&!out[0]);
    }
    for(unsigned status=0;status<=X4_UPDATE_FAILED;++status)if(status!=X4_UPDATE_AVAILABLE){
        x4_release_check_set_mock_tag((enum X4UpdateStatus)status,"9.8.7","v9.8.7");
        assert(!x4_release_check_tag(out,sizeof(out))&&!out[0]);
    }
    x4_release_check_set_mock(X4_UPDATE_AVAILABLE,"9.8.7");
    assert(x4_release_check_status(out,sizeof(out))==X4_UPDATE_AVAILABLE&&!strcmp(out,"9.8.7"));
    assert(!x4_release_check_tag(out,sizeof(out))&&!out[0]); /* Existing helper: no invented tag. */
    assert(!x4_release_check_tag(NULL,sizeof(out))&&!x4_release_check_tag(out,0));
#if X4_PUBLIC
    worker_fixture("{\"tag_name\":\"v9.8.7-beta\",\"html_url\":\"https://untrusted.invalid/ignored\"}",X4_UPDATE_AVAILABLE,"v9.8.7-beta");
    worker_fixture("{\"tag_name\":\"v9.8.7?unsafe\"}",X4_UPDATE_AVAILABLE,NULL);
    worker_fixture("{\"tag_name\":\"v0.0.0\"}",X4_UPDATE_LATEST,NULL);
#else
    worker_fixture("[{\"tag_name\":\"v99.0.0\"},{\"tag_name\":\"lab-9.8.7\",\"html_url\":\"https://untrusted.invalid/ignored\"}]",X4_UPDATE_AVAILABLE,"lab-9.8.7");
    worker_fixture("[{\"tag_name\":\"lab-9.8.7?unsafe\"}]",X4_UPDATE_AVAILABLE,NULL);
    worker_fixture("[{\"tag_name\":\"lab-0.0.0\"}]",X4_UPDATE_LATEST,NULL);
#endif
    worker_fixture("{}",X4_UPDATE_FAILED,NULL);
    puts("PASS release tag: real public/lab worker+parser, exact 63-byte ASCII allowlist, no truncation, invalid/missing tag yields generic fallback, no added request or remote URL consumed");
    return 0;
}
