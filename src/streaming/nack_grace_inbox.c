/* SPDX-License-Identifier: GPL-3.0-only */
#include "nack_grace_inbox.h"
#ifndef X4_GRACE_INBOX_HOST_TEST
#include <orbis/libkernel.h>
#else
extern void x4_grace_inbox_test_sleep(unsigned);
/* Deterministic stale-cursor interleaving; absent from production builds. */
extern void x4_grace_inbox_test_after_enqueue_load(X4NackGraceInbox *, uint_fast64_t);
#endif
#include <string.h>

static _Atomic(X4NackGraceInbox *) bound;
static atomic_uint producers;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2 &&
    ATOMIC_INT_LOCK_FREE == 2, "grace inbox requires lock-free atomics");

void x4_nack_grace_inbox_init(X4NackGraceInbox *q)
{
    memset(q,0,sizeof(*q));
    atomic_init(&q->enqueue,0); atomic_init(&q->dequeue,0);
    atomic_init(&q->admitted,0); atomic_init(&q->omitted,0);
    atomic_init(&q->stale,0); atomic_init(&q->epoch,0);
    for(unsigned i=0;i<X4_GRACE_INBOX_CAP;++i) atomic_init(&q->cells[i].ready,false);
}
void x4_nack_grace_inbox_bind(X4NackGraceInbox *q,uint32_t epoch)
{
    /* Session owner: no other binding remains. Epoch is immutable while
     * published, including detach/drain. Zero never admits a request. */
    if(!q || !epoch) return;
    atomic_store_explicit(&q->epoch,epoch,memory_order_relaxed);
    atomic_store_explicit(&bound,q,memory_order_seq_cst);
}
void x4_nack_grace_inbox_unbind(X4NackGraceInbox *q)
{
    X4NackGraceInbox *expected=q;
    if(!q || !atomic_compare_exchange_strong_explicit(&bound,&expected,NULL,
        memory_order_seq_cst,memory_order_seq_cst)) return;
    /* Shutdown owner only. New callbacks seeNULL; entered producers drain
     * before q can be freed or a subsequent session can bind another q. */
    while(atomic_load_explicit(&producers,memory_order_seq_cst)) {
#ifndef X4_GRACE_INBOX_HOST_TEST
        sceKernelUsleep(1000);
#else
        x4_grace_inbox_test_sleep(1000);
#endif
    }
}
bool x4_rtc_nack_grace_control(const X4NackGraceControl *control)
{
    bool ok=false;
    atomic_fetch_add_explicit(&producers,1,memory_order_seq_cst);
    X4NackGraceInbox *q=atomic_load_explicit(&bound,memory_order_seq_cst);
    if(!q || !control) goto leave;
    if((uint32_t)(control->identity>>32)!=atomic_load_explicit(&q->epoch,memory_order_relaxed)) {
        atomic_fetch_add_explicit(&q->stale,1,memory_order_relaxed); goto leave;
    }
    uint_fast64_t at=atomic_load_explicit(&q->enqueue,memory_order_relaxed);
#ifdef X4_GRACE_INBOX_HOST_TEST
    x4_grace_inbox_test_after_enqueue_load(q,at);
#endif
    for(unsigned n=0;n<X4_GRACE_INBOX_TRIES;++n) {
        uint_fast64_t tail=atomic_load_explicit(&q->dequeue,memory_order_acquire);
        uint_fast64_t distance=at-tail; /* unsigned modulo also preserves wrap */
        if(distance>X4_GRACE_INBOX_CAP) {
            /* Another producer and the consumer passed our stale cursor.
             * Refresh within the same bounded budget, not a false full. */
            at=atomic_load_explicit(&q->enqueue,memory_order_relaxed); continue;
        }
        if(distance==X4_GRACE_INBOX_CAP) break;
        if(atomic_compare_exchange_strong_explicit(&q->enqueue,&at,at+1,
            memory_order_relaxed,memory_order_relaxed)) {
            X4NackGraceCell *cell=&q->cells[at&(X4_GRACE_INBOX_CAP-1)];
            cell->control=*control;
            atomic_store_explicit(&cell->ready,true,memory_order_release);
            atomic_fetch_add_explicit(&q->admitted,1,memory_order_relaxed);
            ok=true; break;
        }
    }
    if(!ok) atomic_fetch_add_explicit(&q->omitted,1,memory_order_relaxed);
leave:
    atomic_fetch_sub_explicit(&producers,1,memory_order_seq_cst);
    return ok;
}
bool x4_nack_grace_inbox_peek(X4NackGraceInbox *q,X4NackGraceControl *out)
{
    if(!q || !out) return false;
    uint_fast64_t tail=atomic_load_explicit(&q->dequeue,memory_order_relaxed);
    if(tail==atomic_load_explicit(&q->enqueue,memory_order_acquire)) return false;
    X4NackGraceCell *cell=&q->cells[tail&(X4_GRACE_INBOX_CAP-1)];
    if(!atomic_load_explicit(&cell->ready,memory_order_acquire)) return false;
    *out=cell->control;
    return true;
}
bool x4_nack_grace_inbox_pop(X4NackGraceInbox *q,X4NackGraceControl *out)
{
    if(!x4_nack_grace_inbox_peek(q,out)) return false;
    uint_fast64_t tail=atomic_load_explicit(&q->dequeue,memory_order_relaxed);
    X4NackGraceCell *cell=&q->cells[tail&(X4_GRACE_INBOX_CAP-1)];
    atomic_store_explicit(&cell->ready,false,memory_order_release);
    atomic_store_explicit(&q->dequeue,tail+1,memory_order_release);
    return true;
}
