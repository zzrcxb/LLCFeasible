#pragma once

#include "inline_asm.h"
#include "sugar.h"

static inline void access_stride(u8 *start, ptrdiff_t stride, size_t size) {
    size_t i;
    for (i = 0; i < size; i++) {
        _maccess(start + i * stride);
    }
}

static inline void access_stride_seq(u8 *start, ptrdiff_t stride, size_t size) {
    size_t i;
    for (i = 0; i < size; i++) {
        _maccess(start + i * stride);
        _lfence();
    }
}

static inline void access_array(u8 **addrs, size_t size) {
    size_t i;
    for (i = 0; i < size; i++) {
        _maccess(addrs[i]);
    }
}

static inline void access_array_seq(u8 **addrs, size_t size) {
    size_t i;
    for (i = 0; i < size; i++) {
        _maccess(addrs[i]);
        _lfence();
    }
}

/* if we forward traverse the array, we may access elements after (size - 1) due
to speculation. Backward traversal mitigates this problem, since the index will
underflow and make the reference invalid, blocking speculative accesses.
*/
static inline void access_array_bwd(u8 **addrs, size_t size) {
    size_t i;
    for (i = size; i > 0; i--) {
        _maccess(addrs[i - 1]);
    }
}

static inline void access_array_bwd_seq(u8 **addrs, size_t size) {
    size_t i;
    for (i = size; i > 0; i--) {
        _maccess(addrs[i - 1]);
        _lfence();
    }
}

static inline void write_array(u8 **addrs, size_t size) {
    size_t i;
    for (i = 0; i < size; i++) {
        _mwrite(addrs[i], 0x8);
    }
    _mfence();
}

static inline void write_array_offset(u8 **addrs, size_t size) {
    size_t i;
    for (i = 0; i < size; i++) {
        _mwrite(addrs[i] + sizeof(u8 *), 0x8);
    }
    _mfence();
}

static inline void flush_array(u8 **addrs, size_t size) {
    size_t i;
    for (i = 0; i < size; i++) {
        _clflush(addrs[i]);
    }
}

// eviction pattern from Daniel Gruss
// Rowhammer.js: A Remote Software-Induced Fault Attack in JavaScript
static __always_inline void prime_cands_daniel(u8 **cands, size_t cnt,
                                               size_t repeat, size_t stride,
                                               size_t block) {
    block = _min(block, cnt);
    for (size_t s = 0; s < cnt; s += stride) {
        for (size_t c = 0; c < repeat; c++) {
            if (cnt >= block + s) {
                access_array_bwd(&cands[s], block);
            } else {
                u32 rem = cnt - s;
                access_array_bwd(&cands[s], rem);
                access_array_bwd(cands, block - rem);
            }
        }
    }
}
