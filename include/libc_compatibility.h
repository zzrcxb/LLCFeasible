#pragma once

#ifdef __KERNEL__
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sort.h>

#define _malloc(S) vmalloc((S))
#define _free(P) vfree((P))
#define _assert(PRED) BUG_ON(!(PRED))

#define _print(...)                                                            \
    do {                                                                       \
        printk(KERN_INFO __VA_ARGS__);                                         \
    } while (0)

#define _info(...)                                                             \
    do {                                                                       \
        printk(KERN_INFO __VA_ARGS__);                                         \
    } while (0)

#define _warn(...)                                                             \
    do {                                                                       \
        printk(KERN_WARNING __VA_ARGS__);                                      \
    } while (0)

#define _error(...)                                                            \
    do {                                                                       \
        printk(KERN_ERR __VA_ARGS__);                                          \
    } while (0)

#define _sort(_base, _cnt, _esize, _cmp)                                       \
    sort((_base), (_cnt), (_esize), (_cmp), NULL)

#else
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _malloc(S) malloc((S))
#define _free(P) free((P))
#define _assert(PRED) assert((PRED))
#define _print(...)                                                            \
    do {                                                                       \
        printf(__VA_ARGS__);                                                   \
    } while (0)

#define _info(...)                                                             \
    do {                                                                       \
        fprintf(stderr, "INFO: " __VA_ARGS__);                                  \
    } while (0)

#define _warn(...)                                                             \
    do {                                                                       \
        fprintf(stderr, "WARN: " __VA_ARGS__);                                  \
    } while (0)

#define _error(...)                                                            \
    do {                                                                       \
        fprintf(stderr, "ERROR: " __VA_ARGS__);                                 \
    } while (0)

#define _sort(_base, _cnt, _esize, _cmp)                                       \
    qsort((_base), (_cnt), (_esize), (_cmp))

#endif // end of "#ifdef __KERNEL__"

static inline void *_calloc(size_t nelem, size_t elem_sz) {
    void *p = _malloc(nelem * elem_sz);
    if (p) {
        memset(p, 0, nelem * elem_sz);
    }
    return p;
}
