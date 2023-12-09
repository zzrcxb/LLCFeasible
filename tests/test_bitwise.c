#include "tests.h"
#include "sugar.h"
#include <num_types.h>
#include <stdio.h>
#include <unistd.h>
#include <bitwise.h>

unittest_res test_bitwise_basic() {
    u64 results[] = {
        _SET_BIT(0ull, 2),
        _SET_BIT(0ull, 40),
        _SET_BIT(2ull, 1),
        _CLEAR_BIT(7ull, 1),
        _CLEAR_BIT(4ull, 8), // 4
        _TOGGLE_BIT(7ull, 2),
        _TOGGLE_BIT(8ull, 3),
        _WRITE_BIT(7ull, 0, 0),
        _WRITE_BIT(7ull, 0, 1),
        _WRITE_BIT(7ull, 3, 1), // 9
        _TEST_BIT(16ull, 4),
        _TEST_BIT(15ull, 4),
        _ALIGNED(0xb0, 6),
        _ALIGNED(0xc0, 6),
        __ALIGN_UP(0x1800, 12), // 14
        _ALIGN_UP(0x1a00, 12),
        _ALIGN_UP(0x1000, 12),
        _ALIGN_DOWN(0x10a0, 12),
    };

    u64 oracles[] = {
        4ull,
        (1ull << 40),
        2ull,
        5ull,
        4ull, // 4
        3ull,
        0ull,
        6ull,
        7ull,
        15ull, // 9
        true,
        false,
        false,
        true,
        0x2000, // 14
        0x2000,
        0x1000,
        0x1000
    };

    for (u32 i = 0; i < _array_size(oracles); i++) {
        if (results[i] != oracles[i]) {
            fprintf(stderr, "Pair %u failed; Expect %#lx, get %#lx\n", i,
                    oracles[i], results[i]);
            return UNITTEST_FAIL;
        }
    }
    return UNITTEST_PASS;
}

unittest_res test_bitwise_complex() {
    u64 data = 8;
    const u16 count = 5;
    u64 results[count];
    u64 oracles[count];

    data = _WRITE_BIT(data, 3, true);
    data = _WRITE_BIT(data, 2, true);
    data = _WRITE_BIT(data, 1, true);
    data = _WRITE_BIT(data, 0, true);
    results[0] = data;
    oracles[0] = 0xf;

    data = 0xffull;
    data = _write_bit_range(data, 16, 8, 0x40);
    results[1] = data;
    oracles[1] = 0x40ffull;

    data = 0x8888ull;
    data = _write_bit_range(data, 12, 4, 0xff);
    results[2] = data;
    oracles[2] = 0x8ff8ull;

    data = 0x8888ull;
    data = _read_bit_range(data, 8, 4);
    results[3] = data;
    oracles[3] = 0x8ull;

    results[4] = _read_bit_range(0x1c0003fu, 32, 22);
    oracles[4] = 0x7u;

    for (u32 i = 0; i < _array_size(oracles); i++) {
        if (results[i] != oracles[i]) {
            fprintf(stderr, "Pair %u failed; Expect %#lx, get %#lx\n", i,
                    oracles[i], results[i]);
            return UNITTEST_FAIL;
        }
    }
    return UNITTEST_PASS;
}
