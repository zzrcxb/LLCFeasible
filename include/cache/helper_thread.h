#pragma once

#include "access_seq.h"
#include <pthread.h>

typedef enum {
    HELPER_STOP,
    READ_SINGLE,
    TIME_SINGLE,
    READ_ARRAY,
    TRAVERSE_CANDS
} helper_thread_action;

typedef struct {
    bool running;
    volatile bool waiting;
    volatile helper_thread_action action;
    void * volatile payload;
    u64 lat;
    pthread_t pid;
} helper_thread_ctrl;

void *helper_thread_worker(void * _args);

static __always_inline void wait_helper_thread(helper_thread_ctrl *ctrl) {
    while (!ctrl->waiting);
}

static __always_inline bool start_helper_thread(helper_thread_ctrl *ctrl) {
    ctrl->waiting = false;
    _barrier();
    if (pthread_create(&ctrl->pid, NULL, helper_thread_worker, ctrl)) {
        perror("Failed to start the helper thread!\n");
        return true;
    }
    wait_helper_thread(ctrl);
    ctrl->running = true;
    return false;
}

static __always_inline void stop_helper_thread(helper_thread_ctrl *ctrl) {
    if (ctrl->running) {
        ctrl->action = HELPER_STOP;
        _barrier();
        ctrl->waiting = false;
        pthread_join(ctrl->pid, NULL);
        ctrl->running = false;
    }
}

static __always_inline void
helper_thread_read_single(u8 *target, helper_thread_ctrl *ctrl) {
    _assert(ctrl->running);
    ctrl->action = READ_SINGLE;
    ctrl->payload = target;
    _barrier();
    ctrl->waiting = false;
    wait_helper_thread(ctrl);
}

static __always_inline u64
helper_thread_time_single(u8 *target, helper_thread_ctrl *ctrl) {
    _assert(ctrl->running);
    ctrl->action = TIME_SINGLE;
    ctrl->payload = target;
    _barrier();
    ctrl->waiting = false;
    wait_helper_thread(ctrl);
    _mfence();
    _lfence();
    return ctrl->lat;
}

struct helper_thread_read_array {
    u8 ** volatile addrs;
    volatile size_t cnt, repeat, stride, block;
    volatile bool bwd;
};

struct _evtest_config;

struct helper_thread_traverse_cands {
    void (*volatile traverse)(u8 **cands, size_t cnt, struct _evtest_config *c);
    u8 ** volatile cands;
    size_t volatile cnt;
    struct _evtest_config * volatile tconfig;
};
