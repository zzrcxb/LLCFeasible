#pragma once

#include "inline_asm.h"
#include "cache/cache_param.h"

static const u32 DEF_LATENCY_CALI = 0x200;

typedef struct {
    i64 l1d, l2, l3, dram;
    i64 l1d_thresh, l2_thresh, l3_thresh, interrupt_thresh;
} cache_latencies;

extern cache_latencies detected_cache_lats;

bool cache_latencies_sanity_check(cache_latencies *lats);

bool cache_latencies_detect(cache_latencies *lats, cpu_caches *caches);

void cache_latencies_pprint(cache_latencies *lats);

i64 detect_l1d_latency(u32 repeats);

i64 detect_l2_latency(cpu_caches *caches, u32 repeats);

i64 detect_l3_latency(cpu_caches *caches, u32 repeats);

i64 detect_dram_latency(u32 repeats);

static inline i64 calc_hit_threshold(i64 hit_lat, i64 miss_lat) {
    return (3 * hit_lat + 2 * miss_lat) / 5;
}

static inline i64 detect_l1d_thresh(cpu_caches *caches, u32 repeats) {
    i64 hit = detect_l1d_latency(repeats);
    i64 miss = detect_l2_latency(caches, repeats);
    if (hit < 0 || miss < 0) {
        return -1;
    } else {
        return calc_hit_threshold(hit, miss);
    }
}

static inline i64 detect_l2_thresh(cpu_caches *caches, u32 repeats) {
    i64 hit = detect_l2_latency(caches, repeats);
    i64 miss = detect_l3_latency(caches, repeats);
    if (hit < 0 || miss < 0) {
        return -1;
    } else {
        return calc_hit_threshold(hit, miss);
    }
}

static inline i64 detect_l3_thresh(cpu_caches *caches, u32 repeats) {
    i64 hit = detect_l3_latency(caches, repeats);
    i64 miss = detect_dram_latency(repeats);
    if (hit < 0 || miss < 0) {
        return -1;
    } else {
        return calc_hit_threshold(hit, miss);
    }
}

static int compare_lats(const void *lhs, const void *rhs) {
    const i64 *l = (i64 *)lhs, *r = (i64 *)rhs;
    if (*l < *r) return -1;
    if (*l > *r) return 1;
    return 0;
}

static i64 find_median_lats(i64 *lats, size_t cnt) {
    if (cnt == 0) return -1;
    if (cnt == 1) return lats[0];

    _sort(lats, cnt, sizeof(lats[0]), compare_lats);
    if (cnt % 2) {
        return lats[cnt / 2];
    } else {
        return (lats[cnt / 2] + lats[cnt / 2 - 1]) / 2;
    }
}
