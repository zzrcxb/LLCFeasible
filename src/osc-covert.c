#include "osc-common.h"
#include "core.h"
#include "sync.h"
#include "cache/cache.h"
#include <getopt.h>

static u64 n_emits = 100, recv_scale = 20, emit_interval = 100000;
static bool secret_access = false, use_prime_scope = false, use_sense = false,
            monitor_only = false, ptr_chase = false;
static double secret_timing_scale = 1.0, bad_threshold_ratio = 0.08;
static u32 max_retry = 10, l2_repeat = 1, array_repeat = 12;

static u8 *target = NULL;
static helper_thread_ctrl hctrl;
static i64 para_threshold = 0, ptr_threshold = 0, spurious_cnt = 0;
static EVSet *helper_sf_evset = NULL;
static evchain *sf_chain1 = NULL, *sf_chain2 = NULL;

static bool check_and_set_sf_evset(EVSet *evset) {
    if (!evset || evset->size < SF_ASSOC) {
        _error("Failed to build sf evset\n");
        return false;
    }
    evset->size = SF_ASSOC;

    EVTestRes tres = precise_evset_test_alt(target, evset);
    if (tres != EV_POS) {
        _error("Cannot evict target\n");
        return false;
    }

    tres = generic_test_eviction(target, evset->addrs, SF_ASSOC,
                                 &evset->config->test_config_alt);
    if (tres != EV_POS) {
        _error("Cannot evict target with SF_ASSOC\n");
        return false;
    }
    return true;
}

int measure_performance(EVSet *evset) {
    u32 n_repeat = 1000, aux;
    i64 para_lat = 0, ps_lat = 0, ptr_lat = 0;
    u64 end_tsc, start, end;

    prime_skx_sf_evset_para(evset, array_repeat, l2_repeat);
    start = _timer_start();
    for (u32 i = 0; i < n_repeat * 10; i++) {
        probe_skx_sf_evset_para(evset, &end_tsc, &aux);
    }
    end = _timer_end();
    para_lat = (end - start) / n_repeat / 10;

    u8 *ptr = evset->addrs[0];
    _time_maccess(ptr);
    start = _timer_start();
    for (u32 i = 0; i < n_repeat * 10; i++) {
        _time_maccess(ptr);
    }
    end = _timer_end();
    ps_lat = (end - start) / n_repeat / 10;

    start = _timer_start();
    for (u32 i = 0; i < n_repeat * 10; i++) {
        probe_skx_sf_evset_ptr_chase(evset, &end_tsc, &aux);
    }
    end = _timer_end();
    ptr_lat = (end - start) / n_repeat / 10;

    _info("Para. Resolution: %lu cycles; Ptr-Chase Resolution: %lu cycles; PS "
          "Resolution: %lu cycles\n\n",
          para_lat, ptr_lat, ps_lat);
    return false;
}

EVSet *prepare_evsets() {
    EVSet *l2_evset = NULL;
    for (u32 i = 0; i < max_retry; i++) {
        l2_evset = build_l2_EVSet(target, &def_l2_ev_config, NULL);
        if (!l2_evset || generic_evset_test(target, l2_evset) != EV_POS) {
            l2_evset = NULL;
        } else {
            break;
        }
    }
    if (!l2_evset) {
        _error("Failed to build an L2 evset\n");
        return NULL;
    }

    EVBuildConfig sf_config;
    default_skx_sf_evset_build_config(&sf_config, NULL, l2_evset, &hctrl);
    sf_config.algo_config.extra_cong = SF_ASSOC - detected_l3->n_ways;

    EVSet *sf_evset = build_skx_sf_EVSet(target, &sf_config, NULL);
    helper_sf_evset = build_skx_sf_EVSet(target, &sf_config, NULL);

    if (!check_and_set_sf_evset(sf_evset)) {
        _error("Failed to build the main SF evset\n");
        return NULL;
    }

    if (!check_and_set_sf_evset(helper_sf_evset)) {
        _error("Failed to build the helper SF evset\n");
        return NULL;
    }
    sf_chain1 = evchain_build(sf_evset->addrs, SF_ASSOC);
    sf_chain2 = evchain_build(helper_sf_evset->addrs, SF_ASSOC);

    para_threshold = calibrate_para_probe_lat(target, sf_evset, array_repeat,
                                              l2_repeat, bad_threshold_ratio);
    if (para_threshold <= 0) {
        _error("Failed to calibrate grp access lat!\n");
        return NULL;
    }

    if (ptr_chase) {
        ptr_threshold = calibrate_chase_probe_lat(target, sf_evset, array_repeat,
                                                l2_repeat, .2);
        if (ptr_threshold <= 0) {
            _error("Failed to calibrate ptr access lat!\n");
            return NULL;
        }
    }

    if (measure_performance(sf_evset)) {
        _error("Failed to measure prime+probe performance!\n");
        return NULL;
    }

    return sf_evset;
}

