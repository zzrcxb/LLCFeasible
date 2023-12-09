/**
 * @file uncore_msr_private.h
 * @author your name (you@domain.com)
 * @brief List of uncore PMON-related MSRs that should be "hidden" to users
 * @version 0.1
 * @date 2022-12-01
 *
 * @copyright Copyright (c) 2022
 *
 */

#pragma once
#include "attribs.h"
#include "num_types.h"

#ifndef UNCORE_GLB_CTRL
#define UNCORE_GLB_CTRL 0x700
#endif

#ifndef UNCORE_GLB_STATUS
#define UNCORE_GLB_STATUS 0x701
#endif

// 1. CHA related
#define CHA_MSR_STRIDE 0x10

#define CHA_BLOCK_CTRL_BASE 0xe00
#define CHA_EVTSEL0_BASE 0xe01
#define CHA_EVTSEL1_BASE 0xe02
#define CHA_EVTSEL2_BASE 0xe03
#define CHA_EVTSEL3_BASE 0xe04
#define CHA_FILTER0_BASE 0xe05
#define CHA_FILTER1_BASE 0xe06
#define CHA_STATUS_BASE 0xe07
#define CHA_CTR0_BASE 0xe08
#define CHA_CTR1_BASE 0xe09
#define CHA_CTR2_BASE 0xe0a
#define CHA_CTR3_BASE 0xe0b

#define DERIVE_MSR(BASE, STRIDE, ID, SUBID) ((BASE) + (ID) * (STRIDE) + SUBID)

static ALWAYS_INLINE u32 CHA_block_ctrl_msr(u32 cid) {
    return DERIVE_MSR(CHA_BLOCK_CTRL_BASE, CHA_MSR_STRIDE, cid, 0);
}

static ALWAYS_INLINE u32 CHA_evtsel_msr(u32 cid, u32 eid) {
    return DERIVE_MSR(CHA_EVTSEL0_BASE, CHA_MSR_STRIDE, cid, eid);
}

static ALWAYS_INLINE u32 CHA_filter_msr(u32 cid, u32 fid) {
    return DERIVE_MSR(CHA_FILTER0_BASE, CHA_MSR_STRIDE, cid, fid);
}

static ALWAYS_INLINE u32 CHA_status_msr(u32 cid) {
    return DERIVE_MSR(CHA_STATUS_BASE, CHA_MSR_STRIDE, cid, 0);
}

static ALWAYS_INLINE u32 CHA_ctr_msr(u32 cid, u32 ctr_id) {
    return DERIVE_MSR(CHA_CTR0_BASE, CHA_MSR_STRIDE, cid, ctr_id);
}
