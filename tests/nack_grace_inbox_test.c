/* SPDX-License-Identifier: GPL-3.0-only */
#define _DEFAULT_SOURCE
#include "../src/streaming/nack_grace_inbox.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void x4_grace_inbox_test_sleep(unsigned us) { usleep(us); }
static X4NackGraceInbox first,second,wrap;
static atomic_uint advance_after_load;
static unsigned stale_hook_runs;
/* Test-only hook simulates a complete other-producer/consumer interleaving
 * between the original producer's enqueue load and dequeue sample. It does
 * not fabricate queue contents or change a reserved cell underneath a writer. */
void x4_grace_inbox_test_after_enqueue_load(X4NackGraceInbox *q,uint_fast64_t at)
{
    if(!atomic_exchange(&advance_after_load,0)) return;
    ++stale_hook_runs;
    assert(at==atomic_load(&q->enqueue) && at==atomic_load(&q->dequeue));
    X4NackGraceControl other={.identity=(UINT64_C(1)<<32)|1,
        .kind=X4_NACK_GRACE_REQUEST,.first_extended=900001},out;
    assert(x4_rtc_nack_grace_control(&other));
    assert(x4_nack_grace_inbox_pop(q,&out) && out.first_extended==900001);
    assert(atomic_load(&q->enqueue)==at+1 && atomic_load(&q->dequeue)==at+1);
}
static void *producer(void *arg)
{
    unsigned start=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<32;++i) {
        X4NackGraceControl c={.identity=(UINT64_C(1)<<32)|1,
            .kind=X4_NACK_GRACE_REQUEST,.first_extended=start+i,
            .detected_us=100,.deadline_us=45100,.original_mask=1};
        assert(x4_rtc_nack_grace_control(&c));
    }
    return NULL;
}
int main(void)
{
    x4_nack_grace_inbox_init(&first);x4_nack_grace_inbox_init(&second);
    X4NackGraceControl c={.identity=(UINT64_C(1)<<32)|1,.kind=1},out;
    assert(!x4_rtc_nack_grace_control(&c));
    x4_nack_grace_inbox_bind(NULL,1);x4_nack_grace_inbox_unbind(NULL);
    x4_nack_grace_inbox_bind(&first,0);assert(!x4_rtc_nack_grace_control(&c));
    x4_nack_grace_inbox_bind(&first,1);
    assert(!x4_rtc_nack_grace_control(NULL));
    assert(x4_rtc_nack_grace_control(&c));assert(x4_nack_grace_inbox_pop(&first,&out));
    assert(out.identity==c.identity&&!x4_nack_grace_inbox_pop(&first,&out));
    puts("PASS unbound/null/zero-epoch/FIFO publication");
    uint_fast64_t before=atomic_load(&first.enqueue),admitted=atomic_load(&first.admitted);
    uint_fast64_t omitted=atomic_load(&first.omitted);
    c.first_extended=900002; atomic_store(&advance_after_load,1);
    assert(x4_rtc_nack_grace_control(&c));
    assert(stale_hook_runs==1 && atomic_load(&first.enqueue)==before+2);
    assert(atomic_load(&first.admitted)==admitted+2 && atomic_load(&first.omitted)==omitted);
    assert(x4_nack_grace_inbox_pop(&first,&out) && out.first_extended==900002);
    assert(!x4_nack_grace_inbox_pop(&first,&out));
    puts("PASS stale producer cursor refreshes after dequeue advances without false full/omission");
    x4_nack_grace_inbox_unbind(&first);
    x4_nack_grace_inbox_init(&wrap);
    /* An empty queue immediately before its uint64 counter wrap. */
    atomic_store(&wrap.enqueue,UINT64_MAX); atomic_store(&wrap.dequeue,UINT64_MAX);
    x4_nack_grace_inbox_bind(&wrap,1); atomic_store(&advance_after_load,1);
    assert(x4_rtc_nack_grace_control(&c));
    assert(stale_hook_runs==2 && atomic_load(&wrap.enqueue)==1 && atomic_load(&wrap.dequeue)==0);
    assert(atomic_load(&wrap.admitted)==2 && atomic_load(&wrap.omitted)==0);
    assert(x4_nack_grace_inbox_pop(&wrap,&out) && out.first_extended==900002);
    assert(atomic_load(&wrap.dequeue)==1 && !x4_nack_grace_inbox_pop(&wrap,&out));
    /* A valid occupied queue across wrap has tail > at; it must not be
     * mistaken for a stale cursor merely by comparing unsigned magnitudes. */
    x4_nack_grace_inbox_unbind(&wrap); x4_nack_grace_inbox_init(&wrap);
    atomic_store(&wrap.enqueue,UINT64_MAX); atomic_store(&wrap.dequeue,UINT64_MAX);
    x4_nack_grace_inbox_bind(&wrap,1);
    c.first_extended=900003; assert(x4_rtc_nack_grace_control(&c));
    c.first_extended=900004; assert(x4_rtc_nack_grace_control(&c));
    assert(x4_nack_grace_inbox_pop(&wrap,&out) && out.first_extended==900003);
    assert(x4_nack_grace_inbox_pop(&wrap,&out) && out.first_extended==900004);
    assert(atomic_load(&wrap.enqueue)==1 && atomic_load(&wrap.dequeue)==1);
    assert(atomic_load(&wrap.admitted)==2 && atomic_load(&wrap.omitted)==0);
    x4_nack_grace_inbox_unbind(&wrap); x4_nack_grace_inbox_bind(&first,1);
    puts("PASS stale cursor and valid FIFO admission across uint64 counter wrap");
    c.identity=(UINT64_C(2)<<32)|1;
    assert(!x4_rtc_nack_grace_control(&c)&&atomic_load(&first.stale)==1);
    x4_nack_grace_inbox_unbind(&second); /* wrong owner must not detach */
    c.identity=(UINT64_C(1)<<32)|1;assert(x4_rtc_nack_grace_control(&c));
    assert(x4_nack_grace_inbox_pop(&first,&out));
    puts("PASS stale callback epoch and wrong-owner detach");
    for(unsigned i=0;i<256;++i) { c.first_extended=i;assert(x4_rtc_nack_grace_control(&c)); }
    assert(!x4_rtc_nack_grace_control(&c)&&atomic_load(&first.omitted)==1);
    for(unsigned i=0;i<256;++i) { assert(x4_nack_grace_inbox_pop(&first,&out));assert(out.first_extended==i); }
    assert(!x4_nack_grace_inbox_pop(&first,&out));
    puts("PASS bounded256 capacity/omission without overwrite and wrap reuse");
    pthread_t threads[8];
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,producer,(void *)(uintptr_t)(i*32)));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
    bool seen[256]={0};
    for(unsigned i=0;i<256;++i) { assert(x4_nack_grace_inbox_pop(&first,&out));
        assert(out.first_extended<256&&!seen[out.first_extended]);seen[out.first_extended]=true;
        assert(out.original_mask==1&&out.detected_us==100&&out.deadline_us==45100); }
    puts("PASS eight producers coherent POD and one consumer");
    x4_nack_grace_inbox_unbind(&first);assert(!x4_rtc_nack_grace_control(&c));
    x4_nack_grace_inbox_bind(&second,2);
    assert(!x4_rtc_nack_grace_control(&c));c.identity=(UINT64_C(2)<<32)|1;
    assert(x4_rtc_nack_grace_control(&c)&&x4_nack_grace_inbox_pop(&second,&out));
    assert(atomic_load(&second.stale)==1&&atomic_load(&second.omitted)==0);
    x4_nack_grace_inbox_unbind(&second);
    puts("PASS close/new session rejects old epoch and accepts new epoch");
    return 0;
}