struct covert_emit_rec {
    u64 tsc;
    u32 aux, lat;
};

struct psender_ctrl {
    pthread_barrier_t *barrier;
    struct covert_emit_rec *emit_recs;
    pthread_t pid;
    volatile bool finished;
};

void *covert_send(void *arg) {
    struct psender_ctrl *pctrl = arg;
    pthread_barrier_wait(pctrl->barrier);

    u64 long_interval = emit_interval * secret_timing_scale;

    _clflush(target);
    _lfence();
    _maccess(target);
    srand(time(NULL));
    u32 aux;
    u64 tsc = _rdtsc();

    while (_rdtsc() - tsc < 100000);
    _lfence();
    for (u32 i = 0; i < n_emits; i++) {
        u64 interval = emit_interval;
        if (rand() % 2) {
            interval = long_interval;
        }

        while (_rdtsc() - tsc < interval);
        tsc = _rdtscp_aux(&aux);

        if (!secret_access || rand() % 2) {
            u64 after;
            u64 lat = _time_maccess_aux(target, after, aux);
            pctrl->emit_recs[i] = (struct covert_emit_rec){tsc, aux, lat};
        } else {
            pctrl->emit_recs[i] = (struct covert_emit_rec){tsc, aux, 0};
        }
    }
    pctrl->finished = true;
    return NULL;
}

static struct psender_ctrl sender_ctrl;
static pthread_barrier_t barrier;
static pthread_barrierattr_t battr;
static u64 switched_thresh = 20000; // cycles

static size_t monitor_para(EVSet *sf_evset, cache_acc_rec *recv_recs,
                           sender_switched_out *switch_recs, size_t max_recv) {
    u64 n_recvs = 0, iters = 0, end, n_switches = 0;
    u32 aux, last_aux;
    i64 threshold = ptr_chase ? ptr_threshold : para_threshold;

    _rdtscp_aux(&last_aux);
    flush_evset(sf_evset);
    _lfence();
    prime_skx_sf_evset_para(sf_evset, array_repeat, l2_repeat);
    u64 last_tsc = _rdtsc();
    while (n_recvs < max_recv) {
        u64 now_tsc = _rdtsc();
        bool switched_out = now_tsc - last_tsc > switched_thresh;

        u64 lat = 0;
        if (ptr_chase) {
            lat = probe_skx_sf_evset_ptr_chase(sf_evset, &end, &aux);
        } else {
            lat = probe_skx_sf_evset_para(sf_evset, &end, &aux);
        }
        bool spurious =
            (aux != last_aux) || lat > detected_cache_lats.interrupt_thresh;

        if (spurious || lat > threshold) {
            prime_skx_sf_evset_para(sf_evset, array_repeat, l2_repeat);
            if (!spurious) {
                _mfence();
                _lfence();
                u32 blindspot = _rdtscp_aux(&aux) - end;
                recv_recs[n_recvs++] = (cache_acc_rec){.tsc = end,
                                                       .iters = iters,
                                                       .aux = aux,
                                                       .lat = lat,
                                                       .blindspot = blindspot};
            }
            last_aux = aux;
            spurious_cnt += spurious;
        }

        if (switched_out && n_switches < max_recv) {
            switch_recs[n_switches++] =
                (sender_switched_out){.start = last_tsc, .end = now_tsc};
        }
        last_tsc = now_tsc;

        iters += 1;
        if (!monitor_only && iters % 128 == 0 && sender_ctrl.finished) {
            break;
        }
    }

    return n_recvs;
}

