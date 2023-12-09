#include "core.h"
#include "cache/cache.h"
#include "sync.h"
#include <math.h>
#include <getopt.h>
#include "osc-common.h"

size_t max_num_recs = 1001, retry = 5;
static helper_thread_ctrl hctrl;

static inline void llc_evset_prime(EVSet *evset, u64 threshold) {
    generic_evset_traverse(evset->config->test_config.lower_ev);
    _lfence();
    for (u32 i = 0; i < 10; i++) {
        skx_sf_cands_traverse_mt(evset->addrs, evset->size,
                                 &evset->config->test_config);
        u64 begin = _timer_start();
        access_array(evset->addrs, evset->size);
        u64 end = _timer_end();
        if (end - begin < threshold) {
            break;
        }
    }
}

i64 calibrate_grp_access_lat(u8 *target, EVSet *evset, EVTestConfig *tconfig) {
    u64 n_repeat = 1000;
    i64 *no_acc_lats = calloc(n_repeat, sizeof(no_acc_lats[0]));
    i64 *acc_lats = calloc(n_repeat, sizeof(acc_lats[0]));
    if (!no_acc_lats || !acc_lats) {
        _error("Failed to calibrate grp access latency\n");
        return 0;
    }

    for (u64 r = 0; r < n_repeat;) {
        u32 aux_before, aux_after;
        _rdtscp_aux(&aux_before);

        flush_array(evset->addrs, evset->size);
        _lfence();
        llc_evset_prime(evset, 0);
        _lfence();

        u64 begin = _timer_start();
        access_array(evset->addrs, evset->size);
        u64 end = _timer_end_aux(&aux_after);

        if (aux_before == aux_after) {
            no_acc_lats[r] = end - begin;
            r++;
        }
    }

    for (u64 r = 0; r < n_repeat;) {
        u32 aux_before, aux_after;
        _rdtscp_aux(&aux_before);

        _clflush(target);
        flush_array(evset->addrs, evset->size);
        _lfence();
        llc_evset_prime(evset, 0);
        _lfence();
        _maccess(target);
        helper_thread_read_single(target, tconfig->hctrl);

        u64 begin = _timer_start();
        access_array(evset->addrs, evset->size);
        u64 end = _timer_end_aux(&aux_after);

        if (aux_before == aux_after) {
            acc_lats[r] = end - begin;
            r++;
        }
    }

    i64 no_acc_lat = find_median_lats(no_acc_lats, n_repeat);
    i64 acc_lat = find_median_lats(acc_lats, n_repeat);
    i64 threshold = (no_acc_lat + acc_lat) / 2;
    u32 otc = 0, utc = 0;
    for (u32 i = 0; i < n_repeat; i++) {
        otc += no_acc_lats[i] > threshold;
        utc += acc_lats[i] < threshold;
    }

    _info("no access: %ld; access: %ld; threshold: %ld; otc: %u; utc: %u\n",
          no_acc_lat, acc_lat, threshold, otc, utc);

    if (otc > n_repeat * 0.05 || utc > n_repeat * 0.05) {
        threshold = 0; // bad threshold
    }

    free(no_acc_lats);
    free(acc_lats);
    return threshold;
}

