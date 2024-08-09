#pragma once

#include "cache_param.h"
#include "latency.h"
#include "evset.h"
#include "evchain.h"

typedef struct {
    u64 tsc, iters;
    u32 aux, lat, blindspot;
} cache_acc_rec;

typedef struct {
    u64 start, end;
} sender_switched_out;

static inline void pprint_cache_acc_recs(cache_acc_rec *recs, size_t n_recs) {
    for (size_t n = 0; n < n_recs; n++) {
        printf("Recv %2lu: tsc: %lu; aux: %u; iters: %lu; lat: %u; "
               "blind: %u\n",
               n, recs[n].tsc, recs[n].aux, recs[n].iters, recs[n].lat,
               recs[n].blindspot);
    }
}

static __always_inline
i64 probe_skx_sf_evset_para_asm(EVSet *evset, u64 *end_tsc, u32 *aux) {
    u8 **addrs = evset->addrs;
    _force_addr_calc(addrs);
    u64 start = _timer_start();
    __asm__ __volatile__("mov (%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t"
                         "mov 8(%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t"
                         "mov 16(%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t"
                         "mov 24(%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t" // 4
                         "mov 32(%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t"
                         "mov 40(%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t"
                         "mov 48(%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t"
                         "mov 56(%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t" // 8
                         "mov 64(%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t"
                         "mov 72(%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t"
                         "mov 80(%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t"
                         "mov 88(%0), %%r10\n\t"
                         "mov (%%r10), %%r11\n\t" // 12
                         // ICELAKE
                        "mov 96(%0), %%r10\n\t"
                        "mov (%%r10), %%r11\n\t"
                        "mov 104(%0), %%r10\n\t"
                        "mov (%%r10), %%r11\n\t"
                        "mov 112(%0), %%r10\n\t"
                        "mov (%%r10), %%r11\n\t"
                        "mov 120(%0), %%r10\n\t"
                        "mov (%%r10), %%r11\n\t"  // 16
                         ::"r"(addrs)
                         : "r10", "r11", "memory");
    *end_tsc = _timer_end_aux(aux);
    return *end_tsc - start;
}

static __always_inline
i64 probe_skx_sf_evset_para_noasm(EVSet *evset, u64 *end_tsc, u32 *aux) {
    u8 **addrs = evset->addrs;
    _force_addr_calc(addrs);
    u64 start = _timer_start();
    access_array_bwd(addrs, SF_ASSOC);
    *end_tsc = _timer_end_aux(aux);
    return *end_tsc - start;
}

static __always_inline
i64 probe_skx_sf_evset_ptr_chase(EVSet *evset, u64 *end_tsc, u32 *aux) {
    evchain *chain = (evchain *)evset->addrs[0];
    u64 start = _timer_start();
    evchain_fwd_loop(chain);
    *end_tsc = _timer_end_aux(aux);
    return *end_tsc - start;
}

static __always_inline
i64 probe_skx_sf_evset_para(EVSet *evset, u64 *end_tsc, u32 *aux) {
    return probe_skx_sf_evset_para_asm(evset, end_tsc, aux);
}

void prime_skx_sf_evset_para(EVSet *evset, u32 arr_repeat, u32 l2_repeat);

void prime_evchain_prime_scope(evchain *ptr);

void prime_skx_sf_evset_ps_sense(evchain *chain1, evchain *chain2,
                                 bool prime_sense, EVSet *lower);

void prime_skx_sf_evset_ps_flush(EVSet *evset, evchain *chain, u32 arr_repeat,
                                 u32 l2_repeat);

i64 calibrate_para_probe_lat(u8 *target, EVSet *evset, u32 arr_repeat,
                             u32 l2_repeat, double bad_thresh_ratio);

i64 calibrate_chase_probe_lat(u8 *target, EVSet *evset, u32 arr_repeat,
                              u32 l2_repeat, double bad_thresh_ratio);
