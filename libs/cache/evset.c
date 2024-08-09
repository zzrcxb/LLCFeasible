#include "cache/evset.h"
#include "cache/oracle.h"
#include "sugar.h"
#include "sync.h"
#include "math.h"

static const bool _dbg = false;

#define _dprintf(...)                                                          \
    do {                                                                       \
        if (_dbg) {                                                            \
            fprintf(stderr, __VA_ARGS__);                                      \
        }                                                                      \
    } while (0)

struct evset_stats _evset_stats;

EVBuildConfig def_l1d_ev_config, def_l2_ev_config;

// batch test whether a group of addresses can be evicted by the evset;
// those can be evicted are swapped to the front of the "targets".
// Number of addresses that can be evicted is returned
i64 evset_test_batch(u8 **targets, size_t cnt, EVSet *evset) {
    size_t batch_sz = evset->target_cache->n_ways;
    if (batch_sz > 2) batch_sz -= 1; // batch_sz = max(n_ways - 1, 1);

    u32 *otcs = _calloc(batch_sz, sizeof(*otcs));
    if (!otcs) {
        _error("Cannot allocate temp otc buffer; batch sz: %lu\n", batch_sz);
        return -1;
    }

    i64 n_pos = 0;
    for (size_t s = 0; s < cnt; s += batch_sz) {
        size_t cur_batch_sz = _min(batch_sz, cnt - s);
        memset(otcs, 0, sizeof(*otcs) * cur_batch_sz);
        for (u32 t = 0; t < evset->config->test_config.trials; t++) {
            access_array(&targets[s], cur_batch_sz);
            _lfence();
            generic_evset_traverse(evset);
            _lfence();
            for (size_t i = 0; i < cur_batch_sz; i++) {
                u64 lat = _time_maccess(targets[s + i]);
                otcs[i] += lat > evset->config->test_config.lat_thresh;
            }
        }

        for (size_t i = 0; i < cur_batch_sz; i++) {
            if (otcs[i] > evset->config->test_config.upp_bnd) {
                _swap(targets[n_pos], targets[s + i]);
                n_pos += 1;
            }
        }
    }

    _free(otcs);
    return n_pos;
}

EVBuffer *evbuffer_new(cache_param *cache, EVCandsConfig *config) {
    size_t uncertainty = cache_uncertainty(cache);
    size_t n_pages, buf_size;

    if (__has_hugepage) {
        u64 n_cands = uncertainty * cache->n_ways * config->scaling;
        u64 cands_per_page = HUGE_PAGE_SIZE / cache_congruent_stride(cache);
        n_pages = n_cands / cands_per_page;
        if (n_cands % cands_per_page) {
            n_pages += 1;
        }
        buf_size = n_pages * HUGE_PAGE_SIZE;
        _info("Need to allocate %lu huge pages\n", n_pages);
    } else {
        n_pages = uncertainty * cache->n_ways * config->scaling;
        buf_size = n_pages * PAGE_SIZE;
    }

    EVBuffer *evb = _calloc(1, sizeof(*evb));
    if (!evb) {
        _error("Failed to allocate EVBuffer\n");
        return NULL;
    }

    void *pages = NULL;
    if (__has_hugepage) {
        pages = mmap_huge_shared_init(NULL, buf_size, 0);
    } else {
        pages = mmap_shared_init(NULL, buf_size, 0);
    }
    if (!pages) {
        _error("Failed to mmap %lu bytes for eviction buffer\n", buf_size);
        goto err;
    }
    _assert(_ALIGNED(pages, PAGE_SHIFT));

    evb->buf = pages;
    evb->n_pages = n_pages;
    evb->ref_cnt = 0;
    return evb;

err:
    _free(evb);
    return NULL;
}

void evbuffer_free(EVBuffer *evb) {
    if (evb && evb->ref_cnt == 0) {
        size_t buf_sz;
        if (__has_hugepage) {
            buf_sz = evb->n_pages * HUGE_PAGE_SIZE;
        } else {
            buf_sz = evb->n_pages * PAGE_SIZE;
        }

        munmap(evb->buf, buf_sz);
        _free(evb);
    }
}

EVCands *evcands_new(cache_param *cache, EVCandsConfig *config, EVBuffer *evb) {
    EVCands *cands = _calloc(1, sizeof(*cands));
    if (!cands) {
        _error("Failed to allocate EVCands");
        return NULL;
    }

    u64 start = time_ns();
    cands->evb = evb;
    if (!cands->evb) {
        cands->evb = evbuffer_new(cache, config);
        if (!cands->evb) {
            _free(cands);
            return NULL;
        }
    }
    u64 end = time_ns();
    _evset_stats.alloc_duration = end - start;

    cands->evb->ref_cnt += 1;
    cands->ref_cnt = 0;
    cands->cache = cache;
    return cands;
}

EVCands *evcands_shift(EVCands *from, u32 offset) {
    EVCands *cands = _calloc(1, sizeof(*cands));
    if (!cands) {
        _error("Failed to allocate EVCands");
        return NULL;
    }

    cands->cands = _calloc(from->size, sizeof(*from->cands));
    cands->evb = from->evb;
    cands->evb->ref_cnt += 1;
    cands->size = from->size;
    cands->ref_cnt = 0;
    if (!cands->cands) {
        _error("Failed to allocate the candidate array\n");
        goto err;
    }

    for (size_t i = 0; i < cands->size; i++) {
        cands->cands[i] = _ALIGN_DOWN(from->cands[i], PAGE_SHIFT) + offset;
    }

    return cands;

err:
    _free(cands);
    return NULL;
}

static void shuffle_evset(u8 **addrs, u32 sz) {
    if (sz <= 1) return;

    for (u32 tail = sz - 1; tail > 0; tail--) {
        u32 n_choice = tail + 1;
        u32 choice = rand() % n_choice;
        u8 *tmp = addrs[choice];
        addrs[choice] = addrs[tail];
        addrs[tail] = tmp;
    }
}

bool evcands_populate(u32 offset, EVCands *cands, EVCandsConfig *config) {
    size_t n_cands_init = cands->evb->n_pages;
    u64 stride = PAGE_SIZE;
    if (__has_hugepage) {
        stride = cache_congruent_stride(cands->cache);
        n_cands_init *= HUGE_PAGE_SIZE / stride;
    }

    u8 **addrs = _calloc(n_cands_init, sizeof(*addrs));
    if (!addrs) {
        _error("Failed to allocate the candidate array; n_pages: %lu\n",
               n_cands_init);
        goto err;
    }

    for (size_t n = 0; n < n_cands_init; n++) {
        addrs[n] = cands->evb->buf + n * stride + offset;
        *addrs[n] = n;
    }

    // we do not have a filter evset or it's not worth filtering
    if (!config->filter_ev ||
        cache_uncertainty(config->filter_ev->target_cache) == 1) {
        cands->cands = addrs;
        cands->size = n_cands_init;
        return false;
    }

    u64 start = time_ns();
#ifndef ICELAKE
    i64 n_cands = evset_test_batch(addrs, n_cands_init, config->filter_ev);
    if (n_cands <= 0) {
        _error("Failed to filter out candidate lines\n");
        goto err;
    }
#else
    // the evset_test_batch gives unstable results on ICELAKE-SP for unknown reasons
    i64 n_cands = 0;
    for (size_t i = 0; i < n_cands_init; i++) {
        if (generic_evset_test(addrs[i], config->filter_ev) == EV_POS) {
            _swap(addrs[n_cands], addrs[i]);
            n_cands += 1;
        }
    }
#endif

    if (n_cands <= 0) {
        _error("Failed to filter out candidate lines\n");
        goto err;
    }

    _info("Filtered %lu lines to %ld candidates\n", n_cands_init, n_cands);
    u8 **tmp = realloc(addrs, n_cands * sizeof(*addrs));
    if (!tmp) {
        _error("Failed to realloc the candidate array\n");
        goto err;
    }
    u64 end = time_ns();
    _evset_stats.population_duration = end - start;

    cands->cands = tmp;
    cands->size = n_cands;
    return false;

err:
    _free(addrs);
    return true;
}