int monitor_l3_rate() {
    int ret = EXIT_SUCCESS;

    // prepare buffer
    u64 *timestamps = _calloc(max_num_recs, sizeof(*timestamps)), sz = 0;
    u64 *iters = _calloc(max_num_recs, sizeof(*timestamps));
    u8 *page = mmap_shared_init(NULL, PAGE_SIZE, 'a');
    if (!page || !timestamps || !iters) {
        _error("Failed to allocate buffer\n");
        return EXIT_FAILURE;
    }

    srand(time_ns());
    u32 offset = (rand() % (PAGE_SIZE / CL_SIZE)) * CL_SIZE;
    u8 *target = page + offset;

    // construct evsets
    EVSet *l2_evset = build_l2_EVSet(target, &def_l2_ev_config, NULL);
    if (!l2_evset) {
        _error("Failed to build an L2 evset\n");
        ret = EXIT_FAILURE;
        goto err;
    }

    EVBuildConfig sf_config;
    default_skx_sf_evset_build_config(&sf_config, NULL, l2_evset, &hctrl);
    sf_config.algo_config.verify_retry = 10;
    sf_config.algo_config.retry_timeout = 1000;

    start_helper_thread(sf_config.test_config.hctrl);

    EVSet *llc_ev = NULL;
    i64 threshold = 0;
    for (u32 i = 0; i <= retry; i++) {
        llc_ev = build_skx_sf_EVSet(target, &sf_config, NULL);
        if (llc_ev && llc_ev->size >= detected_l3->n_ways) {
            llc_ev->size = detected_l3->n_ways;
        }
        _info("try %u: sz: %u\n", i, llc_ev ? llc_ev->size : 0);

        if (llc_ev && llc_ev->size == detected_l3->n_ways &&
            generic_evset_test(target, llc_ev) == EV_POS) {
            threshold = calibrate_grp_access_lat(target, llc_ev,
                                                 &sf_config.test_config);
            if (threshold < detected_cache_lats.l3) {
                _error("Bad threshold! %ld\n", threshold);
            } else {
                break;
            }
        }

        _warn("Retry %u\n", i + 1);
        llc_ev = NULL;
        threshold = 0;
    }

    if (!llc_ev || threshold == 0) {
        _error("Failed to build an LLC evset\n");
        return EXIT_FAILURE;
    }

    u64 begin = _timer_start();
    for (size_t i = 0; i < 100; i++) {
        _timer_start();
        access_array(llc_ev->addrs, llc_ev->size);
        _timer_end();
    }
    u64 end = _timer_end();
    _info("Temporal resolution: %lu\n", (end - begin) / 100);

    if (!llc_ev || !threshold) {
        _error("Failed to build an LLC evset or calibrate the threshold\n");
        ret = EXIT_FAILURE;
        goto err;
    }
    _info("EV Size: %u; EV Level: %d\n", llc_ev->size,
          generic_evset_test(target, llc_ev));

    // to monitor
    u32 aux, last_aux;
    u64 iter = 0;
    _rdtscp_aux(&last_aux);
    llc_evset_prime(llc_ev, threshold);
    while (sz < max_num_recs) {
        u64 begin = _timer_start();
        access_array(llc_ev->addrs, llc_ev->size);
        u64 end = _timer_end_aux(&aux);
        bool ctx_switch = aux != last_aux;
        if ((end - begin) > threshold || ctx_switch) {
            if (!ctx_switch) {
                iters[sz] = iter;
                timestamps[sz++] = end;
            }
            llc_evset_prime(llc_ev, threshold);
            last_aux = aux;
        }
        iter += 1;
    }

    for (size_t i = 1; i < sz; i++) {
        printf("%lu %lu\n", timestamps[i] - timestamps[i - 1],
               iters[i] - iters[i - 1]);
    }

err:
    free(timestamps);
    free(iters);
    if (l2_evset) {
        stop_helper_thread(sf_config.test_config.hctrl);
    }
    munmap(page, PAGE_SIZE);
    return ret;
}

int main(int argc, char **argv) {
    int opt, opt_idx;
    static struct option long_opts[] = {
        {"num-recs", required_argument, NULL, 'n'},
        {"retry", required_argument, NULL, 'r'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "n:r:", long_opts, &opt_idx)) != -1) {
        switch (opt) {
            case 'n': max_num_recs = strtoull(optarg, NULL, 10) + 1; break;
            case 'r': retry = strtoull(optarg, NULL, 10); break;
            default: _error("Unknown option %c\n", opt); return EXIT_FAILURE;
        }
    }

    if (cache_env_init(1)) {
        _error("Failed to initialize cache env!\n");
        return EXIT_FAILURE;
    }

    int ret = monitor_l3_rate();

    return ret;
}
