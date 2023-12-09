#include "tests.h"
#include "sugar.h"
#include <assert.h>
#include <colors.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef unittest_res (*testfunc)(void);

typedef struct {
    testfunc func;
    char *desc;
    int retry;
} unittest_case;

unittest_case TESTS[] = {
    {test_bitwise_basic, "Test basic bitwise operations", 0},
    {test_bitwise_complex, "Test complex bitwise operations", 0},
    {test_cache_latency, "Test cache latency invariants", 1},
    {test_evchain, "Test evchain structure", 0},
    {test_evcands, "Test eviction candidates", 0},
    {test_evset_l1d, "Test L1d eviction set", 3},
    {test_evset_l2, "Test L2 eviction set", 3}};

void print_time_diff(struct timespec *tstart, struct timespec *tend) {
    assert(tstart && tend);
    double nsec_diff = (double)(tend->tv_sec - tstart->tv_sec) * 1e9 +
                       (double)(tend->tv_nsec - tstart->tv_nsec);

    if (nsec_diff > 1e9) {
        printf(" %6.2fs ", nsec_diff / 1e9);
    } else if (nsec_diff > 1e6) {
        printf(" %6.2fms", nsec_diff / 1e6);
    } else {
        printf(" %6.2fus", nsec_diff / 1e3);
    }
}

#define WIDTH 40

int main() {
    unsigned passed = 0, failed = 0, skipped = 0, errored = 0;
    struct timespec t_start, t_end;
    struct timespec e2e_start, e2e_end;
    clock_gettime(CLOCK_MONOTONIC, &e2e_start);

    for (unsigned idx = 0; idx < _array_size(TESTS); idx++) {
        clock_gettime(CLOCK_MONOTONIC, &t_start);
        unittest_res res = UNITTEST_FAIL;
        int retry = -1;
        while (res == UNITTEST_FAIL && retry < TESTS[idx].retry) {
            res = TESTS[idx].func();
            retry += 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &t_end);

        printf("%s ", TESTS[idx].desc);
        for (unsigned j = 0; j < (WIDTH - strlen(TESTS[idx].desc)); j++)
            printf(".");

        print_time_diff(&t_start, &t_end);

        switch (res) {
            case UNITTEST_FAIL: {
                printf(" [" RED_F "FAIL" RESET_C "]");
                failed += 1;
                break;
            }
            case UNITTEST_PASS: {
                printf(" [" GREEN_F "PASS" RESET_C "]");
                passed += 1;
                break;
            }
            case UNITTEST_SKIP: {
                printf(" [" YELLOW_F "SKIP" RESET_C "]");
                skipped += 1;
                break;
            }
            case UNITTEST_ERR: {
                printf(" [" MAGENTA_F "ERRORED" RESET_C "]");
                errored += 1;
                break;
            }
        }

        if (retry) {
            printf(" (retries: %d)", retry);
        }
        printf("\n");
    }

    clock_gettime(CLOCK_MONOTONIC, &e2e_end);

    printf("\n=== Summary ===\n");
    printf("ELAPSED:");
    print_time_diff(&e2e_start, &e2e_end);
    printf("\n");
    printf("PASS: %u\n", passed);
    printf("FAIL: %u\n", failed);
    printf("SKIP: %u\n", skipped);
    printf("ERRORED: %u\n", errored);
}
