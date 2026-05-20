#include "core.h"
#include "sync.h"
#include "cache/cache.h"
#include <getopt.h>
#include "osc-common.h"
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

static evset_algorithm evalgo = EVSET_ALGO_DEFAULT;
static double cands_scaling = 3;
static size_t extra_cong = 1;
static size_t max_tries = 10, max_backtrack = 20, max_timeout = 0;
static size_t total_runtime_limit = 0; // in minutes
static bool l2_filter = true, single_thread = false;
static size_t n_para = 0;
static size_t num_l2sets;
static helper_thread_ctrl hctrl;

#define NUM_OFFSETS (PAGE_SIZE / CL_SIZE)

static void free_evcands_complex(EVCands ***complex);

EVSet ***build_l2_evsets_all() {
    u64 start = time_ns();
    size_t l2_cnt;
    EVCands *l2_evcands =
        evcands_new(detected_l2, &def_l2_ev_config.cands_config, NULL);
    if (!l2_evcands) {
        _error("Failed to allocate L2 evcands\n");
        return NULL;
    }

    if (evcands_populate(0x0, l2_evcands, &def_l2_ev_config.cands_config)) {
        _error("Failed to populate L2 evcands\n");
        return NULL;
    }

    EVSet **evsets = NULL;
    for (u32 i = 0; i < 5; i++) {
        _info("Building L2 evset, iter=%u\n", i);
        evsets = build_evsets_at(0x0, &def_l2_ev_config, detected_l2,
                                 l2_evcands, &l2_cnt, NULL, NULL, NULL, 0);
        bool has_fail = false;
        for (size_t i = 0; i < l2_cnt; i++) {
            if (!evsets[i] || !evsets[i]->addrs ||
                evsets[i]->size < detected_l2->n_ways) {
                has_fail = true;
            }
        }

        if (has_fail) goto l2_err;

        u32 l2_test = 0;
        for (size_t i = 0; i < l2_cnt; i++) {
            if (!evsets[i] || !evsets[i]->addrs) continue;

            l2_test += evset_self_test(evsets[i]) == EV_POS;
        }

        if (l2_test != cache_uncertainty(detected_l2)) {
            goto l2_err;
        }

        l2_test = 0;
        for (size_t i = 0; i < l2_cnt - 1; i++) {
            for (size_t j = i + 1; j < l2_cnt; j++) {
                u8 *ptr = evsets[j]->addrs[0];
                l2_test += generic_evset_test(ptr, evsets[i]) == EV_NEG;
            }
        }

        if (l2_test == (l2_cnt - 1) * l2_cnt / 2) {
            break;
        }

    l2_err:
        evsets = NULL;
        _error("Failed to build L2 evset, iter=%u\n", i);
    }

    if (!evsets) {
        _error("Cannot build L2 ev set for all uncertain sets\n");
        return NULL;
    }

    if (cache_oracle_inited()) {
        u32 cnts[16] = {0};
        for (u32 i = 0; i < 16; i++) {
            u32 l2_set = cache_set_idx(evsets[i]->addrs[0], detected_l2) >> 6;
            cnts[l2_set] += 1;
        }

        bool succ = true;
        for (u32 i = 0; i < 16; i++) {
            if (cnts[i] != 1) {
                printf("No or multiple evset at set %#x (%u)\n", i, cnts[i]);
                succ = false;
            }
        }
        if (succ) {
            printf("L2 EVSET pass!\n");
        }
    }

    EVSet ***l2evset_complex = calloc(NUM_OFFSETS, sizeof(*l2evset_complex));
    l2evset_complex[0] = evsets;
    for (u32 n = 1; n < NUM_OFFSETS; n++) {
        l2evset_complex[n] = calloc(l2_cnt, sizeof(EVSet *));
        for (u32 i = 0; i < l2_cnt; i++) {
            l2evset_complex[n][i] = evset_shift(evsets[i], CL_SIZE * n);
        }
    }
    u64 end = time_ns();
    _info("L2 Complex: %luus;\n", (end - start) / 1000);
    return l2evset_complex;
}

