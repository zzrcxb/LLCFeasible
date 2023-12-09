#pragma once

#include "cache_param.h"
#include "access_seq.h"
#include "helper_thread.h"
#include "bitwise.h"
#include "latency.h"
#include "libpt.h"
#include "misc.h"

typedef enum {
    EVSET_ALGO_NAIVE = 1,
    EVSET_ALGO_GROUP_TEST = 2,
    EVSET_ALGO_GROUP_TEST_RANDOM = 3,
    EVSET_ALGO_GROUP_TEST_NOEARLY = 4,
    EVSET_ALGO_LAST_STRAW = 5,
    EVSET_ALGO_LAST_STRAW_DEV = 6,
    EVSET_ALGO_PRIME_SCOPE = 7,
    EVSET_ALGO_PRIME_SCOPE_OPT = 8,
    EVSET_ALGO_DEFAULT = EVSET_ALGO_LAST_STRAW,
    EVSET_ALGO_INVALID = -1
} evset_algorithm;

#define MAX_RETRY_REC 22
#define MAX_BACKTRACK_REC 102

struct evset_stats {
    u64 alloc_duration;
    u64 population_duration;
    u64 build_duration;
    u64 pruning_duration;
    u64 extension_duration;
    u64 retries, backtracks, cands_tests, mem_accs;
    u64 pure_mem_acc, pure_tests;
    u64 pure_mem_acc2, pure_tests2;
    u64 pos_unsure, neg_unsure, ooh, ooc; // out-of-history/candidates
    u64 no_next, timeout, meet, retry_duration;
    u32 retry_dist[MAX_RETRY_REC], useful_retry_dist[MAX_RETRY_REC];
    u64 retry_duras[MAX_RETRY_REC], useful_retry_duras[MAX_RETRY_REC];
    u32 bctr_dist[MAX_BACKTRACK_REC], useful_bctr_dist[MAX_BACKTRACK_REC];
    u64 bctr_duras[MAX_BACKTRACK_REC], useful_bctr_duras[MAX_BACKTRACK_REC];
};

extern struct evset_stats _evset_stats;

static inline void inc_retry(u64 retry) {
    if (retry < MAX_RETRY_REC - 1) {
        _evset_stats.retry_dist[retry] += 1;
    } else {
        _evset_stats.retry_dist[MAX_RETRY_REC - 1] += 1;
    }
}

static inline void inc_retry_dura(u64 retry, u64 dura) {
    if (retry < MAX_RETRY_REC - 1) {
        _evset_stats.retry_duras[retry] += dura;
    } else {
        _evset_stats.retry_duras[MAX_RETRY_REC - 1] += dura;
    }
}

static inline void inc_useful_retry(u64 retry) {
    if (retry < MAX_RETRY_REC - 1) {
        _evset_stats.useful_retry_dist[retry] += 1;
    } else {
        _evset_stats.useful_retry_dist[MAX_RETRY_REC - 1] += 1;
    }
}

static inline void inc_useful_retry_dura(u64 retry, u64 dura) {
    if (retry < MAX_RETRY_REC - 1) {
        _evset_stats.useful_retry_duras[retry] += dura;
    } else {
        _evset_stats.useful_retry_duras[MAX_RETRY_REC - 1] += dura;
    }
}

static inline void inc_bctr(u64 bctr) {
    if (bctr < MAX_BACKTRACK_REC - 1) {
        _evset_stats.bctr_dist[bctr] += 1;
    } else {
        _evset_stats.bctr_dist[MAX_BACKTRACK_REC - 1] += 1;
    }
}

static inline void inc_useful_bctr(u64 bctr) {
    if (bctr < MAX_BACKTRACK_REC - 1) {
        _evset_stats.useful_bctr_dist[bctr] += 1;
    } else {
        _evset_stats.useful_bctr_dist[MAX_BACKTRACK_REC - 1] += 1;
    }
}

static inline void inc_bctr_dura(u64 bctr, u64 dura) {
    if (bctr < MAX_BACKTRACK_REC - 1) {
        _evset_stats.bctr_duras[bctr] += dura;
    } else {
        _evset_stats.bctr_duras[MAX_BACKTRACK_REC - 1] += dura;
    }
}

