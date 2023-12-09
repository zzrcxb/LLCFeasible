#include "cache/osc.h"
#include "cache/oracle.h"
#include "sync.h"

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
    u64 start, end, num_l2sets = cache_uncertainty(detected_l2);
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
                conf->cands_config.filter_ev = l2evsets[n][i];
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
