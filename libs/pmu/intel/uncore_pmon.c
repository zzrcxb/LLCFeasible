#include "pmu/intel/uncore_pmon.h"
#include "sugar.h"

#define EVENT_NAME_LEN INTEL_UNCORE_EVENT_NAME_LEN

void intel_uncore_ctr_init(intel_uncore_ctr *this, u32 counter_addr,
                           u32 control_addr, msr_op *msr) {
    this->counter_addr = counter_addr;
    this->control_addr = control_addr;
    _zero_struct(this->event);
    this->reset = false;
    this->edge_detect = false;
    this->interrupt = false;
    this->enable = false;
    this->invert = false;
    this->threshold = 0;
    this->reserved_bit_19 = false;
    this->count = 0;
    this->msr = msr;
}

u32 intel_uncore_ctr_encode(intel_uncore_ctr *this) {
    u64 config = 0;

    config = _write_bit_range(config, 32, 24, this->threshold);
    config = _WRITE_BIT(config, 23, this->invert);
    config = _WRITE_BIT(config, 22, this->enable);
    config = _WRITE_BIT(config, 20, this->interrupt);
    config = _WRITE_BIT(config, 19, this->reserved_bit_19);
    config = _WRITE_BIT(config, 18, this->edge_detect);
    config = _WRITE_BIT(config, 17, this->reset);
    config = _write_bit_range(config, 16, 8, this->event.umask);
    config = _write_bit_range(config, 8, 0, this->event.event_sel);
    return (u32)config;
}

void intel_uncore_ctr_decode(intel_uncore_ctr *this, u32 ctrl_reg,
                             const char *event_name) {
    this->event.event_sel = ctrl_reg & 0xff;
    this->event.umask = (ctrl_reg >> 8) & 0xff;
    this->reset = _TEST_BIT(ctrl_reg, 17);
    this->edge_detect = _TEST_BIT(ctrl_reg, 18);
    this->reserved_bit_19 = _TEST_BIT(ctrl_reg, 19);
    this->interrupt = _TEST_BIT(ctrl_reg, 20);
    this->enable = _TEST_BIT(ctrl_reg, 22);
    this->invert = _TEST_BIT(ctrl_reg, 23);
    this->threshold = (ctrl_reg >> 24) & 0xff;
    _copy_str(this->event.name, event_name, EVENT_NAME_LEN);
}

void intel_uncore_ctr_copy_event(intel_uncore_ctr *this,
                                 intel_uncore_event *event) {
    memcpy(&this->event, event, sizeof(*event));
    this->enable = false;
}

void intel_uncore_ctr_set_event(intel_uncore_ctr *this, u8 event_sel, u8 umask,
                                const char *name) {
    this->event.event_sel = event_sel;
    this->event.umask = umask;
    _copy_str(this->event.name, name, EVENT_NAME_LEN);
    this->enable = true;
}

void intel_uncore_ctr_write_control(intel_uncore_ctr *this) {
    u32 config = intel_uncore_ctr_encode(this);
    msr_op_write(this->msr, this->control_addr, config);
}

u64 intel_uncore_ctr_read(intel_uncore_ctr *this) {
    this->count = msr_op_read(this->msr, this->counter_addr);
    return this->count;
}

void intel_uncore_ctr_write(intel_uncore_ctr *this, u64 val) {
    msr_op_write(this->msr, this->counter_addr, val);
}

void intel_uncore_ctr_reset(intel_uncore_ctr *this, u64 val) {
    this->reset = true;
    intel_uncore_ctr_write_control(this);
    this->reset = false;
}

void intel_uncore_block_reset_all(intel_uncore_block *this) {
    msr_op_write(this->msr, this->control_addr, 0x3);
}

void intel_uncore_block_reset_counter(intel_uncore_block *this) {
    msr_op_write(this->msr, this->control_addr, 0x2);
}

void intel_uncore_block_reset_control(intel_uncore_block *this) {
    msr_op_write(this->msr, this->control_addr, 0x1);
}

// width per column X num columns
#define PP_LINE_LEN                                                            \
    ((INTEL_UNCORE_EVENT_NAME_LEN + 20) * (INTEL_UNCORE_MAX_NUM_CTRS + 1))
void intel_uncore_block_pp(intel_uncore_block *this) {
    char buf[PP_LINE_LEN];
    size_t offset = 0, i;

    snprintf(buf, PP_LINE_LEN - offset,
             "%-.*s%-u | ", INTEL_UNCORE_BLOCK_NAME_LEN, this->name, this->id);
    offset = strlen(buf);

    for (i = 0; i < INTEL_UNCORE_MAX_NUM_CTRS; i++) {
        if (this->counters[i].enable) {
            snprintf(buf + offset, PP_LINE_LEN - offset, "%.*s: %lu; ",
                    INTEL_UNCORE_EVENT_NAME_LEN, this->counters[i].event.name,
                    intel_uncore_ctr_read(&this->counters[i]));
            offset = strlen(buf);
        }
    }
    _print("%s\n", buf);
}

void intel_uncore_block_write_control(intel_uncore_block *this) {
    u32 i;
    for (i = 0; i < this->num_ctrs; i++) {
        intel_uncore_ctr_write_control(&this->counters[i]);
    }
}

void intel_uncore_block_freeze_on_pmi(intel_uncore_block *this, bool freeze) {
    u32 config = _WRITE_BIT(0u, 8, freeze);
    msr_op_write(this->msr, this->control_addr, config);
}

bool intel_uncore_block_has_overflow(intel_uncore_block *this, u32 counter_id) {
    u32 status = msr_op_read(this->msr, this->status_addr);
    if (counter_id < INTEL_UNCORE_MAX_NUM_CTRS) {
        return _TEST_BIT(status, counter_id);
    }
    return false;
}

void intel_uncore_glb_ctrl_init(intel_uncore_glb_ctrl *this, msr_op *msr) {
    this->pmi_mask = (1ull << INTEL_UNCORE_MAX_NUM_CPUS) - 1;
    this->wake_on_pmi = true;
    this->msr = msr;
}

void intel_uncore_glb_set_core_pmi(intel_uncore_glb_ctrl *this, u32 core,
                                   bool enable) {
    this->pmi_mask = _WRITE_BIT(this->pmi_mask, core, enable);
}

bool intel_uncore_glb_overflow(intel_uncore_glb_ctrl *this, u32 unit_idx) {
    u64 status = msr_op_read(this->msr, INTEL_UNCORE_GLB_STATUS);
    if (unit_idx < 64) {
        return _TEST_BIT(status, unit_idx);
    }
    return false;
}
