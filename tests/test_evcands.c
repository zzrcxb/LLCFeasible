#include "tests.h"
#include "cache/cache.h"
#include "cache/evset.h"

unittest_res test_evcands() {
    if (cache_env_init(0)) {
        return UNITTEST_ERR;
    }

    EVCandsConfig config = {2, NULL};
    EVCands *cands = evcands_new(detected_l2, &config, NULL);
    if (!cands) {
        return UNITTEST_FAIL;
    }

    unittest_res res = UNITTEST_FAIL;
    if (cands->evb->n_pages !=
        cache_uncertainty(detected_l2) * detected_l2->n_ways * 2) {
        goto err;
    }

    if (evcands_populate(0x400, cands, &config)) {
        goto err;
    }

    for (size_t n = 0; n < cands->size; n++) {
        if (cands->cands[n] != cands->evb->buf + 0x400 + n * PAGE_SIZE) {
            goto err;
        }
    }
    res = UNITTEST_PASS;

err:
    evcands_free(cands);
    return res;
}
