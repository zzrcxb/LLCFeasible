#pragma once

#include "msr.h"

#define INTEL_UNCORE_EVENT_NAME_LEN 28
#define INTEL_UNCORE_BLOCK_NAME_LEN 10
#define INTEL_UNCORE_CTR_BIT_WIDTH 48
#define INTEL_UNCORE_CTR_BIT_MASK ((1ull << INTEL_UNCORE_CTR_BIT_WIDTH) - 1)
#define INTEL_UNCORE_MAX_NUM_CTRS 4
#define INTEL_UNCORE_MAX_NUM_CPUS 28

typedef struct {
    u8 event_sel, umask;
    char name[INTEL_UNCORE_EVENT_NAME_LEN];
} intel_uncore_event; // generic uncore event type

typedef struct {
    u32 counter_addr, control_addr;

    intel_uncore_event event;
    bool reset, edge_detect, interrupt, enable, invert;
    u8 threshold;

    union {
        bool reserved_bit_19;
        bool tid_enable;
    };

    u64 count;
    msr_op *msr;
} intel_uncore_ctr; // generic uncore performance counter and control

void intel_uncore_ctr_init(intel_uncore_ctr *this, u32 counter_addr,
                           u32 control_addr, msr_op *msr);
u32 intel_uncore_ctr_encode(intel_uncore_ctr *this);
void intel_uncore_ctr_decode(intel_uncore_ctr *this, u32 ctrl_reg,
                             const char *event_name);
void intel_uncore_ctr_copy_event(intel_uncore_ctr *this,
                                 intel_uncore_event *event);
void intel_uncore_ctr_set_event(intel_uncore_ctr *this, u8 event_sel, u8 umask,
                                const char *name);
void intel_uncore_ctr_write_control(intel_uncore_ctr *this);
u64 intel_uncore_ctr_read(intel_uncore_ctr *this);
void intel_uncore_ctr_write(intel_uncore_ctr *this, u64 val);
void intel_uncore_ctr_reset(intel_uncore_ctr *this, u64 val);

typedef struct {
    u32 id, control_addr, status_addr, num_ctrs;
    intel_uncore_ctr counters[INTEL_UNCORE_MAX_NUM_CTRS];
    char name[INTEL_UNCORE_BLOCK_NAME_LEN];
    msr_op *msr;
} intel_uncore_block; // generic uncore PMON block

void intel_uncore_block_reset_all(intel_uncore_block *this);
void intel_uncore_block_reset_counter(intel_uncore_block *this);
void intel_uncore_block_reset_control(intel_uncore_block *this);
void intel_uncore_block_pp(intel_uncore_block *this);
void intel_uncore_block_write_control(intel_uncore_block *this);
void intel_uncore_block_freeze_on_pmi(intel_uncore_block *this, bool freeze);
bool intel_uncore_block_has_overflow(intel_uncore_block *this, u32 counter_id);

// ====== start of customization of uncore blocks ======
// CHA blocks and socket-wide CHA PMON

// State filter for CHA LLC_LOOKUP event
#define CHA_LLC_F_MASK (1u << 7)
#define CHA_LLC_M_MASK (1u << 6)
#define CHA_LLC_E_MASK (1u << 5)
#define CHA_LLC_S_MASK (1u << 4)
#define CHA_SF_H_MASK  (1u << 3)
#define CHA_SF_E_MASK  (1u << 2)
#define CHA_SF_S_MASK  (1u << 1)
#define CHA_LLC_I_MASK (1u << 0)

#define CHA_ANY_E_MASK (CHA_LLC_E_MASK | CHA_SF_E_MASK)
#define CHA_ANY_S_MASK (CHA_LLC_S_MASK | CHA_SF_S_MASK)
#define CHA_ANY_STATE_MASK ((1u << 8) - 1)
#define CHA_ANY_VALID_MASK (CHA_ANY_STATE_MASK ^ CHA_LLC_I_MASK)
#define CHA_SF_ANY_MASK (CHA_SF_H_MASK | CHA_SF_E_MASK | CHA_SF_S_MASK)
#define CHA_LLC_ANY_MASK (CHA_ANY_STATE_MASK ^ CHA_SF_ANY_MASK)

typedef struct {
    intel_uncore_block base;
    u32 filter0_addr, filter1_addr;

    struct {
        u8 core_id, thread_id;
        u16 state_mask;
    } filter0;

    struct {
        bool remote, local, all_op, near_mem, not_near_mem;
        u16 opc0, opc1;
        bool non_coherent, isoc;
    } filter1;
} intel_cha_block;

void intel_cha_block_init(intel_cha_block *this, u32 cha_id, msr_op *msr);
void intel_cha_block_write_filter0(intel_cha_block *this);
void intel_cha_block_write_filter1(intel_cha_block *this);
void intel_cha_block_write_filters(intel_cha_block *this);

#define INTEL_CHA_MAX_NUM_BLOCKS 28
typedef struct {
    u32 cha_mask, num_chas;
    intel_cha_block active_chas[INTEL_CHA_MAX_NUM_BLOCKS];
} intel_cha_pmon;

bool intel_cha_pmon_init(intel_cha_pmon *this, msr_op *msr);
void intel_cha_pmon_reset_all(intel_cha_pmon *this);
void intel_cha_pmon_reset_counter(intel_cha_pmon *this);
void intel_cha_pmon_reset_control(intel_cha_pmon *this);
void intel_cha_pmon_pp(intel_cha_pmon *this);
void intel_cha_pmon_set_event(intel_cha_pmon *this, u32 ctr_id, u8 event_sel,
                              u8 umask, const char *name);
void intel_cha_pmon_set_control(intel_cha_pmon *this, u32 ctr_id, u32 ctrl_reg,
                                const char *name);
void intel_cha_pmon_write_control(intel_cha_pmon *this);
void intel_cha_pmon_pid_enable(intel_cha_pmon *this, u8 core_id, u8 thread_id);
void intel_cha_pmon_filter_states(intel_cha_pmon *this, u16 state_mask);

// ====== end of the customization of uncore blocks ======

#define INTEL_UNCORE_GLB_CTRL 0x700
#define INTEL_UNCORE_GLB_STATUS 0x701
typedef struct {
    u64 pmi_mask;
    u64 start_mask, stop_mask;
    bool wake_on_pmi;
    msr_op *msr;
} intel_uncore_glb_ctrl; // global uncore PMON control

void intel_uncore_glb_ctrl_init(intel_uncore_glb_ctrl *this, msr_op *msr);
void intel_uncore_glb_set_core_pmi(intel_uncore_glb_ctrl *this, u32 core,
                                   bool enable);
bool intel_uncore_glb_overflow(intel_uncore_glb_ctrl *this, u32 unit_idx);

static ALWAYS_INLINE void intel_uncore_start_pmon(intel_uncore_glb_ctrl *this) {
    this->start_mask = _SET_BIT(this->pmi_mask, 61);
    this->stop_mask = _SET_BIT(this->pmi_mask, 63);
    msr_op_write(this->msr, INTEL_UNCORE_GLB_CTRL, this->start_mask);
}

static ALWAYS_INLINE void intel_uncore_stop_pmon(intel_uncore_glb_ctrl *this) {
    msr_op_write(this->msr, INTEL_UNCORE_GLB_CTRL, this->stop_mask);
}
