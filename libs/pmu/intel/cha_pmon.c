#ifndef __KERNEL__
#define _XOPEN_SOURCE 500
#include <fcntl.h>
#include <ftw.h> // for user-space /proc/bus/pci traversal
#include <stdio.h>
#include <unistd.h>
#endif

#include "pmu/intel/uncore_pmon.h"
#include "pmu/intel/uncore_msr_private.h"
#include "sugar.h"

#ifndef __KERNEL__
#define MAX_PCI_PATH 256
static char pci_dev_path[MAX_PCI_PATH] = "";
static u32 dev_30_cnt = 0;

#define PROC_PCI "/proc/bus/pci"
#define TARGET_DEVICE "1e.3"
#define CAPID6_OFFSET 0x9c

static int find_device_func(const char *fpath, const struct stat *sb, int tflag,
                            struct FTW *ftwbuf) {
    if (tflag == FTW_F && strncmp(fpath + ftwbuf->base, TARGET_DEVICE,
                                  strlen(TARGET_DEVICE)) == 0) {
        dev_30_cnt += 1;
        if (dev_30_cnt == 1) {
            _copy_str(pci_dev_path, fpath, MAX_PCI_PATH);
        } else {
            // _warn("Multiple device 30 detected! Skip %s\n", fpath);
        }
        return FTW_SKIP_SIBLINGS;
    }
    return FTW_CONTINUE;
}

static u32 _query_cha_mask_user() {
    int fd;

    dev_30_cnt = 0;
    if (nftw(PROC_PCI, find_device_func, 20, FTW_PHYS | FTW_ACTIONRETVAL)) {
        _error("Failed to traverse PCI\n");
        return 0;
    }

    if (dev_30_cnt == 0) {
        _error("Did not find device %s\n", TARGET_DEVICE);
        return 0;
    }

    fd = open(pci_dev_path, O_RDONLY);
    if (fd < 0) {
        _error("Failed to open %s\nDid you run with root permission?\n",
               pci_dev_path);
        return 0;
    }

    u32 data;
    if (pread(fd, &data, sizeof(data), CAPID6_OFFSET) != sizeof(data)) {
        _error("Failed to read CAPID6 register from PCI\n");
        return 0;
    }
    close(fd);
    return data;
}

#else
static u32 _query_cha_mask_kernel() {
    // TODO: kernel space implementation
    return 0;
}
#endif

static u32 _query_cha_mask() {
#ifndef __KERNEL__
    return _query_cha_mask_user();
#else
    return _query_cha_mask_kernel();
#endif
}

void intel_cha_block_init(intel_cha_block *this, u32 cha_id, msr_op *msr) {
    u32 i;
    this->base.id = cha_id;
    this->base.control_addr = CHA_block_ctrl_msr(cha_id);
    this->base.status_addr = CHA_status_msr(cha_id);
    this->base.num_ctrs = INTEL_UNCORE_MAX_NUM_CTRS;
    for (i = 0; i < this->base.num_ctrs; i++) {
        intel_uncore_ctr_init(&this->base.counters[i], CHA_ctr_msr(cha_id, i),
                              CHA_evtsel_msr(cha_id, i), msr);
    }

    _copy_str(this->base.name, "CHA", INTEL_UNCORE_BLOCK_NAME_LEN);
    this->base.msr = msr;

    // init filters
    this->filter0_addr = CHA_filter_msr(cha_id, 0);
    this->filter0.core_id = 0;
    this->filter0.thread_id = 0;
    this->filter0.state_mask = CHA_ANY_STATE_MASK;

    this->filter1_addr = CHA_filter_msr(cha_id, 1);
    memset(&this->filter1, 0, sizeof(this->filter1));
    this->filter1.not_near_mem = true;
    this->filter1.near_mem = true;
    this->filter1.all_op = true; // disable filter 1 by default
    this->filter1.local = true;
    this->filter1.remote = true;
    intel_cha_block_write_filters(this);
}

void intel_cha_block_write_filter0(intel_cha_block *this) {
    u32 config = 0u;
    config = _write_bit_range(config, 25, 17, this->filter0.state_mask);
    config = _write_bit_range(config, 9, 3, this->filter0.core_id);
    config = _write_bit_range(config, 3, 0, this->filter0.thread_id);

    msr_op_write(this->base.msr, this->filter0_addr, config);
}