void evcands_free(EVCands *cands) {
    if (cands && cands->ref_cnt == 0) {
        cands->evb->ref_cnt -= 1;
        evbuffer_free(cands->evb);
        _free(cands->cands);
        _free(cands);
    }
}

EVSet *evset_new(u32 offset, EVBuildConfig *config, cache_param *cache,
                 EVCands *evcands) {
    EVSet *evset = _calloc(1, sizeof(*evset));
    if (!evset) {
        _error("Cannot allocate an eviction set.\n");
        return NULL;
    }

    evset->size = 0;
    evset->cap = config->algo_config.cap_scaling * cache->n_ways;
    evset->addrs = _calloc(evset->cap, sizeof(*evset->addrs));
    if (!evset->addrs) {
        _error("Cannot allocate evset addrs buffer.\n");
        goto err;
    }

    evset->cands = evcands;
    evset->config = config;
    evset->target_cache = cache;
    if (!evset->cands) {
        evset->cands = evcands_new(cache, &config->cands_config, NULL);
        if (!evset->cands) {
            _error("Failed to alloc evcands\n");
            goto err;
        }

        if (evcands_populate(offset, evset->cands, &config->cands_config)) {
            _error("Failed to populate evcands\n");
            goto err;
        }
    }
    evset->cands->ref_cnt += 1;

    return evset;

err:
    evset_free(evset);
    return NULL;
}

EVSet *evset_shift(EVSet *from, u32 offset) {
    EVSet *evset = _calloc(1, sizeof(*evset));
    if (!evset) {
        _error("Cannot allocate an eviction set\n");
        return NULL;
    }

    memcpy(evset, from, sizeof(*evset));
    evset->cands->ref_cnt += 1;
    evset->addrs = _calloc(evset->cap, sizeof(*evset->addrs));
    if (!evset->addrs) {
        _error("Cannot allocate evset addrs buffer.\n");
        goto err;
    }

    for (size_t i = 0; i < evset->cap; i++) {
        u8 *page = _ALIGN_DOWN(from->addrs[i], PAGE_SHIFT);
        evset->addrs[i] = page + offset;
    }
    return evset;

err:
    evset_free(evset);
    return NULL;
}

void evset_free(EVSet *evset) {
    if (evset) {
        evset->cands->ref_cnt -= 1;
        _free(evset->addrs);
        _free(evset);
    }
}

/* Traverse functions */
void generic_cands_traverse(u8 **cands, size_t cnt, EVTestConfig *tconf) {
    // traverse backwards to prevent speculative execution to overshoot
    for (size_t r = 0; r < tconf->ev_repeat; r++) {
        access_array_bwd(cands, cnt);
    }
}

static inline void
helper_thread_traverse_cands(u8 **cands, size_t cnt, EVTestConfig *conf) {
    struct helper_thread_traverse_cands *tcands = _calloc(1, sizeof(*tcands));
    assert(tcands);

    *tcands = (struct helper_thread_traverse_cands){.traverse = conf->traverse,
                                                    .cands = cands,
                                                    .cnt = cnt,
                                                    .tconfig = conf};

    conf->hctrl->action = TRAVERSE_CANDS;
    conf->hctrl->payload = tcands;
    _barrier();
    conf->hctrl->waiting = false;
    wait_helper_thread(conf->hctrl);
    _free(tcands);
}

EVTestRes generic_test_eviction(u8 *target, u8 **cands, size_t cnt,
                                EVTestConfig *tconf) {
    u8 *tlb_target = tlb_warmup_ptr(target);
    u32 otc = 0, aux_before, aux_after;
    u32 trials = tconf->trials;
    u32 low_bnd = tconf->low_bnd;
    u32 upp_bnd = tconf->upp_bnd;
    if (tconf->test_scale > 1) {
        trials *= tconf->test_scale;
        low_bnd *= tconf->test_scale;
        upp_bnd *= tconf->test_scale;
    }

    _dprintf("Lats:");
    for (u32 r = 0; r < tconf->unsure_retry; r++) {
        _evset_stats.cands_tests += 1;
        _evset_stats.mem_accs += cnt;

        otc = 0;
        for (u32 i = 0; i < trials;) {
            _rdtscp_aux(&aux_before);

            _clflush(target); // flush it so it gets an insertion age
            if (tconf->flush_cands) {
                flush_array(cands, cnt);
            }
            _lfence();
            // load the target line
            for (u32 j = 0; j < tconf->access_cnt; j++) {
                // may use lower ev to causing repeated access to upper levels
                if (tconf->lower_ev) {
                    generic_evset_traverse(tconf->lower_ev);
                }
                _lfence();
                _maccess(target);
                if (tconf->need_helper) {
                    helper_thread_read_single(target, tconf->hctrl);
                    _maccess(target);
                }
            }

            // traverse candidates
            _lfence();
            if (tconf->foreign_evictor) {
                helper_thread_traverse_cands(cands, cnt, tconf);
            } else {
                tconf->traverse(cands, cnt, tconf);
            }
            _lfence();

            // warmup TLB then time target
            _maccess(tlb_target);
            u64 UNUSED _tmp;
            u64 lat = _time_maccess_aux(target, _tmp, aux_after);
            if (aux_before == aux_after &&
                lat < detected_cache_lats.interrupt_thresh) {
                otc += (lat >= tconf->lat_thresh);
                i += 1;
                if (otc > upp_bnd) {
                    return EV_POS;
                }
                _dprintf(" %lu", lat);
            }
        }

        if (otc > upp_bnd) {
            return EV_POS;
        } else if (otc < low_bnd) {
            return EV_NEG;
        }
    }

    if (otc >= (low_bnd + upp_bnd) / 2) {
        _evset_stats.pos_unsure += 1;
        return EV_POS_UNSURE;
    } else {
        _evset_stats.neg_unsure += 1;
        return EV_NEG_UNSURE;
    }
}