EVCands ***build_evcands_all(EVBuildConfig *conf, EVSet ***l2evsets) {
    u64 start, end;
    start = time_ns();
    EVCands *base_cands = evcands_new(detected_l3, &conf->cands_config, NULL);
    if (!base_cands) {
        _error("Failed to allocate EVB\n");
        return NULL;
    }
    end = time_ns();
    _info("EVCands Complex Alloc: %luus;\n", (end - start) / 1000);

    start = time_ns();
    EVCands ***cands_complex = calloc(NUM_OFFSETS, sizeof(*cands_complex));
    if (!cands_complex) {
        _error("Failed to allocate EVCands complex\n");
        goto err;
    }
    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        u32 offset = n * CL_SIZE;
        cands_complex[n] = calloc(num_l2sets, sizeof(EVCands *));
        if (!cands_complex[n]) {
            _error("Failed to allocate EVCands sub-complex\n");
            goto err;
        }
        for (u32 i = 0; i < num_l2sets; i++) {
            if (n == 0) {
                if (l2_filter) {
                    conf->cands_config.filter_ev = l2evsets[n][i];
                } else {
                    conf->cands_config.filter_ev = NULL;
                }
                EVCands *cands = evcands_new(detected_l3, &conf->cands_config,
                                             base_cands->evb);
                if (!cands) {
                    goto err;
                }

                if (evcands_populate(offset, cands, &conf->cands_config)) {
                    evcands_free(cands);
                    goto err;
                }
                cands_complex[n][i] = cands;
            } else {
                cands_complex[n][i] =
                    evcands_shift(cands_complex[0][i], offset);
                if (!cands_complex[n][i]) {
                    goto err;
                }
            }
        }
    }
    end = time_ns();
    _info("EVCands Complex Populate: %luus;\n", (end - start) / 1000);
    evcands_free(base_cands);
    return cands_complex;

err:
    free_evcands_complex(cands_complex);
    evcands_free(base_cands);
    return NULL;
}

static void shuffle_index(u32 *idxs, u32 sz) {
    srand(time(NULL));
    for (u32 tail = sz - 1; tail > 0; tail--) {
        u32 n_choice = tail + 1;
        u32 choice = rand() % n_choice;
        _swap(idxs[choice], idxs[tail]);
    }
}

static u32 n_cores(void) {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    return n > 0 ? (u32)n : 0;
}

static u32 smt_id(u32 cpu) {
    char path[128];
    u32 id = cpu;
    FILE *fp;

    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list",
             cpu);
    fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%u", &id) != 1) id = cpu;
        fclose(fp);
    }
    return id;
}

static u32 pick_cores(u32 *cores, u32 n_needed) {
    cpu_set_t allowed;
    bool check_allowed = sched_getaffinity(0, sizeof(allowed), &allowed) == 0;
    u32 n_cpu = _min(n_cores(), CPU_SETSIZE);
    u32 ids[CPU_SETSIZE];
    u32 n = 0;

    for (u32 cpu = 0; cpu < n_cpu; cpu++) {
        bool seen = false;
        u32 id;

        if (check_allowed && !CPU_ISSET(cpu, &allowed)) continue;

        id = smt_id(cpu);
        for (u32 i = 0; i < n; i++) {
            if (ids[i] == id) {
                seen = true;
                break;
            }
        }
        if (seen) continue;

        ids[n] = id;
        if (cores) cores[n] = cpu;
        n++;
        if (n_needed && n == n_needed) break;
    }

    return n;
}

static bool set_n_para(size_t n) {
    if (n < 2 || n % 2) {
        _error("Parallel construction requires an even number of cores >= 2; "
               "each construction thread needs one helper thread\n");
        return false;
    }

    u32 n_avail = pick_cores(NULL, 0);
    if (n > n_avail) {
        _error("Need %lu non-SMT cores for parallel construction; only %u "
               "available\n",
               n, n_avail);
        return false;
    }

    n_para = n;
    return true;
}

typedef struct {
    EVSet ****sfevset_complex;
    EVSet ***l2evsets;
    EVCands ***sf_cands;
    u32 *cores;
    u32 *idxs;
    u32 n_offsets;
    u32 next_offset;
    cache_param *lower_cache;
    EVBuildConfig *lower_conf;
    size_t n_lower_evsets;
    EVBuildConfig base_config;
    u64 start_time;
    bool stop;
    bool failed;
    pthread_mutex_t work_lock;
} para_build_ctx;