static inline void inc_useful_bctr_dura(u64 bctr, u64 dura) {
    if (bctr < MAX_BACKTRACK_REC - 1) {
        _evset_stats.useful_bctr_duras[bctr] += dura;
    } else {
        _evset_stats.useful_bctr_duras[MAX_BACKTRACK_REC - 1] += dura;
    }
}

static inline void _pprint_dist(u32 *total, u32 *useful, u64 *total_dura,
                                u64 *useful_dura, u32 max_cnt,
                                const char *name) {
    _info("%s:", name);
    for (u32 i = 1; i < max_cnt; i++) {
        if (total[i] || useful[i]) {
            if (i == max_cnt - 1) {
                fprintf(stderr, " >=%u:", i);
            } else {
                fprintf(stderr, " %u:", i);
            }
            fprintf(stderr, " %u/%u-%lu/%lums;", useful[i], total[i],
                    useful_dura[i] / 1000000, total_dura[i] / 1000000);
        }
    }
    fprintf(stderr, "\n");
}

static inline void pprint_evset_stats() {
    _info("Alloc: %luus; Population: %luus; Build: %luus; Pruning: %luus; "
          "Extension: %luus;\n"
          "Retries: %lu; Backtracks: %lu; Tests: %lu; Mem Acc.: %lu;\n"
          "Pos unsure: %lu; Neg unsure: %lu; OOH: %lu; OOC: %lu; NoNex: %lu; "
          "Timeout: %lu\nPure acc: %lu; Pure tests: %lu; Pure acc 2: %lu; Pure "
          "tests 2: %lu\n",
          _evset_stats.alloc_duration / 1000,
          _evset_stats.population_duration / 1000,
          _evset_stats.build_duration / 1000,
          _evset_stats.pruning_duration / 1000,
          _evset_stats.extension_duration / 1000, _evset_stats.retries,
          _evset_stats.backtracks, _evset_stats.cands_tests,
          _evset_stats.mem_accs, _evset_stats.pos_unsure,
          _evset_stats.neg_unsure, _evset_stats.ooh, _evset_stats.ooc,
          _evset_stats.no_next, _evset_stats.timeout, _evset_stats.pure_mem_acc,
          _evset_stats.pure_tests, _evset_stats.pure_mem_acc2,
          _evset_stats.pure_tests2);

    _pprint_dist(_evset_stats.retry_dist, _evset_stats.useful_retry_dist,
                 _evset_stats.retry_duras, _evset_stats.useful_retry_duras,
                 MAX_RETRY_REC, "Retry dist");
    _pprint_dist(_evset_stats.bctr_dist, _evset_stats.useful_bctr_dist,
                 _evset_stats.bctr_duras, _evset_stats.useful_bctr_duras,
                 MAX_BACKTRACK_REC, "Backtrack dist");
    _info("Meet: %lu; Retry: %luus\n", _evset_stats.meet,
          _evset_stats.retry_duration / 1000);
}

static inline void reset_evset_stats() {
    memset(&_evset_stats, 0, sizeof(_evset_stats));
}

struct _evset;
struct _evtest_config;

/* Eviction candidates */
typedef struct {
    // scaling: allocate floor(uncertainty * n_ways * scaling) candidates
    double scaling;
    struct _evset *filter_ev;
} EVCandsConfig;

// a memory buffer to choose candidate address from
typedef struct {
    void *buf;
    size_t n_pages, ref_cnt;
} EVBuffer;

EVBuffer *evbuffer_new(cache_param *cache, EVCandsConfig *config);

void evbuffer_free(EVBuffer *evb);

// tracking eviction candidates
typedef struct {
    u8 **cands;
    EVBuffer *evb;
    size_t size, ref_cnt;
    cache_param *cache;
} EVCands;

void evcands_free(EVCands *cands);

// allocate an EVCands structure. If evb is NULL, it will automatically allocate
// a new evb. This function only allocates the struct without populating
// EVCands.cands
EVCands *evcands_new(cache_param *cache, EVCandsConfig *config, EVBuffer *evb);

// shift each candidate address to a new page offset
EVCands *evcands_shift(EVCands *cands, u32 offset);

// populate EVCands.cands. If EVCandsConfig has filter_ev,
// then candidate filtering is performed.
bool evcands_populate(u32 offset, EVCands *cands, EVCandsConfig *config);

// Test result with uncertainty info.
// Algorithms may use the uncertainty info to improve its accuracy or speed.
typedef enum {
    EV_NEG = -2,
    EV_NEG_UNSURE = -1,
    EV_POS_UNSURE = 1,
    EV_POS = 2
} EVTestRes;