void skx_sf_cands_traverse_st(u8 **cands, size_t cnt, EVTestConfig *tconfig) {
    _assert(tconfig->lower_ev);
    size_t repeat = tconfig->ev_repeat, block = tconfig->block,
           stride = tconfig->stride;
    for (size_t r = 0; r < 2; r++) {
        // access_array(cands, cnt);
        prime_cands_daniel(cands, cnt, repeat, stride, block);
        _lfence();
        if (cnt < 16) {
            generic_evset_traverse(tconfig->lower_ev);
            _lfence();
        }
    }
}

void skx_sf_cands_traverse_mt(u8 **cands, size_t cnt, EVTestConfig *tconfig) {
    struct helper_thread_read_array *arr = _malloc(sizeof(*arr));
    _assert(arr);
    size_t repeat = tconfig->ev_repeat, block = tconfig->block,
           stride = tconfig->stride;

    *arr = (struct helper_thread_read_array){.addrs = cands,
                                             .cnt = cnt,
                                             .repeat = repeat,
                                             .block = block,
                                             .stride = stride,
                                             .bwd = true};
    tconfig->hctrl->action = READ_ARRAY;
    tconfig->hctrl->payload = arr;
    _barrier();
    tconfig->hctrl->waiting = false;

    // access_array(cands, cnt);
    prime_cands_daniel(cands, cnt, repeat, stride, block);
    if (cnt < detected_l2->n_ways && tconfig->lower_ev) {
        generic_evset_traverse(tconfig->lower_ev);
        _lfence();
        access_array_bwd(cands, cnt);
    }

    wait_helper_thread(tconfig->hctrl);
    _free(arr);
}

EVTestRes skx_evset_test_l3_st(u8 *target, EVSet *evset) {
    EVTestConfig *tconf = &evset->config->test_config;
    void *fp_backup = tconf->traverse;
    bool helper = tconf->need_helper;

    tconf->traverse = skx_sf_cands_traverse_st;
    tconf->need_helper = false;

    EVTestRes res =
        generic_test_eviction(target, evset->addrs, evset->size, tconf);

    tconf->traverse = fp_backup;
    tconf->need_helper = helper;
    return res;
}

/* Algorithms */
bool evset_builder_naive(u8 *target, EVSet *evset) {
    u8 **cands = evset->cands->cands;
    size_t n_cands = evset->cands->size, evsz = 0;
    EVTestConfig *test_config = &evset->config->test_config;

    while (evsz < evset->cap && n_cands > evsz) {
        _swap(cands[evsz], cands[n_cands - 1]);
        if (test_config->test(target, cands, n_cands - 1, test_config) > 0) {
            n_cands -= 1;
        } else {
            _swap(cands[evsz], cands[n_cands - 1]);
            evsz += 1;
        }

        if (evsz >= evset->target_cache->n_ways &&
            test_config->test(target, cands, evsz, test_config) == EV_POS) {
            break;
        }
    }
    evset->size = evsz;
    memcpy(evset->addrs, cands, sizeof(*cands) * evsz);
    return false;
}

static void print_congruents(u8 *target, u8 **cands, size_t cnt) {
    if (cache_oracle_inited()) {
        u64 target_hash = llc_addr_hash(target), match = 0;
        for (size_t n = 0; n < cnt; n++) {
            match += target_hash == llc_addr_hash(cands[n]);
        }
        _dprintf("Included %lu congruent lines in the test\n", match);
    }
}

bool evset_builder_last_straw(u8 *target, EVSet *evset) {
    u8 **cands = evset->cands->cands;
    size_t n_cands = evset->cands->size, evsz = 0;
    EVTestConfig *test_config = &evset->config->test_config;
    EVAlgoConfig *algo_config = &evset->config->algo_config;
    cache_param *target_cache = evset->target_cache;
    cand_test_func testev = test_config->test;
    u32 extra_cong = algo_config->extra_cong;
    u32 exp_evsz = target_cache->n_ways + extra_cong;

    if (n_cands <= 1) {
        return true;
    }

    u32 uncertainty = cache_uncertainty(target_cache);
    if (evset->config->cands_config.filter_ev) {
        uncertainty /= cache_uncertainty(
            evset->config->cands_config.filter_ev->target_cache);
    }

    u64 migrated = n_cands - 1, n_ways = target_cache->n_ways;
    u64 max_bctr = algo_config->max_backtrack;
    u64 num_carried_cong = target_cache->n_ways - algo_config->slack;
    i64 lower = 0, upper = n_cands, cnt, n_bctr = 0, iters = 0;
    bool is_reset = false;
    while (evsz < evset->cap && n_bctr < max_bctr) {
        u32 offset = 0;
        if (algo_config->slack && evsz > num_carried_cong) {
            offset = evsz - num_carried_cong;
        }

        // invariant 1: upper - lower >= 1
        // assert(upper - lower >= 1);

        if (evsz > 0 && !is_reset && evsz < evset->target_cache->n_ways) {
            u32 rem = evset->target_cache->n_ways - evsz; // rem > 0
            cnt = (upper * rem + lower) / (rem + 1);
            if (cnt == upper) {
                assert(cnt > 0);
                cnt -= 1; // needed for preserving invariant 2
            }
        } else {
            cnt = (upper + lower) / 2;
        }

        is_reset = false;
        u8 **cands_o = cands + offset;
        bool has_pos = false, has_neg = false;
        while (upper - lower > 1) {
            // invariant 2: lower < cnt < upper
            // assert(cnt > lower);
            // assert(cnt < upper);
            // assert(cnt > offset);
            _dprintf("---\n");
            if (evsz < n_ways) {
                _evset_stats.pure_tests2 += 1;
                _evset_stats.pure_mem_acc2 += (cnt - offset);
            }

            _evset_stats.pure_tests += 1;
            _evset_stats.pure_mem_acc += (cnt - offset);
            EVTestRes res = testev(target, cands_o, cnt - offset, test_config);
            _dprintf("\n%ld: Upper: %lu; Lower: %lu; Cnt: %lu has_pos: %d; "
                     "has_neg: %d; offset: %u; evsz: %lu\n",
                     iters, upper, lower, cnt, has_pos, has_neg, offset, evsz);
            if (res > 0) {
                upper = cnt;
                has_pos = true;
            } else {
                lower = cnt;
                has_neg = true;
            }
            cnt = (upper + lower) / 2;
            // invariant 3: upper != lower
            // assert(lower != upper);
        }
        // invariant 4: upper = lower + 1 >= evsz + 1
        // assert(upper == lower + 1);
        // assert(upper >= evsz + 1);

        // lower is the largest number of candidates that CANNOT evict target;
        // while upper is the smallest # candidates that can; therefore,
        // upper-th element (cands[upper - 1]) is a congruent address
        _dprintf("Final %ld: Upper: %lu; Lower: %lu; has_pos: %d; has_neg: %d; "
                 "offset: %u; evsz: %lu\n",
                 iters, upper, lower, has_pos, has_neg, offset, evsz);
        _dprintf("---------------------------------------\n");
        iters += 1;

        // this is for error detection & backtracking, and handle a corner
        // case that the upper is a congruent line.
        // Note that if we have no pos, then upper is the old upper before
        // the binary search, that's why we use "upper - offset" here
        if (!has_pos &&
            testev(target, cands_o, upper - offset, test_config) < 0) {
            n_bctr += 1;
            is_reset = true;
            _dprintf("POS: backtrack\n");
        } else {
            _swap(cands[evsz], cands[upper - 1]);
            evsz += 1;
        }

        if (evsz >= exp_evsz &&
            testev(target, cands, evsz, test_config) == EV_POS) {
            evsz = prune_evcands(target, cands, evsz, test_config);
            if (evsz >= exp_evsz) {
                break;
            }
            n_bctr += 1;
        }

        lower = evsz;

        if (is_reset || (algo_config->slack && evsz > num_carried_cong)) {
            if (upper >= migrated) {
                // shuffle_evset(&cands[upper], n_cands - upper);
                migrated = n_cands - 1;
                _evset_stats.meet += 1;
                // _evset_stats.ooh += 1;
                // break;
            }

            size_t step = 3 * uncertainty / 2;
            for (size_t i = 0; i < step && upper < migrated;
                 upper++, migrated--, i++) {
                _swap(cands[upper], cands[migrated]);
            }
        }

        if (upper <= lower) {
            // this can happen if upper == evsz + 1 but evsz cannot evict target
            // which is a result of flaky replacement policy
            upper = lower + 1;
            if (upper > n_cands) {
                _error("Upper goes below lower and lower + 1 > n_cands %lu\n",
                       n_cands);
                _evset_stats.ooc += 1;
                break;
            }
        }
    }

    _evset_stats.backtracks += n_bctr;
    evset->size = evsz;
    memcpy(evset->addrs, cands, sizeof(*cands) * evsz);
    return false;
}