typedef struct {
    para_build_ctx *ctx;
    u32 pair_idx;
    helper_thread_ctrl hctrl;
    struct evset_stats stats;
} para_build_worker;

static void merge_evset_stats(struct evset_stats *dst,
                              const struct evset_stats *src) {
    dst->alloc_duration += src->alloc_duration;
    dst->population_duration += src->population_duration;
    dst->build_duration += src->build_duration;
    dst->pruning_duration += src->pruning_duration;
    dst->extension_duration += src->extension_duration;
    dst->retries += src->retries;
    dst->backtracks += src->backtracks;
    dst->cands_tests += src->cands_tests;
    dst->mem_accs += src->mem_accs;
    dst->pure_mem_acc += src->pure_mem_acc;
    dst->pure_tests += src->pure_tests;
    dst->pure_mem_acc2 += src->pure_mem_acc2;
    dst->pure_tests2 += src->pure_tests2;
    dst->pos_unsure += src->pos_unsure;
    dst->neg_unsure += src->neg_unsure;
    dst->ooh += src->ooh;
    dst->ooc += src->ooc;
    dst->no_next += src->no_next;
    dst->timeout += src->timeout;
    dst->meet += src->meet;
    dst->retry_duration += src->retry_duration;

    for (u32 i = 0; i < MAX_RETRY_REC; i++) {
        dst->retry_dist[i] += src->retry_dist[i];
        dst->useful_retry_dist[i] += src->useful_retry_dist[i];
        dst->retry_duras[i] += src->retry_duras[i];
        dst->useful_retry_duras[i] += src->useful_retry_duras[i];
    }
    for (u32 i = 0; i < MAX_BACKTRACK_REC; i++) {
        dst->bctr_dist[i] += src->bctr_dist[i];
        dst->useful_bctr_dist[i] += src->useful_bctr_dist[i];
        dst->bctr_duras[i] += src->bctr_duras[i];
        dst->useful_bctr_duras[i] += src->useful_bctr_duras[i];
    }
}

static bool set_thread_affinity(pthread_t thread, u32 core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(thread, sizeof(set), &set) == 0;
}

static bool next_offset_work(para_build_ctx *ctx, u32 *idx) {
    bool has_work = false;

    pthread_mutex_lock(&ctx->work_lock);
    if (!ctx->stop && ctx->next_offset < ctx->n_offsets) {
        *idx = ctx->next_offset++;
        has_work = true;
    }
    pthread_mutex_unlock(&ctx->work_lock);

    return has_work;
}

static void request_parallel_stop(para_build_ctx *ctx) {
    pthread_mutex_lock(&ctx->work_lock);
    ctx->stop = true;
    pthread_mutex_unlock(&ctx->work_lock);
}

static void free_l2evset_complex(EVSet ***complex) {
    if (!complex) return;

    size_t l2_cnt = cache_uncertainty(detected_l2);
    EVCands *cands = NULL;
    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        if (!complex[n]) continue;
        for (u32 i = 0; i < l2_cnt; i++) {
            EVSet *evset = complex[n][i];
            if (!evset) continue;

            if (!cands) cands = evset->cands;
            if (n == 0 && evset->config) {
                _free(evset->config);
                evset->config = NULL;
            }
            evset_free(evset);
        }
        _free(complex[n]);
    }
    if (cands) evcands_free(cands);
    _free(complex);
}

static void free_evcands_complex(EVCands ***complex) {
    if (!complex) return;

    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        if (!complex[n]) continue;
        for (u32 i = 0; i < num_l2sets; i++) {
            evcands_free(complex[n][i]);
        }
        _free(complex[n]);
    }
    _free(complex);
}

static void free_sfevset_complex(EVSet ****complex, size_t l3_cnt) {
    if (!complex) return;

    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        if (!complex[n]) continue;
        for (u32 i = 0; i < num_l2sets; i++) {
            if (!complex[n][i]) continue;
            for (u32 j = 0; j < l3_cnt; j++) {
                EVSet *evset = complex[n][i][j];
                if (!evset) continue;
                if (evset->config) {
                    _free(evset->config);
                    evset->config = NULL;
                }
                evset_free(evset);
            }
            _free(complex[n][i]);
        }
        _free(complex[n]);
    }
    _free(complex);
}

