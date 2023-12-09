#pragma once

#include "access_seq.h"
#include "cache_param.h"
#include "evchain.h"
#include "evset.h"
#include "oracle.h"
#include "latency.h"
#include "monitor.h"

static inline bool cache_env_init(int verbose) {
    detected_caches.verbose = verbose > 1;
    detect_cpu_caches(&detected_caches);

    find_common_caches(&detected_caches, &detected_l1i, &detected_l1d,
                       &detected_l2, &detected_l3);
    if (detected_l3 && verbose > 0) {
        _info("%u L3 slices detected, does it look right?\n",
              detected_l3->n_slices);
    }

    if (cache_latencies_detect(&detected_cache_lats, &detected_caches)) {
        return true;
    }

    if (verbose > 0) {
        cache_latencies_pprint(&detected_cache_lats);
    }

    default_l1d_evset_build_config(&def_l1d_ev_config);
    default_l2_evset_build_config(&def_l2_ev_config);
    return false;
}
