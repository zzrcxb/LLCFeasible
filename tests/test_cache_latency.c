#include "tests.h"
#include "cache/cache.h"

unittest_res test_cache_latency() {
    cpu_caches caches = {0};
    detect_cpu_caches(&caches);

    cache_latencies clats = {0};
    if (cache_latencies_detect(&clats, &caches)) {
        return UNITTEST_FAIL;
    }
    return UNITTEST_PASS;
}