static void attach_helper_to_sfevsets(EVSet ****complex, u32 *idxs,
                                      u32 n_offset, size_t l3_cnt,
                                      helper_thread_ctrl *ctrl) {
    if (!complex || !ctrl) return;

    for (u32 c = 0; c < n_offset; c++) {
        u32 n = idxs[c];
        if (!complex[n]) continue;
        for (u32 i = 0; i < num_l2sets; i++) {
            if (!complex[n][i]) continue;
            for (u32 j = 0; j < l3_cnt; j++) {
                EVSet *evset = complex[n][i][j];
                if (!evset || !evset->config) continue;
                evset->config->test_config.hctrl = ctrl;
                evset->config->test_config_alt.hctrl = ctrl;
            }
        }
    }
}

static void *para_build_worker_main(void *arg) {
    para_build_worker *worker = arg;
    para_build_ctx *ctx = worker->ctx;
    EVBuildConfig sf_config = ctx->base_config;
    size_t l3_cnt;
    u32 main_core = ctx->cores[worker->pair_idx * 2];
    u32 helper_core = ctx->cores[worker->pair_idx * 2 + 1];

    reset_evset_stats();

    if (!set_proc_affinity(main_core)) {
        _warn("Failed to pin construction thread %u to core %u\n",
              worker->pair_idx, main_core);
    }

    if (start_helper_thread(&worker->hctrl)) {
        pthread_mutex_lock(&ctx->work_lock);
        ctx->failed = true;
        ctx->stop = true;
        pthread_mutex_unlock(&ctx->work_lock);
        worker->stats = _evset_stats;
        return NULL;
    }

    if (!set_thread_affinity(worker->hctrl.pid, helper_core)) {
        _warn("Failed to pin helper thread %u to core %u\n",
              worker->pair_idx, helper_core);
    }

    sf_config.test_config.hctrl = &worker->hctrl;
    sf_config.test_config_alt.hctrl = &worker->hctrl;

    _info("Pair %u: construction core %u; helper core %u\n",
          worker->pair_idx, main_core, helper_core);

    for (u32 c = 0; next_offset_work(ctx, &c);) {
        u32 n = ctx->idxs[c];
        u32 offset = n * CL_SIZE;

        for (u32 i = 0; i < num_l2sets; i++) {
            sf_config.test_config.lower_ev = ctx->l2evsets[n][i];
            EVSet **sf_evsets = build_evsets_at(
                offset, &sf_config, detected_l3, ctx->sf_cands[n][i],
                &l3_cnt, ctx->lower_cache, ctx->lower_conf,
                ctx->l2evsets[n], ctx->n_lower_evsets);
            ctx->sfevset_complex[n][i] = sf_evsets;
            if (!sf_evsets) {
                _error("No sf evsets are built!\n");
            }

            if (total_runtime_limit &&
                ((time_ns() - ctx->start_time) / 1e9 >=
                 total_runtime_limit * 60)) {
                _error("Timeout break!\n");
                request_parallel_stop(ctx);
                break;
            }
        }

        _info("Pair %u: offset %#x finished\n", worker->pair_idx, offset);
    }

    stop_helper_thread(&worker->hctrl);
    worker->stats = _evset_stats;
    return NULL;
}

