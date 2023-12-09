#pragma once

#include "attribs.h"
#include "inline_asm.h"
#include "libc_compatibility.h"

#ifndef __KERNEL__
#define PAGE_SHIFT (12u)
#define PAGE_SIZE (1ull << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1)
#endif

#define HUGE_PAGE_SHIFT (21u)
#define HUGE_PAGE_SIZE (1ull << HUGE_PAGE_SHIFT)
#define HUGE_PAGE_MASK (HUGE_PAGE_SIZE - 1)

#define PL_BITS 9
#define PL_SIZE (1ull << PL_BITS)

static __always_inline u64 addr_crafter(u64 pl4, u64 pl3, u64 pl2, u64 pl1) {
    u64 page = (pl4 << 27) + (pl3 << 18) + (pl2 << 9) + pl1;
    return page << PAGE_SHIFT;
}

static __always_inline u16 get_PL_index(void *ptr, u8 level) {
    u64 page = ((uintptr_t)ptr >> PAGE_SHIFT);
    return (page >> ((level - 1) * 9)) & 0x1ff;
}

static __always_inline void print_pls(void *ptr) {
    u16 pl4 = get_PL_index(ptr, 4);
    u16 pl3 = get_PL_index(ptr, 3);
    u16 pl2 = get_PL_index(ptr, 2);
    u16 pl1 = get_PL_index(ptr, 1);

#ifdef __KERNEL__
    printk(KERN_INFO "%px\tPL4: %#x\tPL3: %#x\tPL2: %#x\tPL1: %#x\n",
           ptr, pl4, pl3, pl2, pl1);
#else
    printf("%p\tPL4: %#x\tPL3: %#x\tPL2: %#x\tPL1: %#x\n",
           ptr, pl4, pl3, pl2, pl1);
#endif
}

static __always_inline void *_page_start(void *ptr) {
    return (void *)((uintptr_t)ptr & (~PAGE_MASK));
}

static __always_inline void *_hugepage_start(void *ptr) {
    return (void *)((uintptr_t)ptr & (~HUGE_PAGE_MASK));
}

static __always_inline uintptr_t page_offset(void *ptr) {
    return (uintptr_t)ptr & PAGE_MASK;
}

static __always_inline uintptr_t hugepage_offset(void *ptr) {
    return (uintptr_t)ptr & HUGE_PAGE_MASK;
}

static __always_inline void *_ceil_to_offset(void *ptr, uintptr_t offset) {
    uintptr_t cur_offset = page_offset(ptr);
    return ptr + ((offset + PAGE_SIZE - cur_offset) & PAGE_MASK);
}

static __always_inline void *_ceil_to_hp_offset(void *ptr, uintptr_t offset) {
    uintptr_t cur_offset = hugepage_offset(ptr);
    return ptr + ((offset + HUGE_PAGE_SIZE - cur_offset) & HUGE_PAGE_MASK);
}

#define PAGE_START(_p) ((typeof(_p))_page_start((_p)))
#define HUGE_PAGE_START(_p) ((typeof(_p))_hugepage_start((_p)))
#define CEIL_TO_OFFSET(_p, _o) ((typeof(_p))_ceil_to_offset((_p), (_o)))
#define CEIL_TO_HP_OFFSET(_p, _o) ((typeof(_p))_ceil_to_hp_offset((_p), (_o)))
