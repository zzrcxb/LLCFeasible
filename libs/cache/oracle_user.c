#ifndef __KERNEL__

#include "cache/oracle.h"
#include "pmu/intel.h"
#include "ptedit_header.h"
#include <sched.h>

static msr_op msr;
static intel_cha_pmon cha;
static intel_uncore_glb_ctrl unc_ctrl;
static bool msr_inited = false, cha_inited = false, ptedit_inited = false;

bool cache_oracle_init() {
    bool err = false;
    if (!err && !msr_inited) {
        msr_inited = !msr_op_init(&msr, sched_getcpu());
        err = !msr_inited;
        if (err) {
            // _error("Failed to initialize msr_op\n");
        } else {
            intel_uncore_glb_ctrl_init(&unc_ctrl, &msr);
        }
    }

    if (!err && !cha_inited) {
        cha_inited = !intel_cha_pmon_init(&cha, &msr);
        err = !cha_inited;
        if (err) {
            _error("Failed to initialize CHA\n");
        }
    }

    if (!err && !ptedit_inited) {
        ptedit_inited = !ptedit_init();
        err = !ptedit_inited;
        if (err) {
            _error("Failed to initialized pteditor\n");
        }
    }

    if (err) {
        cache_oracle_cleanup();
    }
    return err;
}

bool cache_oracle_inited() {
    return msr_inited && cha_inited && ptedit_inited;
}

void cache_oracle_cleanup() {
    if (msr_inited) {
        msr_op_cleanup(&msr);
    }

    if (ptedit_inited) {
        ptedit_cleanup();
    }
}

uintptr_t cache_oracle_pa(void *addr) {
    ptedit_entry_t entry;
    uintptr_t pfn = 0, pa, offset_shift = PAGE_SHIFT, offset;
    if (!ptedit_inited) {
        return -1;
    }

    entry = ptedit_resolve(addr, 0);
    if (entry.valid & PTEDIT_VALID_MASK_PTE) {
        pfn = ptedit_get_pfn(entry.pte);
    } else if (entry.valid & PTEDIT_VALID_MASK_PMD) {
        offset_shift += PL_BITS;
        pfn = ptedit_get_pfn(entry.pmd); // 2MB page
    } else if (entry.valid & PTEDIT_VALID_MASK_PUD) {
        offset_shift += 2 * PL_BITS;
        pfn = ptedit_get_pfn(entry.pud); // 1GB page
    } else {
        return -1;
    }

    offset = (uintptr_t)addr & ((1ull << offset_shift) - 1);
    pa = (pfn << PAGE_SHIFT) + offset;
    return pa;
}

i32 cache_set_idx(void *addr, cache_param *param) {
    uintptr_t pa = cache_oracle_pa(addr);
    return (pa >> param->num_cl_bits) % param->n_sets;
}

i32 cache_slice_idx(void *addr) {
    i32 cha_id = -1;
    u64 top = 0, scd = 0, measures = 1000;
    if (!msr_inited || !cha_inited) {
        return -1;
    }

    intel_uncore_stop_pmon(&unc_ctrl);
    intel_cha_pmon_reset_all(&cha);
    intel_cha_pmon_set_event(&cha, 0, 0x50, 0x3, "Reads");
    intel_cha_pmon_write_control(&cha);
    intel_uncore_start_pmon(&unc_ctrl);
    for (u32 i = 0; i < measures; i++) {
        _clflush(addr);
        _lfence();
        _maccess((u8 *)addr);
        _lfence();
    }
    intel_uncore_stop_pmon(&unc_ctrl);

    for (u32 i = 0; i < cha.num_chas; i++) {
        u64 cnt = intel_uncore_ctr_read(&cha.active_chas[i].base.counters[0]);
        if (cnt > top) {
            scd = top;
            top = cnt;
            cha_id = i;
        }
    }

    if (top - scd < measures * 3 / 10) {
        _warn("Diff too small! %p: CHA: %d; Top: %lu; Scd: %lu\n", addr, cha_id,
              top, scd);
        return -1;
    }
    return cha_id;
}

#endif
