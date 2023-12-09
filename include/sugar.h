#pragma once

#include "libc_compatibility.h"
#include "num_types.h"

#define _copy_str(des, src, N)                                                 \
    do {                                                                       \
        if (src) {                                                             \
            strncpy((des), (src), (N));                                        \
            (des)[(N)-1] = '\0';                                               \
        } else {                                                               \
            (des)[0] = '\0';                                                   \
        }                                                                      \
    } while (0)

#define _zero_struct(obj)                                                      \
    do {                                                                       \
        memset(&(obj), 0, sizeof((obj)));                                      \
    } while (0)

#define _swap(X, Y)                                                            \
    do {                                                                       \
        typeof(X) _tmp = (X);                                                  \
        (X) = (Y);                                                             \
        (Y) = _tmp;                                                            \
    } while (0);

#define _min(x, y) ({(x) > (y) ? (y) : (x);})
#define _max(x, y) ({(x) > (y) ? (x) : (y);})

#define _array_size(x) (sizeof(x) / sizeof((x)[0]))

#define xstr(s) str(s)
#define str(s) #s
