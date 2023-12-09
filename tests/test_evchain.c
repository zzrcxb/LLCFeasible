#include "tests.h"
#include "cache/cache.h"
#include "core.h"

static bool _test_evchain_against_addrs(u8 **addrs, u32 n_addrs, evchain *node) {
    for (u32 i = 0; i < n_addrs; i++) {
        if (!(((u8 *)node == addrs[i]) &&
              ((u8 *)node->next == addrs[(i + 1) % n_addrs]) &&
              ((u8 *)node->prev == addrs[(i + n_addrs - 1) % n_addrs]))) {
            return true;
        }
        node = node->next;
    }
    return false;
}

unittest_res test_evchain() {
    u32 n_addrs = 8;
    u8 *pages = mmap_private_init(NULL, n_addrs * PAGE_SIZE, 0);
    u8 **addrs = calloc(8, sizeof(u8 *));

    if (!pages || !addrs) {
        return UNITTEST_ERR;
    }

    for (u32 i = 0; i < n_addrs; i++) {
        addrs[i] = pages + i * PAGE_SIZE;
    }

    evchain *head = evchain_stride(pages, PAGE_SIZE, n_addrs);
    if (!head) {
        return UNITTEST_ERR;
    }

    if (_test_evchain_against_addrs(addrs, n_addrs, head)) {
        return UNITTEST_FAIL;
    }

    memset(pages, 0, n_addrs * PAGE_SIZE);
    for (u32 i = 0; i < n_addrs; i++) {
        addrs[i] = pages + i * PAGE_SIZE + 0x800;
    }

    head = evchain_build(addrs, n_addrs);
    if (!head) {
        return UNITTEST_FAIL;
    }

    if (_test_evchain_against_addrs(addrs, n_addrs, head)) {
        return UNITTEST_FAIL;
    }

    free(addrs);
    munmap(pages, n_addrs * PAGE_SIZE);
    return UNITTEST_PASS;
}