static bool build_sf_evsets_parallel(EVSet ****sfevset_complex,
                                     EVSet ***l2evsets,
                                     EVCands ***sf_cands, u32 *idxs,
                                     u32 n_offset, EVBuildConfig *sf_config,
                                     cache_param *lower_cache,
                                     EVBuildConfig *lower_conf,
                                     size_t n_lower_evsets,
                                     u64 start_time) {
    u32 n_pairs = n_para / 2;

    if (n_pairs > n_offset) {
        _warn("Only %u page offsets selected; using %u construction pairs "
              "instead of %u\n",
              n_offset, n_offset, n_pairs);
        n_pairs = n_offset;
    }

    u32 n_pinned = n_pairs * 2;
    u32 *cores = _calloc(n_pinned, sizeof(*cores));
    if (!cores) {
        _error("Failed to allocate core list\n");
        return false;
    }

    if (pick_cores(cores, n_pinned) != n_pinned) {
        _error("Need %u non-SMT cores for parallel construction\n", n_pinned);
        _free(cores);
        return false;
    }

    para_build_ctx ctx = {
        .sfevset_complex = sfevset_complex,
        .l2evsets = l2evsets,
        .sf_cands = sf_cands,
        .cores = cores,
        .idxs = idxs,
        .n_offsets = n_offset,
        .next_offset = 0,
        .lower_cache = lower_cache,
        .lower_conf = lower_conf,
        .n_lower_evsets = n_lower_evsets,
        .base_config = *sf_config,
        .start_time = start_time,
        .stop = false,
        .failed = false,
    };
    pthread_mutex_init(&ctx.work_lock, NULL);

    pthread_t *threads = _calloc(n_pairs, sizeof(*threads));
    para_build_worker *workers = _calloc(n_pairs, sizeof(*workers));
    if (!threads || !workers) {
        _error("Failed to allocate parallel construction workers\n");
        _free(threads);
        _free(workers);
        _free(cores);
        pthread_mutex_destroy(&ctx.work_lock);
        return false;
    }

    _info("Starting %u parallel construction pairs\n", n_pairs);
    for (u32 i = 0; i < n_pairs; i++) {
        workers[i].ctx = &ctx;
        workers[i].pair_idx = i;
        if (pthread_create(&threads[i], NULL, para_build_worker_main,
                           &workers[i])) {
            _error("Failed to create construction thread %u\n", i);
            ctx.failed = true;
            request_parallel_stop(&ctx);
            n_pairs = i;
            break;
        }
    }

    struct evset_stats stats = {0};
    for (u32 i = 0; i < n_pairs; i++) {
        pthread_join(threads[i], NULL);
        merge_evset_stats(&stats, &workers[i].stats);
    }
    _evset_stats = stats;

    bool success = !ctx.failed;
    _free(threads);
    _free(workers);
    _free(cores);
    pthread_mutex_destroy(&ctx.work_lock);
    return success;
}

