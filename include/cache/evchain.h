#pragma once

#include "inline_asm.h"

typedef struct eviction_chain {
    struct eviction_chain *next;
    struct eviction_chain *prev;
} evchain;

evchain *evchain_build(u8 *addrs[], size_t size);

evchain *evchain_stride(u8 *start, ptrdiff_t stride, size_t size);

static __always_inline evchain *evchain_next(evchain *chain) {
    evchain *next = NULL;
    __asm__ __volatile__("mov (%1), %0\n\t"
                         : "+r"(next)
                         : "r"(&chain->next)
                         : "memory");
    return next;
}

static __always_inline evchain *evchain_prev(evchain *chain) {
    evchain *prev = NULL;
    __asm__ __volatile__("mov (%1), %0\n\t"
                         : "+r"(prev)
                         : "r"(&chain->prev)
                         : "memory");
    return prev;
}

static __always_inline size_t evchain_size(evchain *chain) {
    evchain *start = chain;
    size_t sz = 0;
    if (!chain)
        return 0;

    do {
        chain = evchain_next(chain);
        sz += 1;
    } while (chain != start);
    return sz;
}

static __always_inline void evchain_flush(evchain *chain) {
    evchain *start = chain, *next;
    do {
        next = evchain_next(chain);
        _clflush(chain);
        chain = next;
    } while (chain != start);
}

static __always_inline void evchain_fwd(evchain *chain, size_t cnt) {
    size_t i = 0;
    for (i = 0; i < cnt; i++) {
        chain = evchain_next(chain);
    }
}

static __always_inline void evchain_bck(evchain *chain, size_t cnt) {
    size_t i = 0;
    for (i = 0; i < cnt; i++) {
        chain = evchain_prev(chain);
    }
}

static __always_inline void evchain_fwd_loop(evchain *chain) {
    evchain *start = chain;
    do {
        chain = evchain_next(chain);
    } while (chain != start);
}

static __always_inline void evchain_bck_loop(evchain *chain) {
    evchain *start = chain;
    do {
        chain = evchain_prev(chain);
    } while (chain != start);
}
