/*
*
* BSD 2-Clause License
* 
* Copyright (c) 2018, Taymindis Woon
* All rights reserved.
* 
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
* 
* * Redistributions of source code must retain the above copyright notice, this
*   list of conditions and the following disclaimer.
* 
* * Redistributions in binary form must reproduce the above copyright notice,
*   this list of conditions and the following disclaimer in the documentation
*   and/or other materials provided with the distribution.
* 
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#if defined __GNUC__ || defined __CYGWIN__ || defined __MINGW32__ || defined __APPLE__

#include <pthread.h>
#include <sched.h>

#define __LFQ_VAL_COMPARE_AND_SWAP __sync_val_compare_and_swap
#define __LFQ_BOOL_COMPARE_AND_SWAP __sync_bool_compare_and_swap
#define __LFQ_FETCH_AND_ADD __sync_fetch_and_add
#define __LFQ_ADD_AND_FETCH __sync_add_and_fetch
#define __LFQ_YIELD_THREAD sched_yield
#define __LFQ_SYNC_MEMORY __sync_synchronize

#else

#include <Windows.h>
#ifdef _WIN64
inline BOOL __SYNC_BOOL_CAS(LONG64 volatile *dest, LONG64 input, LONG64 comparand) {
    return InterlockedCompareExchangeNoFence64(dest, input, comparand) == comparand;
}
#define __LFQ_VAL_COMPARE_AND_SWAP(dest, comparand, input) \
    InterlockedCompareExchangeNoFence64((LONG64 volatile *)dest, (LONG64)input, (LONG64)comparand)
#define __LFQ_BOOL_COMPARE_AND_SWAP(dest, comparand, input) \
    __SYNC_BOOL_CAS((LONG64 volatile *)dest, (LONG64)input, (LONG64)comparand)
#define __LFQ_FETCH_AND_ADD InterlockedExchangeAddNoFence64
#define __LFQ_ADD_AND_FETCH InterlockedAddNoFence64
#define __LFQ_SYNC_MEMORY MemoryBarrier

#else
#ifndef asm
#define asm __asm
#endif
inline BOOL __SYNC_BOOL_CAS(LONG volatile *dest, LONG input, LONG comparand) {
    return InterlockedCompareExchangeNoFence64(dest, input, comparand) == comparand;
}
#define __LFQ_VAL_COMPARE_AND_SWAP(dest, comparand, input) \
    InterlockedCompareExchangeNoFence64((LONG volatile *)dest, (LONG)input, (LONG)comparand)
#define __LFQ_BOOL_COMPARE_AND_SWAP(dest, comparand, input) \
    __SYNC_BOOL_CAS((LONG volatile *)dest, (LONG)input, (LONG)comparand)
#define __LFQ_FETCH_AND_ADD InterlockedExchangeAddNoFence
#define __LFQ_ADD_AND_FETCH InterlockedAddNoFence
#define __LFQ_SYNC_MEMORY() asm mfence

#endif
#include <windows.h>
#define __LFQ_YIELD_THREAD SwitchToThread
#endif

#include "lfqueue.h"
#define DEF_LFQ_ASSIGNED_SPIN 2048

static lfqueue_cas_node_t* __lfq_assigned(lfqueue_t *);

static void *
dequeue_(lfqueue_t *lfqueue) {
    lfqueue_cas_node_t *head, *next;
    void *val;
    for (;;) {
        head = __LFQ_VAL_COMPARE_AND_SWAP(&lfqueue->head, -1, -1);
        next = head->next;
        if (__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->head, head, head)) {
            if (__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->tail, head, head) && next == NULL) {
                return NULL;
            }
            else {
                val = next->value;
                if (__LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->head, head, next)) {
                    break;
                }
            }
        }
    }
    return val;
}

static int
enqueue_(lfqueue_t *lfqueue, void* value) {
    lfqueue_cas_node_t *tail, *node;
    node = __lfq_assigned(lfqueue);
    node->value = value;
    node->next = NULL;
    for (;;) {
        tail = __LFQ_VAL_COMPARE_AND_SWAP(&lfqueue->tail, -1, -1);
        if (__LFQ_BOOL_COMPARE_AND_SWAP(&tail->next, NULL, node)) {
            // compulsory swap as tail->next is no NULL anymore, it has fenced on other thread
            __LFQ_BOOL_COMPARE_AND_SWAP(&lfqueue->tail, tail, node);
            return 0;
        }
    }
    return -1;
}

int
lfqueue_init(lfqueue_t *lfqueue, size_t queue_sz) {
    size_t i;
    // To assume concurrent access * concurrent access at 1 time
    lfqueue_cas_node_t *base = malloc(sizeof(lfqueue_cas_node_t));
    if (base == NULL) {
        return errno;
    }
    base->value = NULL;
    base->next = NULL;
    lfqueue->head = lfqueue->tail = base;
    lfqueue->size = 0;
    lfqueue->capacity = queue_sz;
    lfqueue->root = lfqueue->chain = malloc(queue_sz * sizeof(lfqueue_cas_node_t));
    for (i = 0; i < queue_sz - 1; i++) {
        lfqueue->chain[i].p = (lfqueue_cas_node_t*) (lfqueue->chain + i);
		lfqueue->chain[i].next = (lfqueue_cas_node_t*) (lfqueue->chain + i + 1);
    }

	lfqueue->chain[i].p = (lfqueue_cas_node_t*) (lfqueue->chain + i);
	lfqueue->chain[i].next = (lfqueue_cas_node_t*) lfqueue->chain; // Back to sentinant

    return 0;
}

void
lfqueue_destroy(lfqueue_t *lfqueue) {
    void* p;
    int i;
    while ((p = lfqueue_deq(lfqueue))) {
        free(p);
    }
    free(lfqueue->root);
    lfqueue->size = 0;
}

int
lfqueue_enq(lfqueue_t *lfqueue, void *value) {
    if ( __LFQ_FETCH_AND_ADD(&lfqueue->size, 1) >= lfqueue->capacity) {
        // Rest the thread for other enqueue
        __LFQ_ADD_AND_FETCH(&lfqueue->size, -1);        
		__LFQ_YIELD_THREAD();
		return -1;
    }
    if ( enqueue_(lfqueue, value) ) {
		__LFQ_ADD_AND_FETCH(&lfqueue->size, -1);
		__LFQ_YIELD_THREAD();
		return -1;
    }   
    return 0;
}

void*
lfqueue_deq(lfqueue_t *lfqueue) {
    void *v;
    __LFQ_SYNC_MEMORY();
    if (__LFQ_ADD_AND_FETCH(&lfqueue->size, 0) && (v = dequeue_(lfqueue))) {
        __LFQ_FETCH_AND_ADD(&lfqueue->size, -1);
		return v;
    }
    return NULL;
}

size_t
lfqueue_size(lfqueue_t *lfqueue) {
    return __LFQ_ADD_AND_FETCH(&lfqueue->size, 0);
}

static lfqueue_cas_node_t* __lfq_assigned(lfqueue_t *q) {
	lfqueue_cas_node_t *next, *new_update_node, *p;
	new_update_node = p = (lfqueue_cas_node_t*)__LFQ_VAL_COMPARE_AND_SWAP(&q->chain, -1, -1);	
	int cas_assign_spin_cycle = 0;
	do {
		/** make spin 2048 only if still cannot**/
		if( cas_assign_spin_cycle++ == DEF_LFQ_ASSIGNED_SPIN ) {
			cas_assign_spin_cycle = 0;
			__LFQ_YIELD_THREAD();
		}
		p = new_update_node;
		next = new_update_node->next;
	} while ( (new_update_node = (lfqueue_cas_node_t*)__LFQ_VAL_COMPARE_AND_SWAP(&q->chain, new_update_node, next)) != p );
	return p;
}


