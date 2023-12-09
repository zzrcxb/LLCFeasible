#include "core.h"

#include <time.h>
#include <unistd.h>

#ifdef USE_REALTIME
#define ts_now time_ns
#else
#define ts_now _rdtsc
#endif

typedef uint64_t timestamp;

// linux timer related functions
__always_inline static u64 get_timespec_ns(struct timespec *ts) {
    return ts->tv_nsec + ts->tv_sec * 1000000000ull;
}

__always_inline static u64 time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return get_timespec_ns(&ts);
}

// returns true if the target ns is missed
__always_inline static bool busy_wait_till(timestamp *now_ts, timestamp target,
                                           timestamp bound) {
    while (*now_ts < target) {
        _relax_cpu();
        *now_ts = ts_now();
    }

    return (bound != 0) && (*now_ts - target) >= bound;
}
