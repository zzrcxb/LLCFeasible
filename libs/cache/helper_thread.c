#include "cache/helper_thread.h"
#include "sugar.h"

void *helper_thread_worker(void * _args) {
    helper_thread_ctrl *ctrl = _args;
    while (true) {
        ctrl->waiting = true;
        while (ctrl->waiting);
        switch (ctrl->action) {
            case HELPER_STOP: {
                return NULL;
            }
            case READ_SINGLE: {
                _maccess((u8 *)ctrl->payload);
                break;
            }
            case TIME_SINGLE: {
                u8 *ptr = (u8 *)ctrl->payload;
                ctrl->lat = _time_maccess(ptr);
                break;
            }
            case READ_ARRAY: {
                struct helper_thread_read_array *arr = ctrl->payload;

                prime_cands_daniel(arr->addrs, arr->cnt, arr->repeat,
                                   arr->stride, arr->block);
                break;
            }
            case TRAVERSE_CANDS: {
                struct helper_thread_traverse_cands *cmd = ctrl->payload;
                cmd->traverse(cmd->cands, cmd->cnt, cmd->tconfig);
                break;
            }
        }
    }
}