#define MAX_UPPER_HIST 50

// an implementation with an alternative backtracking mechanism;
// it has higher performance in local env and lower performance in cloud;
bool evset_builder_last_straw_dev(u8 *target, EVSet *evset) {
    u8 **cands = evset->cands->cands;
    size_t n_cands = evset->cands->size, evsz = 0;
    EVTestConfig *test_config = &evset->config->test_config;
    EVAlgoConfig *algo_config = &evset->config->algo_config;
    cache_param *target_cache = evset->target_cache;
    cand_test_func testev = test_config->test;
    u32 extra_cong = algo_config->extra_cong;
    u32 exp_evsz = target_cache->n_ways + extra_cong;

    if (n_cands <= 1) {
        return true;
    }

    u32 uncertainty = cache_uncertainty(target_cache);
    if (evset->config->cands_config.filter_ev) {
        uncertainty /= cache_uncertainty(
            evset->config->cands_config.filter_ev->target_cache);
    }

    i64 upper_hists[MAX_UPPER_HIST] = {0}, uh_idx = 0;

    u64 migrated = n_cands - 1, n_ways = target_cache->n_ways;
    u64 max_bctr = algo_config->max_backtrack;
    i64 lower = 0, upper = n_cands, cnt, n_bctr = 0;
    bool is_reset = false;
    bool only_recharge = false, double_bctr = false;
    u32 offset = 0;
    while (evsz < evset->cap && n_bctr < max_bctr) {

        if (evsz > 0 && !is_reset && evsz < evset->target_cache->n_ways) {
            u32 rem = evset->target_cache->n_ways - evsz; // rem > 0
            cnt = (upper * rem + lower) / (rem + 1);
            if (cnt == upper) {
                assert(cnt > 0);
                cnt -= 1; // needed for preserving invariant 2
            }
        } else {
            cnt = (upper + lower) / 2;
        }

        bool is_overprune = false, is_recharge = false;

        is_reset = false;
        u8 **cands_o = cands + offset;
        bool has_pos = false, has_neg = false;
        // u64 start = time_ns(), searches = 0;
        // i64 upper_before = upper;
        while (upper - lower > 1) {
            if (evsz < n_ways) {
                _evset_stats.pure_tests2 += 1;
                _evset_stats.pure_mem_acc2 += (cnt - offset);
            }
            _evset_stats.pure_tests += 1;
            _evset_stats.pure_mem_acc += (cnt - offset);
            EVTestRes res = testev(target, cands_o, cnt - offset, test_config);
            if (res > 0) {
                upper = cnt;
                has_pos = true;
            } else {
                lower = cnt;
                has_neg = true;
            }
            cnt = (upper + lower) / 2;
            // searches += 1;
        }
        // iters += 1;

        // this is for error detection & backtracking, and handle a corner
        // case that the upper is a congruent line.
        // Note that if we have no pos, then upper is the old upper before
        // the binary search, that's why we use "upper - offset" here
        if (!has_pos &&
            testev(target, cands_o, upper - offset, test_config) < 0) {
            if (only_recharge) {
                is_recharge = true;
            } else {
                is_overprune = true;
            }
        } else if (!has_neg) {
            is_recharge = true;
        } else {
            _swap(cands[evsz], cands[upper - 1]);
            evsz += 1;
            upper_hists[uh_idx] = upper;
            uh_idx = (uh_idx + 1) % MAX_UPPER_HIST;
        }

        // u64 dur = time_ns() - start;
        // _info("%ld->%ld; has_pos: %d; has_neg: %d; offset: %u is_op: %d; "
        //       "is_re: %d; only_re: %d; evsz: %lu; us: %luus; s: %lu\n", upper_before,
        //       cnt, has_pos, has_neg, offset, is_overprune, is_recharge, only_recharge, evsz,
        //       dur / 1000, searches);

        if (evsz >= exp_evsz &&
            testev(target, cands, evsz, test_config) == EV_POS) {
            // u32 old_evsz = evsz;
            evsz = prune_evcands(target, cands, evsz, test_config);
            only_recharge = true;
            // _info(BLUE_F " Pruned from %u to %ld\n" RESET_C, old_evsz, evsz);
            if (evsz >= exp_evsz) {
                break;
            }
            if (evsz >= target_cache->n_ways - algo_config->slack) {
                offset = evsz - (target_cache->n_ways - algo_config->slack);
            } else {
                offset = 0;
            }
        }

        lower = evsz;

        if (is_overprune) {
            n_bctr += 1;
            if (double_bctr) {
                uh_idx = (uh_idx + MAX_UPPER_HIST - 1) % MAX_UPPER_HIST;
            } else {
                uh_idx = (uh_idx + MAX_UPPER_HIST - 2) % MAX_UPPER_HIST;
            }

            double_bctr = true;
            if (upper_hists[uh_idx]) {
                upper = upper_hists[uh_idx];
                upper_hists[uh_idx] = 0;
            } else {
                _evset_stats.ooh += 1;
                upper = n_cands;
            }
            // if (upper > n_cands / 2) {
            //     n_bctr += 1;
            // }
            // _info(YELLOW_F "POS: backtrack=>%ld\n" RESET_C, upper);
        } else {
            double_bctr = false;
        }

        if (is_recharge) {
            only_recharge = true;
            n_bctr += 1;
            if (evsz > offset + 2 && !has_neg) {
                offset += 1;
            }

            if (upper >= migrated) {
                // shuffle_evset(&cands[upper], n_cands - upper);
                migrated = n_cands - 1;
            }

            size_t step = 3 * uncertainty / 2;
            for (size_t i = 0; i < step && upper < migrated;
                 upper++, migrated--, i++) {
                _swap(cands[upper], cands[migrated]);
            }
            // if (upper > n_cands / 2) {
            //     n_bctr += 1;
            // }
            // _info(GREEN_F "Recharge=>%ld\n" RESET_C, upper);
        }

        if (upper <= lower) {
            // this can happen if upper == evsz + 1 but evsz cannot evict target
            // which is a result of flaky replacement policy
            upper = lower + 1;
            if (upper > n_cands) {
                _error("Upper goes below lower and lower + 1 > n_cands %lu\n",
                       n_cands);
                _evset_stats.ooc += 1;
                break;
            }
        }
    }

    _evset_stats.backtracks += n_bctr;
    evset->size = evsz;
    memcpy(evset->addrs, cands, sizeof(*cands) * evsz);
    return false;
}