int build_sf_evset_all(u32 n_offset) {
    int ret = EXIT_FAILURE;
    bool helper_started = false;
    EVCands ***sf_cands = NULL;
    EVSet ****sfevset_complex = NULL;
    size_t l3_cnt = 0;

    EVSet ***l2evsets = build_l2_evsets_all();
    if (!l2evsets) {
        _error("Failed to build L2 evset complex\n");
        return EXIT_FAILURE;
    }

    u32 idxs[NUM_OFFSETS] = {0};
    for (u32 i = 0; i < NUM_OFFSETS; i++) {
        idxs[i] = i;
    }

    if (n_offset > 0) {
        shuffle_index(idxs, NUM_OFFSETS);
    }

    EVBuildConfig sf_config;
    default_skx_sf_evset_build_config(&sf_config, NULL, NULL, &hctrl);
    sf_config.algorithm = evalgo;
    sf_config.cands_config.scaling = cands_scaling;
    sf_config.algo_config.verify_retry = max_tries;
    sf_config.algo_config.max_backtrack = max_backtrack;
    sf_config.algo_config.retry_timeout = max_timeout;
    sf_config.algo_config.ret_partial = true;
    sf_config.algo_config.prelim_test = true;
    sf_config.algo_config.extra_cong = extra_cong;

    sf_cands = build_evcands_all(&sf_config, l2evsets);
    if (!sf_cands) {
        _error("Failed to allocate or filter SF candidates\n");
        goto cleanup;
    }

    reset_evset_stats();

    if (single_thread) {
        sf_config.test_config.traverse = skx_sf_cands_traverse_st;
        sf_config.test_config.need_helper = false;
    }

    n_offset = _min(n_offset, NUM_OFFSETS);
    if (n_offset == 0) {
        n_offset = NUM_OFFSETS;
    }

    l3_cnt = cache_uncertainty(detected_l3);
    if (sf_config.cands_config.filter_ev) {
        l3_cnt /= cache_uncertainty(sf_config.cands_config.filter_ev->target_cache);
    }

    sfevset_complex = calloc(NUM_OFFSETS, sizeof(*sfevset_complex));
    if (!sfevset_complex) {
        _error("Failed to allocate SF complex\n");
        goto cleanup;
    }

    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        sfevset_complex[n] =
            calloc(num_l2sets, sizeof(**sfevset_complex));
        if (!sfevset_complex[n]) {
            _error("Failed to allocate SF sub-complex\n");
            goto cleanup;
        }
    }

    _info("About to start evset construction\n");

    cache_param *lower_cache = NULL;
    EVBuildConfig *lower_conf = NULL;
    size_t n_lower_evsets = 0;
    if (!l2_filter) {
        lower_cache = detected_l2;
        lower_conf = &def_l2_ev_config;
        n_lower_evsets = cache_uncertainty(detected_l2);
    }

    if (!single_thread && !n_para) {
        if (start_helper_thread(sf_config.test_config.hctrl)) {
            goto cleanup;
        }
        helper_started = true;
    }

    u64 start = time_ns(), end;
    if (n_para) {
        if (!build_sf_evsets_parallel(sfevset_complex, l2evsets, sf_cands,
                                      idxs, n_offset, &sf_config, lower_cache,
                                      lower_conf, n_lower_evsets, start)) {
            goto cleanup;
        }
    } else {
        for (u32 c = 0; c < n_offset; c++) {
            u32 n = idxs[c];
            u32 offset = n * CL_SIZE;
            for (u32 i = 0; i < num_l2sets; i++) {
                sf_config.test_config.lower_ev = l2evsets[n][i];
                EVSet **sf_evsets = build_evsets_at(
                    offset, &sf_config, detected_l3, sf_cands[n][i], &l3_cnt,
                    lower_cache, lower_conf, l2evsets[n], n_lower_evsets);
                sfevset_complex[n][i] = sf_evsets;
                if (!sf_evsets) {
                    _error("No sf evsets are built!\n");
                }

                if (total_runtime_limit &&
                    ((time_ns() - start) / 1e9 >= total_runtime_limit * 60)) {
                    _error("Timeout break!\n");
                    goto timeout_break;
                }
            }
            _info("Offset %#x finished\n", offset);
        }
    }

timeout_break:
    end = time_ns();
    _info("Finished evset construction\n");
    _info("L3 Duration: %.3fms\n", (end - start) / 1e6);
    pprint_evset_stats();

    if (n_para) {
        if (start_helper_thread(&hctrl)) {
            goto cleanup;
        }
        helper_started = true;
        attach_helper_to_sfevsets(sfevset_complex, idxs, n_offset, l3_cnt,
                                  &hctrl);
    }

    size_t total_succ = 0, total_sf_succ = 0;
    for (u32 c = 0; c < n_offset; c++) {
        u32 n = idxs[c];
        size_t offset_succ = 0, offset_sf_succ = 0;
        for (u32 i = 0; i < num_l2sets; i++) {
            if (!sfevset_complex[n][i]) {
                continue;
            }

            for (u32 j = 0; j < l3_cnt; j++) {
                EVSet *sf_evset = sfevset_complex[n][i][j];
                if (!sf_evset || !sf_evset->addrs) {
                    continue;
                }

                EVTestRes llc_test = evset_self_precise_test(sf_evset);
                bool succ = llc_test == EV_POS;
                offset_succ += succ;
                total_succ += succ;

                sf_evset->config->test_config_alt.foreign_evictor = true;

                if (sf_evset->size > SF_ASSOC + 1) {
                    sf_evset->size = SF_ASSOC + 1;
                }

                EVTestRes sf_test = evset_self_precise_test_alt(sf_evset);
                bool sf_succ = sf_test == EV_POS;
                offset_sf_succ += sf_succ;
                total_sf_succ += sf_succ;
            }
        }

        _info("Offset %#5lx: %lu/%lu/%lu (LLC/SF/Expecting)\n", n * CL_SIZE,
              offset_succ, offset_sf_succ, cache_uncertainty(detected_l3));
    }

    _info("Aggregated: %lu/%lu/%lu (LLC/SF/Expecting)\n",
          total_succ, total_sf_succ, cache_uncertainty(detected_l3) * n_offset);

    ret = EXIT_SUCCESS;

