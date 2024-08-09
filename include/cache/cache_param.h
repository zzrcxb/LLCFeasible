#pragma once

#include "inline_asm.h"
#include "libpt.h"

extern bool __has_hugepage;

static inline void cache_use_hugepage() {
    __has_hugepage = true;
}

static inline void cache_disable_hugepage() {
    __has_hugepage = false;
}

// 64-byte cache-line size is the most common option on x86-64 these days;
// therefore, we hardcode this as a macro.
// If we detect a violation, a warning will be raised.
#define CL_SHIFT 6u
#define CL_SIZE (1ul << CL_SHIFT)
#define CL_MASK (CL_SIZE - 1)

#define SF_ASSOC 12

#ifdef ICELAKE
#undef SF_ASSOC
#define SF_ASSOC 16
#endif

#define MAXIMUM_CACHES (8u)

typedef enum {
    CACHE_INVAL = 0u,
    CACHE_DATA,
    CACHE_INST,
    CACHE_UNIF
} cache_type;

typedef struct {
    u32 level;
    cache_type type;
    bool self_init, fully_assoc, inclusive, complex_idx;
    u32 line_size, n_ways, n_sets, n_slices; // all sizes are in bytes
    u64 size;
    u32 num_cl_bits, num_set_idx_bits;
} cache_param;

typedef struct {
    u32 num_caches, verbose;
    cache_param caches[MAXIMUM_CACHES];
} cpu_caches;

// globally shared cache info
extern cpu_caches detected_caches;
extern cache_param *detected_l1d, *detected_l1i, *detected_l2, *detected_l3;

int detect_cpu_caches(cpu_caches *caches);

cache_param *find_cpu_cache(cpu_caches *_this, u32 level, cache_type type);

void find_common_caches(cpu_caches *_this, cache_param **l1i, cache_param **l1d,
                        cache_param **l2, cache_param **l3);

void set_cache_num_slices(cache_param *param, u32 n_slices);

void pprint_cache_param(cache_param *param);

static __always_inline size_t cache_uncertainty(cache_param *param) {
    size_t bits_under_ctrl = __has_hugepage ? HUGE_PAGE_SHIFT : PAGE_SHIFT;
    size_t set_bits_under_ctrl = bits_under_ctrl - param->num_cl_bits;
    if (set_bits_under_ctrl >= param->num_set_idx_bits)
        return param->n_slices;
    else
        return (1ull << (param->num_set_idx_bits - set_bits_under_ctrl)) *
               param->n_slices;
}

static __always_inline size_t cache_congruent_stride(cache_param *param) {
    return 1ull << (param->num_set_idx_bits + param->num_cl_bits);
}

static inline bool check_power_of_two(u64 val) {
    return _count_ones(val) == 1;
}

static inline u32 log2_ceil(u64 data) {
    u32 cnt = 0;
    while ((1ull << cnt) < data) {
        cnt += 1;
    }
    return cnt;
}