bool evset_builder_group_test(u8 *target, EVSet *evset, bool early_terminate) {
    u8 **cands = evset->cands->cands;
    size_t n_cands = evset->cands->size, evsz = 0;
    EVTestConfig *test_config = &evset->config->test_config;
    EVAlgoConfig *algo_config = &evset->config->algo_config;
    cache_param *target_cache = evset->target_cache;

    u32 n_grps = target_cache->n_ways + 1;
    double sz_ratio = (double)target_cache->n_ways / n_cands;
    double rate = (double)target_cache->n_ways / n_grps; // from vila et al.
    size_t n_backup = (size_t)(1.5 * log(sz_ratio) / log(rate));
    size_t *sz_backup = _calloc(n_backup, sizeof(*sz_backup));
    size_t bt_idx = 0, n_bctr = 0, max_backtrack = algo_config->max_backtrack;
    if (!sz_backup) {
        return true;
    }

    bool ooh = false; // out of history
    while (n_cands > target_cache->n_ways && n_bctr < max_backtrack && !ooh) {
        size_t _b_grpsz = n_cands / n_grps, rnd = n_cands % n_grps;
        size_t start_idx = 0, num_tests = _b_grpsz ? n_grps : n_cands;
        bool has_remove = false;
        for (u32 t = 0; t < num_tests; t++) {
            size_t grp_sz = _b_grpsz + (t < rnd);
            bool is_lst_grp = t == num_tests - 1;

            // move the removed group to the back
            if (!is_lst_grp) {
                for (u32 i = 0; i < grp_sz; i++) {
                    _swap(cands[start_idx + i], cands[n_cands - 1 - i]);
                }
            }

            _evset_stats.pure_tests += 1;
            _evset_stats.pure_tests2 += 1;
            _evset_stats.pure_mem_acc += (n_cands - grp_sz);
            _evset_stats.pure_mem_acc2 += (n_cands - grp_sz);
            EVTestRes res =
                test_config->test(target, cands, n_cands - grp_sz, test_config);
            if (res > 0) {
                // positive
                n_cands -= grp_sz;
                has_remove = true;

                // backup states
                sz_backup[bt_idx] = grp_sz;
                bt_idx = (bt_idx + 1) % n_backup;

                if (early_terminate) {
                    break;
                }
            } else {
                // erroneous state, backtrack!
                if (is_lst_grp && !has_remove) {
                    n_bctr += 1;
                    bt_idx = (bt_idx + n_backup - 1) % n_backup;
                    if (sz_backup[bt_idx] == 0) {
                        ooh = true;
                        _evset_stats.ooh += 1;
                    } else {
                        n_cands += sz_backup[bt_idx];
                        sz_backup[bt_idx] = 0;
                    }
                    break;
                }

                // restore the pruned group
                if (!is_lst_grp) {
                    for (u32 i = 0; i < grp_sz; i++) {
                        _swap(cands[start_idx + i], cands[n_cands - 1 - i]);
                    }
                }
                start_idx += grp_sz;
            }
        }
    }

    _evset_stats.backtracks += n_bctr;

    evsz = _min(evset->cap, n_cands);
    evset->size = evsz;
    memcpy(evset->addrs, cands, sizeof(*cands) * evsz);
    _free(sz_backup);
    return false;
}

bool evset_builder_group_test_random(u8 *target, EVSet *evset) {
    u8 **cands = evset->cands->cands;
    size_t n_cands = evset->cands->size, evsz = 0;
    EVTestConfig *test_config = &evset->config->test_config;
    EVAlgoConfig *algo_config = &evset->config->algo_config;
    cache_param *target_cache = evset->target_cache;

    u32 n_grps = target_cache->n_ways + 1;
    double sz_ratio = (double)target_cache->n_ways / n_cands;
    double rate = (double)target_cache->n_ways / n_grps; // from vila et al.
    size_t n_backup = (size_t)(1.5 * log(sz_ratio) / log(rate));
    size_t *sz_backup = _calloc(n_backup, sizeof(*sz_backup));
    size_t bt_idx = 0, n_bctr = 0, max_backtrack = algo_config->max_backtrack;
    if (!sz_backup) {
        return true;
    }

    bool early_terminate = true;
    bool ooh = false; // out of history
    while (n_cands > target_cache->n_ways && n_bctr < max_backtrack && !ooh) {
        size_t _b_grpsz = n_cands / n_grps, rnd = n_cands % n_grps;
        size_t num_tests = _b_grpsz ? n_grps : n_cands;
        bool has_remove = false;
        for (u32 t = 0; t < num_tests; t++) {
            size_t grp_sz = _b_grpsz + (t < rnd);
            bool is_lst_grp = t == num_tests - 1;

            shuffle_evset(cands, n_cands);

            _evset_stats.pure_tests += 1;
            _evset_stats.pure_tests2 += 1;
            _evset_stats.pure_mem_acc += (n_cands - grp_sz);
            _evset_stats.pure_mem_acc2 += (n_cands - grp_sz);

            EVTestRes res =
                test_config->test(target, cands, n_cands - grp_sz, test_config);
            if (res > 0) {
                // positive
                n_cands -= grp_sz;
                has_remove = true;

                // backup states
                sz_backup[bt_idx] = grp_sz;
                bt_idx = (bt_idx + 1) % n_backup;

                if (early_terminate) {
                    break;
                }
            } else {
                // erroneous state, backtrack!
                if (is_lst_grp && !has_remove) {
                    n_bctr += 1;
                    bt_idx = (bt_idx + n_backup - 1) % n_backup;
                    if (sz_backup[bt_idx] == 0) {
                        ooh = true;
                        _evset_stats.ooh += 1;
                    } else {
                        n_cands += sz_backup[bt_idx];
                        sz_backup[bt_idx] = 0;
                    }
                    break;
                }
            }
        }
    }

    _evset_stats.backtracks += n_bctr;

    evsz = _min(evset->cap, n_cands);
    evset->size = evsz;
    memcpy(evset->addrs, cands, sizeof(*cands) * evsz);
    _free(sz_backup);
    return false;
}

