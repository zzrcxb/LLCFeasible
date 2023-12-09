#include "cache/cache_param.h"

bool cache_oracle_init();

bool cache_oracle_inited();

void cache_oracle_cleanup();

uintptr_t cache_oracle_pa(void *addr);

i32 cache_set_idx(void *addr, cache_param *param);

i32 cache_slice_idx(void *addr);

#define INVALID_ADDR_HASH (-1ul)

static inline u64 llc_addr_hash(void *addr) {
    i32 cha_id = cache_slice_idx(addr);
    i32 set_id = cache_set_idx(addr, detected_l3);
    if (cha_id == -1 || set_id == -1) {
        return INVALID_ADDR_HASH;
    } else {
        return ((u64)cha_id << 32) | (u64)set_id;
    }
}