size_t monitor_ps(EVSet *sf_evset, cache_acc_rec *recv_recs,
                  sender_switched_out *switch_recs, size_t max_recv) {
    u64 n_recvs = 0, iters = 0, n_switches = 0;
    u32 aux, last_aux;
    bool prime_sense = false;

    _rdtscp_aux(&last_aux);

    sf_chain1 = evchain_build(sf_evset->addrs, SF_ASSOC);
    sf_chain2 = evchain_build(helper_sf_evset->addrs, SF_ASSOC);
    i64 threshold = detected_cache_lats.l2_thresh;

    flush_evset(sf_evset);
    flush_evset(helper_sf_evset);
    _lfence();

    if (use_sense) {
        prime_skx_sf_evset_ps_sense(sf_chain1, sf_chain2, true, NULL);
        prime_skx_sf_evset_ps_sense(sf_chain1, sf_chain2, false, NULL);
    } else {
        prime_skx_sf_evset_ps_flush(sf_evset, sf_chain1, array_repeat, l2_repeat);
    }

    u64 last_tsc = _rdtsc();
    u8 *scope = sf_evset->addrs[0];
    while (n_recvs < max_recv) {
        u64 now_tsc = _rdtsc(), end;
        bool switched_out = now_tsc - last_tsc > switched_thresh;

        u64 lat = _time_maccess_aux(scope, end, aux);
        bool spurious =
            (aux != last_aux) || lat > detected_cache_lats.interrupt_thresh;

        if (spurious || lat > threshold) {
            if (use_sense) {
                prime_sense = !prime_sense;
                _lfence();
                prime_skx_sf_evset_ps_sense(sf_chain1, sf_chain2, prime_sense,
                                            NULL);
                _lfence();
                scope = (u8 *)(prime_sense ? sf_chain2 : sf_chain1);
            } else {
                prime_skx_sf_evset_ps_flush(sf_evset, sf_chain1, array_repeat, l2_repeat);
            }

            if (!spurious) {
                _mfence();
                _lfence();
                u32 blindspot = _rdtscp_aux(&aux) - end;
                recv_recs[n_recvs++] = (cache_acc_rec){.tsc = end,
                                                       .iters = iters,
                                                       .aux = aux,
                                                       .lat = lat,
                                                       .blindspot = blindspot};
            }
            last_aux = aux;
            spurious_cnt += spurious;
        }

        if (switched_out && n_switches < max_recv) {
            switch_recs[n_switches++] =
                (sender_switched_out){.start = last_tsc, .end = now_tsc};
        }
        last_tsc = now_tsc;

        iters += 1;
        if (!monitor_only && iters % 128 == 0 && sender_ctrl.finished) {
            break;
        }
    }
    return n_recvs;
}

