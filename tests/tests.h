#ifndef TESTS_H
#define TESTS_H

#include <stdbool.h>

typedef enum {
    UNITTEST_PASS,
    UNITTEST_FAIL,
    UNITTEST_ERR,
    UNITTEST_SKIP
} unittest_res;

unittest_res test_bitwise_basic();
unittest_res test_bitwise_complex();
unittest_res test_cache_latency();
unittest_res test_evchain();
unittest_res test_evcands();
unittest_res test_evset_l1d();
unittest_res test_evset_l2();

#endif // TESTS_H