void intel_cha_block_write_filter1(intel_cha_block *this) {
    u32 config = 0u;
    config = _WRITE_BIT(config, 31, this->filter1.isoc);
    config = _WRITE_BIT(config, 30, this->filter1.non_coherent);
    config = _write_bit_range(config, 29, 19, this->filter1.opc1);
    config = _write_bit_range(config, 19, 9, this->filter1.opc0);
    config = _WRITE_BIT(config, 5, this->filter1.not_near_mem);
    config = _WRITE_BIT(config, 4, this->filter1.near_mem);
    config = _WRITE_BIT(config, 3, this->filter1.all_op);
    config = _WRITE_BIT(config, 1, this->filter1.local);
    config = _WRITE_BIT(config, 0, this->filter1.remote);

    msr_op_write(this->base.msr, this->filter1_addr, config);
}

void intel_cha_block_write_filters(intel_cha_block *this) {
    intel_cha_block_write_filter0(this);
    intel_cha_block_write_filter1(this);
}

bool intel_cha_pmon_init(intel_cha_pmon *this, msr_op *msr) {
    u32 cid;
    this->cha_mask = _query_cha_mask();
    if (!this->cha_mask) {
        _error("Failed to retrieve CHA information, "
               "cannot initialize CHA PMON!\n");
        return true;
    }

    this->num_chas = _count_ones(this->cha_mask);
    for (cid = 0; cid < this->num_chas; cid++) {
        intel_cha_block_init(&this->active_chas[cid], cid, msr);
    }

    intel_cha_pmon_reset_all(this);
    return false;
}

void intel_cha_pmon_reset_all(intel_cha_pmon *this) {
    u32 cid;
    for (cid = 0; cid < this->num_chas; cid++) {
        intel_uncore_block_reset_all(&this->active_chas[cid].base);
    }
}

void intel_cha_pmon_reset_counter(intel_cha_pmon *this) {
    u32 cid;
    for (cid = 0; cid < this->num_chas; cid++) {
        intel_uncore_block_reset_counter(&this->active_chas[cid].base);
    }
}

void intel_cha_pmon_reset_control(intel_cha_pmon *this) {
    u32 cid;
    for (cid = 0; cid < this->num_chas; cid++) {
        intel_uncore_block_reset_control(&this->active_chas[cid].base);
    }
}

void intel_cha_pmon_pp(intel_cha_pmon *this) {
    u32 cid;

    _print("----------------------------------------\n");
    for (cid = 0; cid < this->num_chas; cid++) {
        intel_uncore_block_pp(&this->active_chas[cid].base);
    }
    _print("----------------------------------------\n");
}

void intel_cha_pmon_set_event(intel_cha_pmon *this, u32 ctr_id, u8 event_sel,
                              u8 umask, const char *name) {
    u32 cid;
    intel_uncore_ctr *ctr;
    for (cid = 0; cid < this->num_chas; cid++) {
        if (ctr_id < this->active_chas[cid].base.num_ctrs) {
            ctr = &this->active_chas[cid].base.counters[ctr_id];
            intel_uncore_ctr_set_event(ctr, event_sel, umask, name);
        }
    }
}

void intel_cha_pmon_set_control(intel_cha_pmon *this, u32 ctr_id, u32 ctrl_reg,
                                const char *name) {
    u32 cid;
    intel_uncore_ctr *ctr;
    for (cid = 0; cid < this->num_chas; cid++) {
        if (ctr_id < this->active_chas[cid].base.num_ctrs) {
            ctr = &this->active_chas[cid].base.counters[ctr_id];
            intel_uncore_ctr_decode(ctr, ctrl_reg, name);
        }
    }
}

void intel_cha_pmon_write_control(intel_cha_pmon *this) {
    u32 cid;
    for (cid = 0; cid < this->num_chas; cid++) {
        intel_uncore_block_write_control(&this->active_chas[cid].base);
    }
}

void intel_cha_pmon_pid_enable(intel_cha_pmon *this, u8 core_id, u8 thread_id) {
    u32 cid, ctr;
    intel_cha_block *block;
    for (cid = 0; cid < this->num_chas; cid++) {
        block = &this->active_chas[cid];
        block->filter0.core_id = core_id;
        block->filter0.thread_id = thread_id;
        for (ctr = 0; ctr < block->base.num_ctrs; ctr++) {
            block->base.counters[ctr].tid_enable = true;
        }
        intel_cha_block_write_filter0(block);
    }
}

void intel_cha_pmon_filter_states(intel_cha_pmon *this, u16 state_mask) {
    u32 cid;
    intel_cha_block *block;
    for (cid = 0; cid < this->num_chas; cid++) {
        block = &this->active_chas[cid];
        block->filter0.state_mask = state_mask;
        intel_cha_block_write_filter0(block);
    }
}
