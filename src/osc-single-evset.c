#include "core.h"
#include "cache/cache.h"
#include "sync.h"
#include <math.h>
#include <getopt.h>
#include "osc-common.h"

static evset_algorithm evalgo = EVSET_ALGO_DEFAULT;
static double cands_scaling = 3;
static size_t extra_cong = 1;
static size_t max_tries = 10, max_backtrack = 20, max_timeout = 0;
static bool l2_filter = true, single_thread = false, has_hugepage = false;
static helper_thread_ctrl hctrl;

u8 *page, *target;

int test_page_continuity(u8 *hugepage) {
    u8 **addrs = _calloc(detected_l2->n_ways, sizeof(u8 *));
    if (!addrs) return -1;

    u32 stride = cache_congruent_stride(detected_l2);
    u32 nlines = HUGE_PAGE_SIZE / stride;

    int cnt = 0;
    for (u32 s = 1; s <= 10 && s + detected_l2->n_ways < nlines; s++) {
        for (u32 i = 0; i < detected_l2->n_ways; i++) {
            addrs[i] = hugepage + (s + i) * stride;
        }
        cnt += generic_test_eviction(hugepage, addrs, detected_l2->n_ways,
                                     &def_l2_ev_config.test_config) == EV_POS;
    }
    return cnt;
}

int single_l2_evset() {
    def_l2_ev_config.algorithm = evalgo;
    def_l2_ev_config.cands_config.scaling = cands_scaling;
    def_l2_ev_config.algo_config.verify_retry = max_tries;
    def_l2_ev_config.algo_config.max_backtrack = max_backtrack;
    def_l2_ev_config.algo_config.retry_timeout = max_timeout;
    def_l2_ev_config.algo_config.ret_partial = true;

    if (has_hugepage) {
        cache_use_hugepage();
    }

    u64 start = time_ns();
    EVSet *evset = build_l2_EVSet(target, &def_l2_ev_config, NULL);
    u64 end = time_ns();

    if (!evset) {
        _error("Failed to build evset\n");
        return EXIT_FAILURE;
    }
    pprint_evset_stats();
    _info("Duration: %.3fms; Size: %u; Candidates: %lu\n", (end - start) / 1e6,
          evset->size, evset->cands->size);

    if (cache_oracle_inited()) {
        u32 target_set = cache_set_idx(target, detected_l2), match = 0;
        printf("Target: %p; set=%#x\n", target, target_set);
        for (u32 i = 0; i < evset->size; i++) {
            u32 set = cache_set_idx(evset->addrs[i], detected_l2);
            printf("%2u: %p (set=%#x)\n", i, evset->addrs[i], set);
            match += set == target_set;
        }
        printf("Match: %u\n", match);
    }
    _info("L2 EV Test Level: %d\n", generic_evset_test(target, evset));
    return EXIT_SUCCESS;
}

