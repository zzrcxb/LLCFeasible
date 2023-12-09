#include "bitwise.h"
#include "sugar.h"
#include "cache/latency.h"
#include "cache/access_seq.h"

cache_latencies detected_cache_lats = {0};

static const bool _dbg = false;

#define _dprintf(...)                                                          \
    do {                                                                       \
        if (_dbg) {                                                            \
            fprintf(stderr, __VA_ARGS__);                                      \
        }                                                                      \
    } while (0)

static const u32 N_OFFSETS = 0x10, OFFSET_STEP = PAGE_SIZE / N_OFFSETS;

static i64 check_and_calc_median(i64 *lats, size_t cnt, size_t repeats) {
    if (cnt < repeats / 2) {
        _error("Too many context switches\n");
        return -1;
    } else {
        return find_median_lats(lats, cnt);
    }
}

i64 detect_l1d_latency(u32 repeats) {
    u32 aux_before, aux_after, i;
    u8 *target = _calloc(8, 1);
    i64 *lats = _calloc(repeats, sizeof(lats[0])), median = -1, lat_cnt = 0;
    if (!target || !lats) {
        goto err;
    }

    _maccess(target);
    for (i = 0; i < repeats; i++) {
        _rdtscp_aux(&aux_before);
        i64 lat = _time_maccess(target);
        _rdtscp_aux(&aux_after);
        if (aux_after == aux_before) {
            lats[lat_cnt++] = lat;
        }
    }
    median = check_and_calc_median(lats, lat_cnt, repeats);

err:
    _free(target);
    _free(lats);
    return median;
}

static i64 _detect_mid_level_latency(cache_param *c, u32 repeats) {
    u32 aux_before, aux_after, i, j, n_rep_offset = repeats / N_OFFSETS;
    size_t ev_sz = 2 * c->n_ways * cache_uncertainty(c);
    // two extra pages, one for target, one for alignment
    size_t buf_sz = (ev_sz + 2) * PAGE_SIZE;
    u8 *buf = _calloc(buf_sz, 1), *page, *target, *tlb_target, *ev_start;
    i64 *lats = _calloc(repeats, sizeof(lats[0])), median = -1, lat_cnt = 0;
    if (!buf || !lats) {
        goto err;
    }

    page = (u8 *)_ALIGN_UP(buf, PAGE_SHIFT);
    for (i = 0; i < repeats; i++) {
        u32 oid = (i / n_rep_offset);
        target = page + (oid % N_OFFSETS) * OFFSET_STEP;
        tlb_target = page + ((oid + 1) % N_OFFSETS) * OFFSET_STEP;
        ev_start = target + PAGE_SIZE;

        _rdtscp_aux(&aux_before);
        _mwrite(target, 0x1);
        _mfence();
        _lfence();
        for (j = 0; j < 5; j++) {
            access_stride(ev_start, PAGE_SIZE, ev_sz);
        }

        _lfence();
        _maccess(tlb_target);
        i64 lat = _time_maccess(target);

        _rdtscp_aux(&aux_after);
        if (aux_after == aux_before) {
            lats[lat_cnt++] = lat;
        }
    }
    median = check_and_calc_median(lats, lat_cnt, repeats);

err:
    _free(buf);
    _free(lats);
    return median;
}

i64 detect_l2_latency(cpu_caches *caches, u32 repeats) {
    cache_param *l1d = find_cpu_cache(caches, 1, CACHE_DATA);
    if (l1d) {
        return _detect_mid_level_latency(l1d, repeats);
    } else {
        _error("L1d cache parameters not found, cannot detect L2 latency!\n");
        return -1;
    }
}

i64 detect_l3_latency(cpu_caches *caches, u32 repeats) {
    cache_param *l2 = find_cpu_cache(caches, 2, CACHE_UNIF);
    if (l2) {
        return _detect_mid_level_latency(l2, repeats);
    } else {
        _error("L2 cache parameters not found, cannot detect L3 latency!\n");
        return -1;
    }
}

i64 detect_dram_latency(u32 repeats) {
    u8 *target = _calloc(8, 1);
    u32 aux_before, aux_after, i;
    i64 *lats = _calloc(repeats, sizeof(lats[0])), median = -1, lat_cnt = 0;
    if (!target || !lats) {
        goto err;
    }

    _maccess(target);
    for (i = 0; i < repeats; i++) {
        _rdtscp_aux(&aux_before);
        _clflush(target);
        i64 lat = _time_maccess(target);
        _rdtscp_aux(&aux_after);
        if (aux_after == aux_before) {
            lats[lat_cnt++] = lat;
        }
    }
    median = check_and_calc_median(lats, lat_cnt, repeats);

err:
    _free(target);
    _free(lats);
    return median;
}

bool cache_latencies_sanity_check(cache_latencies *lats) {
    bool failed =
        lats->l1d <= 0 || lats->l2 <= 0 || lats->l3 <= 0 || lats->dram <= 0;

    failed |= lats->l2 <= lats->l1d * 11 / 10;
    failed |= lats->l3 <= lats->l2 * 14 / 10;
    failed |= lats->dram <= lats->l3 * 16 / 10;
    return !failed;
}

bool cache_latencies_detect(cache_latencies *lats, cpu_caches *caches) {
    bool passed = false;
    for (u32 r = 0; r < 5 && !passed; r++) {
        lats->l1d = detect_l1d_latency(DEF_LATENCY_CALI);
        lats->l2 = detect_l2_latency(caches, DEF_LATENCY_CALI);
        lats->l3 = detect_l3_latency(caches, DEF_LATENCY_CALI);
        lats->dram = detect_dram_latency(DEF_LATENCY_CALI);

        if (cache_latencies_sanity_check(lats)) {
            passed = true;
        } else {
            _dprintf("Failed sanity check!\n");
        }
    }

    if (!passed) {
        _error("Failed to detect cache latencies after 5 retries\n");
        cache_latencies_pprint(lats);
        return true;
    }

    lats->l1d_thresh = calc_hit_threshold(lats->l1d, lats->l2);
    lats->l2_thresh = calc_hit_threshold(lats->l2, lats->l3);
    lats->l3_thresh = calc_hit_threshold(lats->l3, lats->dram);
    lats->interrupt_thresh = lats->dram * 5;
    return false;
}

void cache_latencies_pprint(cache_latencies *lats) {
    _info("Cache latencies: L1D: %ld; L2: %ld; L3: %ld; DRAM: %ld\n",
          lats->l1d, lats->l2, lats->l3, lats->dram);
    _info("Cache hit thresholds: L1D: %ld; L2: %ld; L3: %ld\n",
          lats->l1d_thresh, lats->l2_thresh, lats->l3_thresh);
    _info("Latency upper bound for interrupts: %ld\n\n", lats->interrupt_thresh);
}