#define MAX_ITERS 10000

bool skx_sf_evset_builder_prime_scope(u8 *target, EVSet *evset, bool migrate) {
    u8 **cands = evset->cands->cands, *tlb_target = tlb_warmup_ptr(target);
    size_t n_cands = evset->cands->size, evsz = 0, iters = 0;
    EVTestConfig *test_config = &evset->config->test_config;
    EVAlgoConfig *algo_config = &evset->config->algo_config;
    cache_param *target_cache = evset->target_cache;
    u32 max_old_lines = target_cache->n_ways - algo_config->slack;
    cand_test_func testev = test_config->test;
    u32 extra_cong = algo_config->extra_cong;
    u32 exp_evsz = target_cache->n_ways + extra_cong;
    u64 migrated_lb = 0, migrated_ub = n_cands - 1, last_idx = 0;
    u32 uncertainty = cache_uncertainty(target_cache);
    if (evset->config->cands_config.filter_ev) {
        uncertainty /= cache_uncertainty(
            evset->config->cands_config.filter_ev->target_cache);
    }

    u32 n_ways = target_cache->n_ways;
    while (evsz < evset->cap && iters < MAX_ITERS) {
        u32 init_idx = 0;
        if (algo_config->slack && evsz > max_old_lines) {
            init_idx = evsz - max_old_lines;
        }
        if (init_idx > 0 && migrate) {
            migrated_lb = _max(last_idx, evsz);
            size_t step = 3 * uncertainty / 2;
            for (size_t i = 0; i < step && migrated_lb < migrated_ub;
                 migrated_lb++, migrated_ub--, i++) {
                _swap(cands[migrated_lb], cands[migrated_ub]);
            }
        }
        _evset_stats.pure_tests += 1;
        if (evsz < n_ways) {
            _evset_stats.pure_tests2 += 1;
        }

        _maccess(target);
        helper_thread_read_single(target, test_config->hctrl);

        bool found = false;
        u32 local_iters = 0;
        while (!found && iters < MAX_ITERS && local_iters < 5) {
            for (u32 idx = init_idx; idx < n_cands; idx++) {
                _maccess(cands[idx]);
                helper_thread_read_single(cands[idx], test_config->hctrl);
                _lfence();
                _maccess(tlb_target);
                u64 lat = _time_maccess(target);
                if (lat > test_config->lat_thresh &&
                    lat < detected_cache_lats.interrupt_thresh) {
                    found = true;
                    last_idx = idx;
                    _swap(cands[evsz], cands[idx]);
                    evsz += 1;
                    break;
                }
                _evset_stats.pure_mem_acc += 1;
                if (evsz < n_ways) {
                    _evset_stats.pure_mem_acc2 += 1;
                }
            }
            iters += 1;
            local_iters += 1;
        }

        if (evsz >= exp_evsz &&
            testev(target, cands, evsz, test_config) == EV_POS) {
            evsz = prune_evcands(target, cands, evsz, test_config);
            if (evsz >= exp_evsz) {
                break;
            }
        }
    }
    evset->size = evsz;
    memcpy(evset->addrs, cands, sizeof(*cands) * evsz);
    return false;
}

void default_l1d_evset_build_config(EVBuildConfig *config) {
    config->cands_config =
        (EVCandsConfig){.scaling = 1, .filter_ev = NULL};
    config->test_config =
        (EVTestConfig){.lat_thresh = detected_cache_lats.l1d_thresh,
                       .trials = 10,
                       .low_bnd = 3,
                       .upp_bnd = 7,
                       .ev_repeat = 4,
                       .unsure_retry = 5,
                       .test_scale = 1,
                       .access_cnt = 3,
                       .lower_ev = NULL,
                       .need_helper = false,
                       .flush_cands = false,
                       .foreign_evictor = false,
                       .hctrl = NULL,
                       .traverse = generic_cands_traverse,
                       .test = generic_test_eviction};
    config->algo_config = (EVAlgoConfig){.cap_scaling = 1,
                                         .verify_retry = 1,
                                         .retry_timeout = 10,
                                         .max_backtrack = 10,
                                         .slack = 0,
                                         .extra_cong = 0,
                                         .ret_partial = false,
                                         .prelim_test = false,
                                         .need_skx_sf_ext = false};
    config->algorithm = EVSET_ALGO_DEFAULT;
}

void default_l2_evset_build_config(EVBuildConfig *config) {
    config->cands_config =
        (EVCandsConfig){.scaling = 3, .filter_ev = NULL};
    config->test_config =
        (EVTestConfig){.lat_thresh = detected_cache_lats.l2_thresh,
                       .trials = 10,
                       .low_bnd = 3,
                       .upp_bnd = 7,
                       .ev_repeat = 4,
                       .unsure_retry = 5,
                       .test_scale = 1,
                       .access_cnt = 3,
                       .lower_ev = NULL,
                       .need_helper = false,
                       .flush_cands = false,
                       .foreign_evictor = false,
                       .hctrl = NULL,
                       .traverse = generic_cands_traverse,
                       .test = generic_test_eviction};
    config->algo_config = (EVAlgoConfig){.cap_scaling = 2,
                                         .verify_retry = 5,
                                         .retry_timeout = 20,
                                         .max_backtrack = 20,
                                         .slack = 0,
                                         .extra_cong = 0,
                                         .ret_partial = false,
                                         .prelim_test = false,
                                         .need_skx_sf_ext = false};
    config->algorithm = EVSET_ALGO_DEFAULT;
}

void default_skx_sf_evset_build_config(EVBuildConfig *config, EVSet *l1d_ev,
                                       EVSet *l2_ev,
                                       helper_thread_ctrl *hctrl) {
    config->cands_config =
        (EVCandsConfig){.scaling = 3, .filter_ev = l2_ev};
    config->test_config =
        (EVTestConfig){.lat_thresh = detected_cache_lats.l3_thresh,
                       .trials = 4,
                       .low_bnd = 2,
                       .upp_bnd = 2,
                       .ev_repeat = 1,
                       .block = 24,
                       .stride = 12,
                       .unsure_retry = 3,
                       .test_scale = 1,
                       .access_cnt = 1,
                       .lower_ev = l2_ev,
                       .need_helper = true,
                       .flush_cands = false,
                       .foreign_evictor = false,
                       .hctrl = hctrl,
                       .traverse = skx_sf_cands_traverse_mt,
                       .test = generic_test_eviction};

    config->test_config_alt =
        (EVTestConfig){.lat_thresh = detected_cache_lats.l2_thresh,
                       .trials = 5,
                       .low_bnd = 2,
                       .upp_bnd = 3,
                       .ev_repeat = 10,
                       .unsure_retry = 5,
                       .test_scale = 1,
                       .access_cnt = 1,
                       .lower_ev = NULL,
                       .need_helper = false,
                       .flush_cands = true,
                       .foreign_evictor = false,
                       .hctrl = hctrl,
                       .traverse = generic_cands_traverse,
                       .test = generic_test_eviction};

    config->algo_config = (EVAlgoConfig){.cap_scaling = 2,
                                         .verify_retry = 10,
                                         .retry_timeout = 1000,
                                         .max_backtrack = 20,
                                         .slack = 2,
                                         .extra_cong = SF_ASSOC - detected_l3->n_ways, // ICELAKE
                                         .ret_partial = false,
                                         .prelim_test = false,
                                         .need_skx_sf_ext = false};
    config->algorithm = EVSET_ALGO_DEFAULT;
}