int single_llc_evset() {
    EVSet *l2_evset = NULL;
    u64 filter_duration = 0, start = 0, end = 0;

    start = time_ns();
    for (u32 r = 0; r < 10; r++) {
        l2_evset = build_l2_EVSet(target, &def_l2_ev_config, NULL);
        if (l2_evset && l2_evset->size == detected_l2->n_ways &&
            generic_evset_test(target, l2_evset) == EV_POS) {
            break;
        }
    }
    end = time_ns();
    filter_duration += end - start;

    if (!l2_evset) {
        _error("Failed to build an L2 evset\n");
        return EXIT_FAILURE;
    }

    if (has_hugepage) {
        cache_use_hugepage();
    }

    EVBuildConfig sf_config;
    default_skx_sf_evset_build_config(&sf_config, NULL, l2_evset, &hctrl);
    sf_config.algorithm = evalgo;
    sf_config.cands_config.scaling = cands_scaling;
    sf_config.algo_config.verify_retry = max_tries;
    sf_config.algo_config.max_backtrack = max_backtrack;
    sf_config.algo_config.retry_timeout = max_timeout;
    sf_config.algo_config.ret_partial = true;
    sf_config.algo_config.extra_cong = extra_cong;

    if (!l2_filter || has_hugepage) {
        sf_config.cands_config.filter_ev = NULL;
    }

    EVCands *cands = evcands_new(detected_l3, &sf_config.cands_config, NULL);
    if (!cands) {
        _error("Failed to allocate evcands\n");
        return EXIT_FAILURE;
    }

    start = time_ns();
    if (evcands_populate(page_offset(target), cands, &sf_config.cands_config)) {
        _error("Failed to populate evcands\n");
        return EXIT_FAILURE;
    }
    end = time_ns();
    filter_duration += end - start;
    if (l2_filter) {
        _info("L2 Filter Duration: %luus\n", filter_duration / 1000);
    }

    if (single_thread) {
        sf_config.test_config.traverse = skx_sf_cands_traverse_st;
        sf_config.test_config.need_helper = false;
    } else {
        start_helper_thread(sf_config.test_config.hctrl);
    }

    if (generic_test_eviction(target, cands->cands, cands->size,
                              &sf_config.test_config) != EV_POS) {
        _error("Not enough candidates due to filtering!\n");
        return EXIT_FAILURE;
    }

    reset_evset_stats();
    start = time_ns();
    EVSet *sf_evset = build_skx_sf_EVSet(target, &sf_config, cands);
    end = time_ns();
    if (!sf_evset) {
        _error("Failed to build evset\n");
        pprint_evset_stats();
        return EXIT_FAILURE;
    }
    pprint_evset_stats();
    _info("Duration: %.3fms; Size: %u; Candidates: %lu\n", (end - start) / 1e6,
          sf_evset->size, sf_evset->cands->size);
    _info("LLC EV Test Level: %d\n", precise_evset_test(target, sf_evset));

    if (sf_evset->size > SF_ASSOC) {
        // truncate the evset to a minimal evset for SF test
        sf_evset->size = SF_ASSOC;
    }
    sf_config.test_config_alt.foreign_evictor = true;
    _info("SF EV Test Level: %d\n", precise_evset_test_alt(target, sf_evset));

    if (!single_thread) {
        stop_helper_thread(sf_config.test_config.hctrl);
    }

    if (cache_oracle_inited()) {
        u64 target_hash = llc_addr_hash(target), match = 0;
        printf("Target: %p; hash=%#lx\n", target, target_hash);
        for (u32 i = 0; i < sf_evset->size; i++) {
            u64 hash = llc_addr_hash(sf_evset->addrs[i]);
            printf("%2u: %p (hash=%#lx)\n", i, sf_evset->addrs[i], hash);
            match += hash == target_hash;
        }
        printf("Match: %lu\n", match);
    }

    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        _error("./osc-single-evset <action>");
        return EXIT_FAILURE;
    }

    const char *action = argv[1];

    int opt, opt_idx;
    static struct option long_opts[] = {
        {"no-filter", no_argument, NULL, 'f'},
        {"single-thread", no_argument, NULL, 's'},
        {"hugepage", no_argument, NULL, 'H'},
        {"cands-scale", required_argument, NULL, 'C'},
        {"max-backtrack", required_argument, NULL, 'B'},
        {"max-tries", required_argument, NULL, 'R'},
        {"timeout", required_argument, NULL, 'T'},
        {"algorithm", required_argument, NULL, 'A'},
        {0, 0, 0, 0}
    };

    char *algo_name = "default";
    while ((opt = getopt_long(argc, argv, "fsHC:B:R:T:A:", long_opts,
                              &opt_idx)) != -1) {
        switch (opt) {
            case 'f': l2_filter = false; break;
            case 's': single_thread = true; break;
            case 'H': has_hugepage = true; break;
            case 'C': cands_scaling = strtod(optarg, NULL); break;
            case 'B': max_backtrack = strtoull(optarg, NULL, 10); break;
            case 'R': max_tries = strtoull(optarg, NULL, 10); break;
            case 'T': max_timeout = strtoull(optarg, NULL, 10); break;
            case 'A': algo_name = optarg; break;
            default: _error("Unknown option %c\n", opt); return EXIT_FAILURE;
        }
    }
    evalgo = parse_evset_algo(algo_name);
    if (evalgo == EVSET_ALGO_INVALID) {
        _error("Invalid evset construction algorithm %s\n", algo_name);
        return EXIT_FAILURE;
    }
    _info("Algorithm: %s\n", algo_name);

    if (cache_env_init(1)) {
        _error("Failed to initialize cache env!\n");
        return EXIT_FAILURE;
    }
    cache_oracle_init();

    if (has_hugepage) {
        page = mmap_huge_shared_init(NULL, PAGE_SIZE, 'a');
        _info("Test target huge page: %d\n", test_page_continuity(page));
    } else {
        page = mmap_shared_init(NULL, PAGE_SIZE, 'a');
    }
    if (!page) {
        _error("Failed to allocate the target page\n");
        return EXIT_FAILURE;
    }

    srand(time_ns());
    u32 offset = rand() % (PAGE_SIZE / CL_SIZE) * CL_SIZE;
    target = page + offset;

    int ret = EXIT_SUCCESS;

    if (strcmp(action, "L2") == 0) {
        ret = single_l2_evset();
    } else if (strcmp(action, "LLC") == 0) {
        extra_cong = 0;
        ret = single_llc_evset();
    } else if (strcmp(action, "SF") == 0) {
        extra_cong = SF_ASSOC - detected_l3->n_ways;
        ret = single_llc_evset();
    } else {
        _error("Unknown action!\n");
        ret = EXIT_FAILURE;
    }

    munmap(page, PAGE_SIZE);
    cache_oracle_cleanup();
    return ret;
}
