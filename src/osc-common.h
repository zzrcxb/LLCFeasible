#include "cache/cache.h"

static struct {
    const char *s;
    evset_algorithm a;
} algo_map[] = {{"straw", EVSET_ALGO_LAST_STRAW},
                {"straw-alt", EVSET_ALGO_LAST_STRAW_DEV},
                {"vila", EVSET_ALGO_GROUP_TEST},
                {"vila-random", EVSET_ALGO_GROUP_TEST_RANDOM},
                {"vila-noearly", EVSET_ALGO_GROUP_TEST_NOEARLY},
                {"ps", EVSET_ALGO_PRIME_SCOPE},
                {"ps-opt", EVSET_ALGO_PRIME_SCOPE_OPT},
                {"default", EVSET_ALGO_DEFAULT}};

static inline evset_algorithm parse_evset_algo(const char * algo_name) {
    for (u32 i = 0; i < _array_size(algo_map); i++) {
        if (strcmp(algo_name, algo_map[i].s) == 0) {
            return algo_map[i].a;
        }
    }
    return EVSET_ALGO_INVALID;
}