size_t prune_evcands(u8 *target, u8 **cands, size_t cnt, EVTestConfig *tconf) {
    u64 start_ns = time_ns();
    for (size_t i = 0; i < cnt;) {
        _swap(target, cands[i]);
        EVTestRes tres = tconf->test(target, cands, cnt, tconf);
        _swap(target, cands[i]);
        if (tres < 0) {
            cnt -= 1;
            _swap(cands[i], cands[cnt]);
        } else {
            i += 1;
        }
    }
    u64 end_ns = time_ns();
    _evset_stats.pruning_duration += (end_ns - start_ns);
    return cnt;
}

EVSet *prune_EVSet(u8 *target, EVSet *evset) {
    u32 cnt = prune_evcands(target, evset->addrs, evset->size,
                            &evset->config->test_config);
    evset->size = cnt;
    return evset;
}

EVSet *extend_skx_sf_EVSet(EVSet *evset) {
    u64 start = time_ns(), n_ways = evset->target_cache->n_ways;
    u64 exp = n_ways + evset->config->algo_config.extra_cong;

    if (evset->size == evset->cap) return evset;
    if (evset->size >= exp) return evset;

    u8 **cands = evset->cands->cands;
    for (i64 i = evset->cands->size - 1; i >= 0 && evset->size < evset->cap;
         i--) {
        u8 *ptr = cands[i];
        if (generic_evset_test(ptr, evset) == EV_POS) {
            evset->addrs[evset->size] = ptr;
            _swap(cands[evset->size], cands[i]);
            evset->size += 1;
            if (evset->size >= exp) {
                break;
            }
        }
    }
    _evset_stats.extension_duration += time_ns() - start;
    return evset;
}

static bool _copy_test_config = true;

EVSet *build_evset_generic(u8 *target, EVBuildConfig *config,
                           cache_param *cache, EVCands *evcands) {
    EVSet *evset = evset_new(page_offset(target), config, cache, evcands);
    if (!evset) return NULL;

    if (_copy_test_config) {
        EVBuildConfig *_conf = _calloc(1, sizeof(*_conf));
        if (!_conf) {
            return NULL;
        }
        memcpy(_conf, evset->config, sizeof(*_conf));
        evset->config = _conf;
        config = _conf;
    }

    if (cache_uncertainty(cache) == 1) {
        u8 **cands = evset->cands->cands;
        size_t nlines = cache->n_ways;
        memcpy(evset->addrs, cands, nlines * sizeof(*cands));
        evset->size = nlines;
        return evset;
    }

    if (evset->config->algo_config.prelim_test &&
        generic_test_eviction(target, evset->cands->cands, evset->cands->size,
                              &evset->config->test_config) < 0) {
        _evset_stats.ooc += 1;
        return NULL;
    }

    // evset_algorithm algo = evset->config->algorithm;
    // if ((algo == EVSET_ALGO_GROUP_TEST ||
    //      algo == EVSET_ALGO_GROUP_TEST_RANDOM ||
    //      algo == EVSET_ALGO_GROUP_TEST_NOEARLY ||
    //      algo == EVSET_ALGO_PPP) &&
    //      evset->config->algo_config.extra_cong) {
    //     evset->config->algo_config.need_skx_sf_ext = true;
    // }
    if (evset->config->algo_config.extra_cong) {
        evset->config->algo_config.need_skx_sf_ext = true;
    }

    bool err = false, can_evict = false;
    bool start_helper =
        config->test_config.need_helper && !config->test_config.hctrl->running;
    if (start_helper) {
        start_helper_thread(config->test_config.hctrl);
    }

    u64 old_retry = _evset_stats.retries;
    u64 start_ns = time_ns(), timeout = config->algo_config.retry_timeout;
    u64 retry_ns = 0;
    for (u32 r = 0; r < config->algo_config.verify_retry; r++) {
        u64 old_bctr = _evset_stats.backtracks, iter_ns = time_ns();
        switch (config->algorithm) {
            case EVSET_ALGO_NAIVE: {
                err = evset_builder_naive(target, evset);
                break;
            }
            case EVSET_ALGO_GROUP_TEST: {
                err = evset_builder_group_test(target, evset, true);
                break;
            }
            case EVSET_ALGO_GROUP_TEST_NOEARLY: {
                err = evset_builder_group_test(target, evset, false);
                break;
            }
            case EVSET_ALGO_GROUP_TEST_RANDOM: {
                err = evset_builder_group_test_random(target, evset);
                break;
            }
            case EVSET_ALGO_LAST_STRAW: {
                err = evset_builder_last_straw(target, evset);
                break;
            }
            case EVSET_ALGO_LAST_STRAW_DEV: {
                err = evset_builder_last_straw_dev(target, evset);
                break;
            }
            case EVSET_ALGO_PRIME_SCOPE: {
                err = skx_sf_evset_builder_prime_scope(target, evset, false);
                break;
            }
            case EVSET_ALGO_PRIME_SCOPE_OPT: {
                err = skx_sf_evset_builder_prime_scope(target, evset, true);
                break;
            }
            case EVSET_ALGO_INVALID: {
                _error("Invalid eviction set construction algorithm!\n");
                return NULL;
            }
        }
        u64 bctr = _evset_stats.backtracks - old_bctr;
        inc_bctr(bctr);
        inc_bctr_dura(bctr, time_ns() - iter_ns);

        if (err) {
            _info("Err\n");
            break;
        }

        if (config->test_config.test(target, evset->addrs, evset->size,
                                     &config->test_config) == EV_POS) {
            if (config->algo_config.need_skx_sf_ext) {
                extend_skx_sf_EVSet(evset);
                prune_EVSet(target, evset);
                extend_skx_sf_EVSet(evset);
            }

            if (!config->algo_config.need_skx_sf_ext ||
                config->test_config.test(target, evset->addrs, evset->size,
                                         &config->test_config) == EV_POS) {

                can_evict = true;
                inc_useful_bctr(bctr);
                inc_useful_bctr_dura(bctr, time_ns() - iter_ns);
                break;
            }
        }

        if (timeout && (time_ns() - start_ns) > timeout * 1000000) {
            _evset_stats.timeout += 1;
            _error("Timeout! Target level: %u; cands: %lu\n", cache->level,
                   evset->cands->size);
            break;
        }
        if (r == 0) {
            retry_ns = time_ns();
        }
        _evset_stats.retries += 1;
        _dprintf("--- EVSet Retry ---\n");
    }
    u64 build_dura = time_ns() - start_ns;
    _evset_stats.build_duration += build_dura;
    if (retry_ns) {
        _evset_stats.retry_duration += time_ns() - retry_ns;
    }

    u64 retry = _evset_stats.retries - old_retry;
    inc_retry(retry);
    inc_retry_dura(retry, build_dura);
    if (can_evict) {
        inc_useful_retry(retry);
        inc_useful_retry_dura(retry, build_dura);
    }

    // if (config->algo_config.need_skx_sf_ext) {
    //     u64 start = time_ns();
    //     extend_skx_sf_EVSet(evset);
    //     prune_EVSet(target, evset);
    //     _evset_stats.build_duration += time_ns() - start;
    // }

    if (start_helper) {
        stop_helper_thread(config->test_config.hctrl);
    }

    if (!can_evict && !config->algo_config.ret_partial) goto err;

    return evset;

err:
    evset_free(evset);
    return NULL;
}

