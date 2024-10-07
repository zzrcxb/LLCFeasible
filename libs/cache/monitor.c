#include "cache/monitor.h"
#include "cache/access_seq.h"
#include "cache/evchain.h"
#include "sync.h"
#include <stdlib.h>

void prime_skx_sf_evset_para(EVSet *evset, u32 arr_repeat, u32 l2_repeat) {
    EVTestConfig *tconf = &evset->config->test_config;
    // tconf->traverse(evset->addrs, evset->size, tconf);
    // _lfence();
    // use the MT traverse can achieve a high eviction rate, but it's slower

    access_array_bwd(evset->addrs, evset->size);
    _lfence();
    if (tconf->lower_ev) {
        for (u32 i = 0; i < l2_repeat; i++) {
            generic_evset_traverse(tconf->lower_ev);
        }
    }

    u32 sz = _min(evset->size, SF_ASSOC);
    _lfence();
    write_array_offset(evset->addrs, sz); // promote to exclusive
    for (u32 i = 0; i < arr_repeat; i++) {
        access_array(evset->addrs, sz);
    }
    _lfence();
}

// from the PRIME+SCOPE implementation
#if defined (SKYLAKE) || defined (CASCADE)
void prime_evchain_prime_scope(evchain *ptr) {
    __asm__ __volatile__("mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         :
                         : "c"(ptr)
                         : "cc", "memory");
}
#elif defined (ICELAKE)
void prime_evchain_prime_scope(evchain *ptr) {
    __asm__ __volatile__("mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         :
                         : "c"(ptr)
                         : "cc", "memory");
}
#else
void prime_evchain_prime_scope(evchain *ptr) {
    _error("Prime+Scope's prime chain is not implemented!\n");
    exit(EXIT_FAILURE);
}
#endif

void prime_skx_sf_evset_ps_sense(evchain *chain1, evchain *chain2,
                                 bool prime_sense, EVSet *lower) {
    if (lower) {
        generic_evset_traverse(lower);
    }

    if (prime_sense) {
        prime_evchain_prime_scope(chain2);
    } else {
        prime_evchain_prime_scope(chain1);
    }
    _lfence();
}

void prime_skx_sf_evset_ps_flush(EVSet *evset, evchain *chain, u32 arr_repeat,
                                 u32 l2_repeat) {
    prime_skx_sf_evset_para(evset, arr_repeat, l2_repeat);
    _lfence();
    flush_evset(evset);
    _lfence();
    prime_evchain_prime_scope(chain);
    _lfence();
}

// from P+S
static void skx_sf_prime_ps(evchain *ptr) {
    __asm__ __volatile__("mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         "mfence;"
                         "movq (%%rcx), %%rcx;"
                         "movq (%%rcx), %%rcx;"
                         :
                         : "c"(ptr)
                         : "cc", "memory");
}

typedef i64 (*probe_func)(EVSet *evset, u64 *end_tsc, u32 *aux);

static i64 calibrate_probe_lat(u8 *target, EVSet *evset, u32 arr_repeat,
                               u32 l2_repeat, double bad_thresh_ratio,
                               const char *name, probe_func pfunc) {
    helper_thread_ctrl *hctrl = evset->config->test_config.hctrl;
    const u64 n_repeat = 1000;
    i64 *no_acc_lats = calloc(n_repeat, sizeof(no_acc_lats[0]));
    i64 *acc_lats = calloc(n_repeat, sizeof(acc_lats[0]));
    if (!no_acc_lats || !acc_lats) {
        _error("Failed to calibrate grp access latency\n");
        return 0;
    }

    // calibrate no access/access
    flush_evset(evset);
    _lfence();
    u64 end_tsc;
    for (u64 r = 0; r < n_repeat * 2;) {
        u32 aux_before, aux_after;
        _rdtscp_aux(&aux_before);
        _lfence();
        prime_skx_sf_evset_para(evset, arr_repeat, l2_repeat);
        _lfence();
        if (r % 2) {
            helper_thread_read_single(target, hctrl);
        }
        _lfence();
        u64 lat = pfunc(evset, &end_tsc, &aux_after);
        if (aux_before == aux_after &&
            lat < detected_cache_lats.interrupt_thresh) {
            if (r % 2) {
                acc_lats[r / 2] = lat;
            } else {
                no_acc_lats[r / 2] = lat;
            }
            r += 1;
        }
        _lfence();
    }

    i64 no_acc_lat = find_median_lats(no_acc_lats, n_repeat);
    i64 acc_lat = find_median_lats(acc_lats, n_repeat);
    i64 thresh = (5 * acc_lat + 4 * no_acc_lat) / 9;
    u32 otc = 0, utc = 0;
    for (u32 i = 0; i < n_repeat; i++) {
        otc += no_acc_lats[i] > thresh;
        utc += acc_lats[i] < thresh;
    }
    _info("%s: no access: %ld; has access: %ld; threshold: %ld; otc: %u; utc: "
          "%u\n",
          name, no_acc_lat, acc_lat, thresh, otc, utc);

    fprintf(stderr, "%s: max no acc:", name);
    for (u32 i = 0; i < 20 && i < n_repeat; i++) {
        fprintf(stderr, " %ld", no_acc_lats[n_repeat - 1 - i]);
    }
    fprintf(stderr, "\n%s: min acc:", name);
    for (u32 i = 0; i < 20 && i < n_repeat; i++) {
        fprintf(stderr, " %ld", acc_lats[i]);
    }
    fprintf(stderr, "\n\n");

    u64 bad_thresh = n_repeat * bad_thresh_ratio;
    if (otc > bad_thresh || utc > bad_thresh) {
        _warn("Bad threshold!\n");
        thresh = 0; // bad threshold
    }

    free(no_acc_lats);
    free(acc_lats);
    return thresh;
}

i64 calibrate_para_probe_lat(u8 *target, EVSet *evset, u32 arr_repeat,
                             u32 l2_repeat, double bad_thresh_ratio) {
    return calibrate_probe_lat(target, evset, arr_repeat, l2_repeat,
                               bad_thresh_ratio, "Para Probe",
                               probe_skx_sf_evset_para);
}

i64 calibrate_chase_probe_lat(u8 *target, EVSet *evset, u32 arr_repeat,
                              u32 l2_repeat, double bad_thresh_ratio) {
    return calibrate_probe_lat(target, evset, arr_repeat, l2_repeat,
                               bad_thresh_ratio, "Ptr-Chase Probe",
                               probe_skx_sf_evset_ptr_chase);
}