int covert_recv() {
    int ret = EXIT_SUCCESS;
    if (start_helper_thread(&hctrl)) {
        _error("Failed to start helper!\n");
        return EXIT_FAILURE;
    }

    EVSet *sf_evset = prepare_evsets();
    if (!sf_evset) {
        _error("Failed to prepare evset\n");
        ret = EXIT_FAILURE;
        goto err;
    }
    stop_helper_thread(&hctrl);

    u64 max_recv = n_emits * recv_scale;
    cache_acc_rec *recv_recs = calloc(max_recv, sizeof(*recv_recs));
    if (!recv_recs) {
        _error("Failed to allocate recv records\n");
        ret = EXIT_FAILURE;
        goto err;
    }

    sender_switched_out *switch_recs = calloc(max_recv, sizeof(*switch_recs));
    if (!switch_recs) {
        _error("Failed to allocate switched records\n");
        ret = EXIT_FAILURE;
        goto err;
    }

    u64 *min_diffs = NULL;
    if (!monitor_only) {
        min_diffs = calloc(n_emits, sizeof(*min_diffs));
        if (!min_diffs) {
            _error("Failed to allocate min diffs\n");
            return EXIT_FAILURE;
        }

        pthread_barrier_init(&barrier, &battr, 2);
        sender_ctrl.barrier = &barrier;
        sender_ctrl.finished = false;
        sender_ctrl.emit_recs = calloc(n_emits, sizeof(struct covert_emit_rec));
        if (!sender_ctrl.emit_recs) {
            _error("Failed to allocate emit buffer!\n");
            ret = EXIT_FAILURE;
            goto err;
        }

        if (pthread_create(&sender_ctrl.pid, NULL, covert_send, &sender_ctrl)) {
            _error("Failed to create the sender thread\n");
            ret = EXIT_FAILURE;
            goto err;
        }
        pthread_barrier_wait(&barrier);
    }

    size_t n_recvs = 0;
    if (use_prime_scope) {
        n_recvs = monitor_ps(sf_evset, recv_recs, switch_recs, max_recv);
    } else {
        n_recvs = monitor_para(sf_evset, recv_recs, switch_recs, max_recv);
    }

    size_t n_switches = 0;
    for (n_switches = 0; n_switches < max_recv; n_switches++) {
        if (switch_recs[n_switches].start) {
            printf("Switched: start: %lu; end: %lu\n", switch_recs[n_switches].start,
                   switch_recs[n_switches].end);
        } else {
            break;
        }
    }

    u32 evicted = 0, emitted = 0, detected = 0, slipped = 0;
    if (!monitor_only) {
        pthread_join(sender_ctrl.pid, NULL);

        u64 ridx = 0;
        for (u64 c = 0; c < n_emits; c++) {
            struct covert_emit_rec *er = &sender_ctrl.emit_recs[c];
            printf("Emit %2lu: tsc: %lu; aux: %u; lat: %u; bit: %u\n", c,
                   er->tsc, er->aux, er->lat, er->lat > 0);
            evicted += er->lat > detected_cache_lats.l2_thresh;
            if (er->lat > 0) {
                emitted += 1;
                while (ridx < n_recvs && recv_recs[ridx].tsc < er->tsc) {
                    ridx += 1;
                }

                if (ridx >= n_recvs) {
                    ridx = n_recvs - 1;
                }

                if (recv_recs[ridx].tsc >= er->tsc) {
                    if (ridx > 0) {
                        min_diffs[emitted] = _min(recv_recs[ridx].tsc - er->tsc,
                                            er->tsc - recv_recs[ridx - 1].tsc);
                    } else {
                        min_diffs[emitted] = recv_recs[ridx].tsc - er->tsc;
                    }
                } else {
                    min_diffs[emitted] = er->tsc - recv_recs[ridx].tsc;
                }
                detected += min_diffs[emitted] < 1000;

                for (size_t i = 0; i < n_switches; i++) {
                    if (er->tsc >= switch_recs[i].start &&
                        er->tsc <= switch_recs[i].end) {
                        slipped += 1;
                    }
                }
            }
        }
    }

    pprint_cache_acc_recs(recv_recs, n_recvs);

    _info("Sender emitted: %u; Sender evicted: %u; Rough detection: %u; "
          "Slipped: %u\n",
          emitted, evicted, detected, slipped);
    _info("Spurious count: %ld\n", spurious_cnt);

err:
    stop_helper_thread(&hctrl);
    return ret;
}

int main(int argc, char **argv) {
    int opt, opt_idx;
    static struct option long_opts[] = {
        {"secret-access", no_argument, NULL, 'a'},
        {"prime-scope", no_argument, NULL, 'p'},
        {"use-sense", no_argument, NULL, 's'},
        {"monitor-only", no_argument, NULL, 'm'},
        {"ptr-chase", no_argument, NULL, 'c'},
        {"emit-interval", required_argument, NULL, 'i'},
        {"num-emits", required_argument, NULL, 'n'},
        {"rec-scale", required_argument, NULL, 'r'},
        {"secret-time-scale", required_argument, NULL, 't'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "apsmci:n:r:t:", long_opts, &opt_idx)) != -1) {
        switch (opt) {
            case 'a': secret_access = true; break;
            case 'p': use_prime_scope = true; break;
            case 's': use_sense = true; break;
            case 'm': monitor_only = true; break;
            case 'c': ptr_chase = true; break;
            case 'i': emit_interval = strtoull(optarg, NULL, 10); break;
            case 'n': n_emits = strtoull(optarg, NULL, 10); break;
            case 'r': recv_scale = strtoull(optarg, NULL, 10); break;
            case 't': secret_timing_scale = strtod(optarg, NULL); break;
            default: _error("Unknown option %c\n", opt); return EXIT_FAILURE;
        }
    }

    if (monitor_only) {
        recv_scale = 1;
    }

    u8 *page = mmap_shared_init(NULL, PAGE_SIZE, 'a');
    if (!page) {
        _error("Failed to allocate the target page\n");
        return EXIT_FAILURE;
    }
    srand(time(NULL));
    u32 offset = (rand() % (PAGE_SIZE / CL_SIZE)) * CL_SIZE;
    target = page + offset;

    if (cache_env_init(1)) {
        _error("Failed to initialize cache env!\n");
        return EXIT_FAILURE;
    }
    return covert_recv();
}