EVSet *build_l1d_EVSet(u8 *target, EVBuildConfig *config, EVCands *evcands) {
    return build_evset_generic(target, config, detected_l1d, evcands);
}

EVSet *build_l2_EVSet(u8 *target, EVBuildConfig *config, EVCands *evcands) {
    return build_evset_generic(target, config, detected_l2, evcands);
}

EVSet *build_skx_sf_EVSet(u8 *target, EVBuildConfig *config, EVCands *evcands) {
    return build_evset_generic(target, config, detected_l3, evcands);
}

EVSet **build_evsets_at(u32 offset, EVBuildConfig *conf, cache_param *cache,
                        EVCands *_cands, size_t *ev_cnt,
                        cache_param *lower_cache, EVBuildConfig *lower_conf,
                        EVSet **lower_evsets, size_t n_lower_evsets) {
    EVCands *cands = _cands;
    u8 **cands_backup = NULL;
    size_t cands_sz_backup = 0;
    EVSet **evsets = NULL;
    size_t n_evsets = cache_uncertainty(cache), lower_skipped = 0;;
    if (conf->cands_config.filter_ev) {
        cache_param *lower = conf->cands_config.filter_ev->target_cache;
        n_evsets /= cache_uncertainty(lower);
    }
    *ev_cnt = n_evsets;

    u8 **addrs = NULL;
    size_t acc_cnt = 0;
    if (!cands) {
        cands = evcands_new(cache, &conf->cands_config, NULL);
        if (!cands) {
            _error("Failed to allocate evcands\n");
            goto err;
        }

        if (evcands_populate(offset, cands, &conf->cands_config)) {
            _error("Failed to populate evcands\n");
            goto err;
        }
    }
    cands_backup = cands->cands;
    cands_sz_backup = cands->size;

    evsets = _calloc(n_evsets, sizeof(EVSet *));
    if (!evsets || cands->size == 0) {
        goto err;
    }

    // EVCands *lower_cands = NULL;
    // if (lower_cache && lower_conf) {
    //     lower_cands = evcands_new(lower_cache, &lower_conf->cands_config, NULL);
    //     if (!lower_cands) {
    //         _error("Failed to allocate lower cands\n");
    //         goto err;
    //     }

    //     if (evcands_populate(offset, lower_cands, &lower_conf->cands_config)) {
    //         _error("Failed to filter lower cands\n");
    //         goto err;
    //     }

    //     printf("Candidate size: %lu\n", lower_cands->size);
    // }

    u64 l2_time_ns = 0;
    u8 *target = cands->cands[cands->size - 1];
    cands->size -= 1;
    for (size_t i = 0; i < n_evsets && cands->size > 0; i++) {
        EVSet *evset = NULL;
        // EVSet *lower_evset = NULL;
        // if (lower_cache && lower_conf) {
        //     u64 start = time_ns();
        //     for (u32 j = 0; j < 5; j++) {
        //         lower_evset = build_evset_generic(target, lower_conf,
        //                                           lower_cache, lower_cands);
        //         if (lower_evset) {
        //             break;
        //         }
        //     }
        //     l2_time_us += (time_ns() - start) / 1e3;
        //     if (!lower_evset) {
        //         // printf("Lower skip!\n");
        //         lower_skipped += 1;
        //         continue;
        //     }
        //     conf->test_config.lower_ev = lower_evset;
        // }

        if (n_lower_evsets) {
            u64 start = time_ns();
            conf->test_config.lower_ev = NULL;
            for (u32 i = 0; i < n_lower_evsets; i++) {
                if (generic_evset_test(target, lower_evsets[i]) > 0) {
                    conf->test_config.lower_ev = lower_evsets[i];
                    break;
                }
            }
            l2_time_ns += (time_ns() - start);

            if (!conf->test_config.lower_ev) {
                lower_skipped += 1;
                goto l2_ev_fail;
            }
        }

        evset = build_evset_generic(target, conf, cache, cands);
        if (lower_cache && lower_conf) {
            printf("\rProgress: %lu/%lu; L2 selection: %.2fms", i, n_evsets,
                   l2_time_ns / 1e6);
        }

        if (evset) {
            cands->cands += evset->size;
            cands->size -= evset->size;
            if (evset->size < evset->cap) {
                // the first entry is the target
                _swap(evset->addrs[0], evset->addrs[evset->size]);
                evset->addrs[0] = target;
                evset->size += 1;
            }

            evsets[i] = evset;
            if (!addrs) {
                addrs = _calloc(evset->cap * n_evsets, sizeof(*addrs));
                if (!addrs)
                    goto err;
            }

            memcpy(&addrs[acc_cnt], evset->addrs, evset->size * sizeof(*addrs));
            acc_cnt += evset->size;
        }

l2_ev_fail:
        if (i == n_evsets - 1 || cands->size == 0) break;

        // find the next target
        if (addrs) {
            bool found = false;
            for (size_t j = 0; j < cands->size && cands->size > 0; j++) {
                EVTestRes res = generic_test_eviction(
                    cands->cands[j], addrs, acc_cnt, &conf->test_config);
                if (res == EV_NEG) {
                    target = cands->cands[j];
                    _swap(cands->cands[j], cands->cands[cands->size - 1]);
                    cands->size -= 1;
                    found = true;
                    break;
                }
            }

            if (!found) {
                if (cache_oracle_inited() && evset) {
                    printf("------ %lu ------\n", i);
                    for (size_t k = 0; k < evset->size; k++) {
                        printf("%lu: %p (%#lx)\n", k, evset->addrs[k],
                            llc_addr_hash(evset->addrs[k]));
                    }
                }
                _error("Cannot find the next target: cands size: %lu; addr: %lu\n",
                       cands->size, acc_cnt);
                _evset_stats.no_next += (n_evsets - (i + 1));
                break;
            }
        } else {
            cands->size -= 1;
            target = cands->cands[cands->size];
        }
    }

cleanup:
    if (lower_skipped) {
        _info("Skipped due to lower evset fail: %lu\n", lower_skipped);
    }

    if (cands_backup) {
        cands->cands = cands_backup;
        cands->size = cands_sz_backup;
    }
    _free(addrs);
    return evsets;

err:
    // for (size_t i = 0; i < n_evsets; i++) {
    //     evset_free(evsets[i]);
    // }
    _free(evsets);
    evsets = NULL;
    goto cleanup;
}
