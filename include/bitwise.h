#pragma once

#include "attribs.h"
#include "num_types.h"

#define _SET_BIT(data, bit) ((data) | (1ull << (bit)))
#define _CLEAR_BIT(data, bit) ((data) & ~(1ull << (bit)))
#define _TOGGLE_BIT(data, bit) ((data) ^ (1ull << (bit)))
#define _WRITE_BIT(data, bit, val)                                             \
    (((data) & (~(1ull << (bit)))) | ((!!(val)) << (bit)))
#define _TEST_BIT(data, bit) (!!((data) & (1ull << (bit))))
#define _SEL_NOSPEC(MASK, T, F)                                                \
    (((MASK) & (typeof((MASK)))(T)) | (~(MASK) & (typeof((MASK)))(F)))

#define _SHIFT_MASK(shift) ((1ull << shift) - 1)
#define _ALIGNED(data, shift) (!((u64)(data) & _SHIFT_MASK(shift)))
#define __ALIGN_UP(data, shift) ((((u64)(data) >> (shift)) + 1) << (shift))
#define _ALIGN_UP(data, shift)                                                 \
    ((typeof(data))(_ALIGNED(data, shift) ? (u64)(data)                        \
                                          : __ALIGN_UP(data, shift)))
#define _ALIGN_DOWN(data, shift)                                               \
    ((typeof(data))((u64)(data) & (~_SHIFT_MASK(shift))))

/**                        |-end      |- start
* Set data 0000000000000000111111111110000
  Count from right to left, starting from 0, range EXCLUDES end
  i.e., [start, end). Then, it assigns data[start:end] = new_val[0:end-start]
*/
static __always_inline uint64_t _write_bit_range(uint64_t data, uint16_t end,
                                                 uint16_t start,
                                                 uint64_t new_val) {
    uint16_t width = end - start;
    uint64_t mask = (1ull << width) - 1;
    if (end <= start)
        return data; // invalid range

    new_val = (new_val & mask) << start;
    mask = ~(mask << start);
    data = (data & mask) | new_val;

    return data;
}

static __always_inline uint64_t _read_bit_range(uint64_t data, uint16_t end,
                                                uint16_t start) {
    // end is excluded
    uint16_t width = end - start;
    uint64_t mask = (1ull << width) - 1;

    if (end <= start)
        return 0;
    else
        return (data >> start) & mask;
}
