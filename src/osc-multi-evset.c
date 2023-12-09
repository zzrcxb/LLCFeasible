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
static size_t num_l2sets;
static helper_thread_ctrl hctrl;

#define NUM_OFFSETS (PAGE_SIZE / CL_SIZE)

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
    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        u32 offset = n * CL_SIZE;
        cands_complex[n] = calloc(num_l2sets, sizeof(EVCands *));
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
                    return NULL;
                }

                if (evcands_populate(offset, cands, &conf->cands_config)) {
                    return NULL;
                }
                cands_complex[n][i] = cands;
            } else {
                cands_complex[n][i] =
                    evcands_shift(cands_complex[0][i], offset);
            }
        }
    }
    end = time_ns();
    _info("EVCands Complex Populate: %luus;\n", (end - start) / 1000);
    return cands_complex;
}

static void shuffle_index(u32 *idxs, u32 sz) {
    srand(time(NULL));
    for (u32 tail = sz - 1; tail > 0; tail--) {
        u32 n_choice = tail + 1;
        u32 choice = rand() % n_choice;
        _swap(idxs[choice], idxs[tail]);
    }
}

int build_sf_evset_all(u32 n_offset) {
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

    EVCands ***sf_cands = build_evcands_all(&sf_config, l2evsets);
    if (!sf_cands) {
        _error("Failed to allocate or filter SF candidates\n");
        return EXIT_FAILURE;
    }

    reset_evset_stats();

    if (single_thread) {
        sf_config.test_config.traverse = skx_sf_cands_traverse_st;
        sf_config.test_config.need_helper = false;
    } else {
        start_helper_thread(sf_config.test_config.hctrl);
    }

    n_offset = _min(n_offset, NUM_OFFSETS);
    if (n_offset == 0) {
        n_offset = NUM_OFFSETS;
    }

    EVSet ****sfevset_complex = calloc(NUM_OFFSETS, sizeof(*sfevset_complex));
    if (!sfevset_complex) {
        _error("Failed to allocate SF complex\n");
        return EXIT_FAILURE;
    }

    for (u32 n = 0; n < NUM_OFFSETS; n++) {
        sfevset_complex[n] =
            calloc(num_l2sets, sizeof(**sfevset_complex));
        if (!sfevset_complex[n]) {
            _error("Failed to allocate SF sub-complex\n");
            return EXIT_FAILURE;
        }
    }

    _info("About to start evset construction\n");
    size_t l3_cnt;

    cache_param *lower_cache = NULL;
    EVBuildConfig *lower_conf = NULL;
    size_t n_lower_evsets = 0;
    if (!l2_filter) {
        lower_cache = detected_l2;
        lower_conf = &def_l2_ev_config;
        n_lower_evsets = cache_uncertainty(detected_l2);
    }

    u64 start = time_ns(), end;
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

timeout_break:
    end = time_ns();
    _info("Finished evset construction\n");
    _info("L3 Duration: %.3fms\n", (end - start) / 1e6);
    pprint_evset_stats();

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

    if (!single_thread) {
        stop_helper_thread(sf_config.test_config.hctrl);
    }

    return EXIT_SUCCESS;
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
    if (argc < 2) {
        _error("./osc-multi-evset <num_offsets>\n");
        return EXIT_FAILURE;
    }
    struct sigaction sa;

    memset(&sa, 0, sizeof(struct sigaction));
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = handler;
    sa.sa_flags   = SA_SIGINFO;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    u32 n_offset = strtoul(argv[1], NULL, 10);

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
        {0, 0, 0, 0}
    };

    char *algo_name = "default";
    while ((opt = getopt_long(argc, argv, "fsC:B:R:T:A:L:", long_opts,
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
            default: _error("Unknown option %c\n", opt); return EXIT_FAILURE;
        }
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