typedef void (*cand_traverse_func)(u8 **cands, size_t cnt,
                                   struct _evtest_config *c);

typedef EVTestRes (*cand_test_func)(u8 *target, u8 **cands, size_t cnt,
                                    struct _evtest_config *c);

typedef EVTestRes (*evset_test_func)(u8 *target, struct _evset *evset);

typedef struct _evtest_config {
    i64 lat_thresh; // latency threshold for cache miss

    // repeat eviction test "trials" times and get over-threshold counts (OTC)
    // the low_bnd and upp_bnd will be scaled according to test_scale
    // if OTC < low_bnd -> EV_NEG; if OTC > upp_bnd -> EV_POS;
    // if low_bnd <= OTC < (low_bnd + upp_bnd) / 2 -> EV_NEG_UNSURE;
    // if (low_bnd + upp_bnd) / 2 <= OTC <= upp_bnd -> EV_POS_UNSURE;
    // we will retry "unsure_retry" times before we give up and return an UNSURE
    u32 trials, low_bnd, upp_bnd, unsure_retry, test_scale;

    // repeat evset "ev_repeat" times, access the target "acc_cnt" times
    u32 ev_repeat, access_cnt;

    u32 stride, block;

    // access an eviction set for a lower-level cache
    struct _evset *lower_ev;

    // whether the eviction requires a helper thread
    bool need_helper, flush_cands, foreign_evictor;
    helper_thread_ctrl *hctrl;

    cand_traverse_func traverse;
    cand_test_func test;
} EVTestConfig;

typedef struct {
    u32 cap_scaling; // evset capacity = cap_scaling * n_ways
    u32 verify_retry; // maximum number of retries
    u32 retry_timeout; // in ms
    u32 max_backtrack; // maximum number of backtracks
    u32 slack;
    u32 extra_cong; // extend the eviction set with extra congruent lines
    bool ret_partial, prelim_test, need_skx_sf_ext;
} EVAlgoConfig;

typedef struct {
    EVCandsConfig cands_config;
    EVTestConfig test_config, test_config_alt;
    EVAlgoConfig algo_config;
    evset_algorithm algorithm;
} EVBuildConfig;

extern EVBuildConfig def_l1d_ev_config, def_l2_ev_config;

/* Eviction set related structures and functions */
typedef struct _evset {
    u8 **addrs; // storage for eviction sets
    u32 size, cap; // size and the capacity of the evset
    EVCands *cands;
    EVBuildConfig *config;
    cache_param *target_cache;
} EVSet;

EVSet *evset_new(u32 offset, EVBuildConfig *config, cache_param *cache,
                 EVCands *evcands);

EVSet *evset_shift(EVSet *from, u32 offset);

void evset_free(EVSet *evset);

i64 evset_test_batch(u8 **targets, size_t cnt, EVSet *evset);

/* Traverse functions */
void generic_cands_traverse(u8 **cands, size_t cnt, EVTestConfig *tconf);

EVTestRes generic_test_eviction(u8 *target, u8 **cands, size_t cnt,
                                EVTestConfig *tconf);

static inline void generic_evset_traverse(EVSet *evset) {
    generic_cands_traverse(evset->addrs, evset->size,
                           &evset->config->test_config);
}

static inline void generic_evset_traverse_alt(EVSet *evset) {
    generic_cands_traverse(evset->addrs, evset->size,
                           &evset->config->test_config_alt);
}

static inline EVTestRes generic_evset_test(u8 *target, EVSet *evset) {
    return generic_test_eviction(target, evset->addrs, evset->size,
                                  &evset->config->test_config);
}

static inline EVTestRes generic_evset_test_alt(u8 *target, EVSet *evset) {
    return generic_test_eviction(target, evset->addrs, evset->size,
                                 &evset->config->test_config_alt);
}

static inline EVTestRes precise_evset_test(u8 *target, EVSet *evset) {
    u32 test_scale = evset->config->test_config.test_scale;
    evset->config->test_config.test_scale = 2;
    EVTestRes tres = generic_evset_test(target, evset);
    evset->config->test_config.test_scale = test_scale;
    return tres;
}

