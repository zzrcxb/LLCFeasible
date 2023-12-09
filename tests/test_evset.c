#include "cache/cache.h"
#include "core.h"
#include "tests.h"

unittest_res test_evset_l1d() {
    if (cache_env_init(0)) {
        _error("Failed to initialize cache env!\n");
        return UNITTEST_ERR;
    }

    u8 *target = calloc(PAGE_SIZE, 1);
    if (!target) {
        return UNITTEST_ERR;
    }

    EVSet *l1d_evset = build_l1d_EVSet(target, &def_l1d_ev_config, NULL);
    if (!l1d_evset) {
        free(target);
        _error("Failed to build an l1d eviction set\n");
        return UNITTEST_FAIL;
    }

    unittest_res res;
    if (precise_evset_test(target, l1d_evset) == EV_POS) {
        res = UNITTEST_PASS;
    } else {
        res = UNITTEST_FAIL;
    }

    free(target);
    evset_free(l1d_evset);
    return res;
}

unittest_res test_evset_l2() {
    if (cache_env_init(0)) {
        _error("Failed to initialize cache env!\n");
        return UNITTEST_ERR;
    }

    u8 *target = calloc(PAGE_SIZE, 1);
    if (!target) {
        return UNITTEST_ERR;
    }

    EVSet *l2_evset = build_l2_EVSet(target, &def_l2_ev_config, NULL);
    if (!l2_evset) {
        free(target);
        _error("Failed to build an l2 eviction set\n");
        return UNITTEST_FAIL;
    }

    unittest_res res;
    if (precise_evset_test(target, l2_evset) == EV_POS) {
        res = UNITTEST_PASS;
    } else {
        res = UNITTEST_FAIL;
    }

    free(target);
    evset_free(l2_evset);
    return res;
}