cleanup:
    if (helper_started) {
        stop_helper_thread(sf_config.test_config.hctrl);
    }
    free_sfevset_complex(sfevset_complex, l3_cnt);
    free_evcands_complex(sf_cands);
    free_l2evset_complex(l2evsets);

    return ret;
}

void handler(int sig, siginfo_t *si, void *unused) {
    void *array[20];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 20);

    // print out all the frames to stderr
    fprintf(stderr, "Error: signal %d:\n", sig);
    fprintf(stderr, "Segfault at address: %p\n", si->si_addr);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    exit(1);
}


int main(int argc, char **argv) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(struct sigaction));
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = handler;
    sa.sa_flags   = SA_SIGINFO;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    u32 n_offset = 0;

    int opt, opt_idx;
    static struct option long_opts[] = {
        {"no-filter", no_argument, NULL, 'f'},
        {"single-thread", no_argument, NULL, 's'},
        {"cands-scale", required_argument, NULL, 'C'},
        {"max-backtrack", required_argument, NULL, 'B'},
        {"max-tries", required_argument, NULL, 'R'},
        {"timeout", required_argument, NULL, 'T'},
        {"algorithm", required_argument, NULL, 'A'},
        {"total-run-time-limit", required_argument, NULL, 'L'}, // in minutes
        {"parallel-construction", required_argument, NULL, 'P'},
        {0, 0, 0, 0}
    };

    char *algo_name = "default";
    optind = 1;
    while ((opt = getopt_long(argc, argv, "fsC:B:R:T:A:L:P:", long_opts,
                              &opt_idx)) != -1) {
        switch (opt) {
            case 'f': l2_filter = false; break;
            case 's': single_thread = true; break;
            case 'C': cands_scaling = strtod(optarg, NULL); break;
            case 'B': max_backtrack = strtoull(optarg, NULL, 10); break;
            case 'R': max_tries = strtoull(optarg, NULL, 10); break;
            case 'T': max_timeout = strtoull(optarg, NULL, 10); break;
            case 'A': algo_name = optarg; break;
            case 'L': total_runtime_limit = strtoull(optarg, NULL, 10); break;
            case 'P': {
                char *endptr = NULL;
                size_t n = strtoull(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0') {
                    _error("Invalid parallel construction core count: %s\n",
                           optarg);
                    return EXIT_FAILURE;
                }
                if (n == 0) {
                    n = pick_cores(NULL, 0);
                    if (n % 2) n--;
                }
                if (!set_n_para(n)) {
                    return EXIT_FAILURE;
                }
                break;
            }
            default: _error("Unknown option %c\n", opt); return EXIT_FAILURE;
        }
    }

    if (optind < argc) {
        char *endptr = NULL;
        unsigned long parsed = strtoul(argv[optind], &endptr, 10);
        if (endptr == argv[optind] || *endptr != '\0') {
            _error("Invalid number of offsets: %s\n", argv[optind]);
            return EXIT_FAILURE;
        }
        n_offset = (u32)parsed;
        optind++;
    }

    if (optind < argc) {
        _error("Unexpected argument: %s\n", argv[optind]);
        return EXIT_FAILURE;
    }

    if (n_para && single_thread) {
        _error("Parallel construction requires helper threads and cannot be "
               "combined with --single-thread\n");
        return EXIT_FAILURE;
    }

    evalgo = parse_evset_algo(algo_name);
    if (evalgo == EVSET_ALGO_INVALID) {
        _error("Invalid evset construction algorithm: %s\n", algo_name);
        return EXIT_FAILURE;
    }

    _info("Algorithm: %s\n", algo_name);

    if (cache_env_init(1)) {
        _error("Failed to initialize cache env!\n");
        return EXIT_FAILURE;
    }
    num_l2sets = cache_uncertainty(detected_l2);
    if (!l2_filter) num_l2sets = 1;

    extra_cong = SF_ASSOC - detected_l3->n_ways;
    cache_oracle_init();
    int ret = build_sf_evset_all(n_offset);
    cache_oracle_cleanup();
    return ret;
}