static inline EVTestRes precise_evset_test_alt(u8 *target, EVSet *evset) {
    u32 test_scale = evset->config->test_config_alt.test_scale;
    evset->config->test_config_alt.test_scale = 2;
    EVTestRes tres = generic_evset_test_alt(target, evset);
    evset->config->test_config_alt.test_scale = test_scale;
    return tres;
}

void skx_sf_cands_traverse_st(u8 **cands, size_t cnt, EVTestConfig *tconfig);

void skx_sf_cands_traverse_mt(u8 **cands, size_t cnt, EVTestConfig *tconfig);

EVTestRes skx_evset_test_l3_st(u8 *target, EVSet *evset);

/* Algorithms */
bool evset_builder_naive(u8 *target, EVSet *evset);

bool evset_builder_group_test(u8 *target, EVSet *evset, bool early_terminate);

bool evset_builder_last_straw(u8 *target, EVSet *evset);

bool skx_sf_evset_builder_prime_scope(u8 *target, EVSet *evset, bool migrate);

/* Default evset build configurations */
void default_l1d_evset_build_config(EVBuildConfig *config);

void default_l2_evset_build_config(EVBuildConfig *config);

void default_skx_sf_evset_build_config(EVBuildConfig *config, EVSet *l1d_ev,
                                       EVSet *l2_ev, helper_thread_ctrl *hctrl);

/* Individual level builder */
EVSet *build_evset_generic(u8 *target, EVBuildConfig *config,
                           cache_param *cache, EVCands *cands);

EVSet *build_l1d_EVSet(u8 *target, EVBuildConfig *config, EVCands *evcands);

EVSet *build_l2_EVSet(u8 *target, EVBuildConfig *config, EVCands *evcands);

EVSet *build_skx_sf_EVSet(u8 *target, EVBuildConfig *config, EVCands *evcands);

EVSet **build_evsets_at(u32 offset, EVBuildConfig *conf, cache_param *cache,
                        EVCands *_cands, size_t *ev_cnt, cache_param *lower_c,
                        EVBuildConfig *lower_conf, EVSet **lower_evsets,
                        size_t n_lower_evsets);

/* more tests */
static inline EVTestRes _evset_self_test(EVSet *evset, evset_test_func tfunc) {
    u8 *target = evset->addrs[0];
    evset->addrs++;
    evset->size--;
    EVTestRes res = tfunc(target, evset);
    evset->addrs--;
    evset->size++;
    return res;
}

static inline EVTestRes evset_self_test(EVSet *evset) {
    return _evset_self_test(evset, generic_evset_test);
}

static inline EVTestRes evset_self_test_alt(EVSet *evset) {
    return _evset_self_test(evset, generic_evset_test_alt);
}

static inline EVTestRes evset_self_precise_test(EVSet *evset) {
    return _evset_self_test(evset, precise_evset_test);
}

static inline EVTestRes evset_self_precise_test_alt(EVSet *evset) {
    return _evset_self_test(evset, precise_evset_test_alt);
}

size_t prune_evcands(u8 *target, u8 **cands, size_t cnt, EVTestConfig *tconf);

EVSet *prune_EVSet(u8 *target, EVSet *evset);

EVSet *extend_skx_sf_EVSet(EVSet *evset);

static __always_inline void access_evset(EVSet *evset) {
    access_array(evset->addrs, evset->size);
}

static __always_inline void access_evset_bwd(EVSet *evset) {
    access_array_bwd(evset->addrs, evset->size);
}

static __always_inline void access_evset_seq(EVSet *evset) {
    access_array_seq(evset->addrs, evset->size);
}

static __always_inline void write_evset(EVSet *evset) {
    write_array(evset->addrs, evset->size);
}

static __always_inline void write_evset_offset(EVSet *evset) {
    write_array_offset(evset->addrs, evset->size);
}

static __always_inline void flush_evset(EVSet *evset) {
    flush_array(evset->addrs, evset->size);
}

static __always_inline void repeat_access_evset(EVSet *evset, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        access_evset(evset);
    }
}

static __always_inline void repeat_write_evset(EVSet *evset, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        write_evset(evset);
    }
}

static __always_inline u8 *tlb_warmup_ptr(u8 *ptr) {
    u8 *page = _ALIGN_DOWN(ptr, PAGE_SHIFT);
    u32 target_offset = page_offset(ptr);
    u32 tlb_offset = (target_offset + PAGE_SIZE / 2) % PAGE_SIZE;
    return page + tlb_offset;
}
