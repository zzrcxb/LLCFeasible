#pragma once

#include "attribs.h"
#include "bitwise.h"
#include "libc_compatibility.h"
#include "num_types.h"
#include "inline_asm.h"

#ifndef __KERNEL__
// headers for MSR file reading
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define MSR_FILE_FORMAT "/dev/cpu/%d/msr"
#endif

typedef struct {
    int fd;
    bool errored;
} msr_op;

static ALWAYS_INLINE bool msr_op_init(msr_op *this, UNUSED int cpu) {
#ifndef __KERNEL__
    char msr_filename[64];

    snprintf(msr_filename, 64, MSR_FILE_FORMAT, cpu);
    this->fd = open(msr_filename, O_RDWR);
    this->errored = this->fd < 0;
#else
    this->fd = -1;
    this->errored = false;
#endif
    return this->errored;
}

static ALWAYS_INLINE void msr_op_cleanup(UNUSED msr_op *this) {
#ifndef __KERNEL__
    if (this->fd >= 0) {
        close(this->fd);
    }
#endif
}

static ALWAYS_INLINE u64 msr_op_read(UNUSED msr_op *this, u32 addr) {
#ifndef __KERNEL__
    u64 data;

    // stop here if the msr_fd is invalid and it's errored already
    if (this->fd < 0 || this->errored) {
        this->errored = true;
        return 0;
    }

    if (pread(this->fd, &data, sizeof(data), addr) != sizeof(data)) {
        this->errored = true;
        return 0;
    }
    return data;
#else
    return _rdmsr(addr);
#endif
}

static ALWAYS_INLINE void msr_op_write(UNUSED msr_op *this, u32 addr,
                                       u64 val) {
#ifndef __KERNEL__
    // stop here if the msr_fd is invalid and it's errored already
    if (this->fd < 0 || this->errored) {
        this->errored = true;
        return;
    }

    if (pwrite(this->fd, &val, sizeof(val), addr) != sizeof(val)) {
        this->errored = true;
        return;
    }
#else
    _wrmsr(addr, val);
#endif
}

static ALWAYS_INLINE u64 _read_msr_privileged(u32 addr) {
    u32 edx = 0, eax = 0;
    __asm__ __volatile__("rdmsr" : "=a"(eax), "=d"(edx) : "c"(addr) : "memory");
    return (u64)eax | ((u64)edx << 0x20);
}

static ALWAYS_INLINE void _write_msr_privileged(u32 addr, u64 val) {
    asm volatile("wrmsr"
                 :
                 : "c"(addr), "a"((u32)val), "d"((u32)(val >> 0x20))
                 : "memory");
}

static ALWAYS_INLINE u64 _read_msr_user(u32 addr, int msr_fd, bool *err) {
#ifndef __KERNEL__
    u64 data;

    // stop here if the msr_fd is invalid and it's errored already
    if (msr_fd <= 0 || *err) {
        *err = true;
        return 0;
    }

    if (pread(msr_fd, &data, sizeof(data), addr) != sizeof(data)) {
        *err = true;
        return 0;
    }
    *err = false;
    return data;
#else
    return 0;
#endif
}

static ALWAYS_INLINE void _write_msr_user(u32 addr, u64 val, int msr_fd, bool *err) {
#ifndef __KERNEL__
    // stop here if the msr_fd is invalid and it's errored already
    if (msr_fd <= 0 || *err) {
        *err = true;
        return;
    }

    if (pwrite(msr_fd, &val, sizeof(val), addr) != sizeof(val)) {
        *err = true;
        return;
    }
    *err = false;
#endif
}

// ====== List of PMU related MSRs ======
// 1. Core PMU related

#define IA32_FIXED_CTR_CTRL 0x38d
#define IA32_PERF_GLOBAL_STATUS 0x38e
#define IA32_PERF_GLOBAL_CTRL 0x38f
#define IA32_PERF_GLOBAL_OVF_CTL 0x390
#define IA32_PERF_GLOBAL_STATUS_RESET 0x390
#define IA32_PERF_GLOBAL_STATUS_SET 0x391
#define IA32_PERF_GLOBAL_INUSE 0x392
#define IA32_DEBUGCTL 0x1d9

#define IA32_PMC0 0xc1
#define IA32_PMC1 0xc2
#define IA32_PMC2 0xc3
#define IA32_PMC3 0xc4
#define IA32_PMC4 0xc5
#define IA32_PMC5 0xc6
#define IA32_PMC6 0xc7
#define IA32_PMC7 0xc8

#define IA32_PERFEVTSEL0 0x186
#define IA32_PERFEVTSEL1 0x187
#define IA32_PERFEVTSEL2 0x188
#define IA32_PERFEVTSEL3 0x189
#define IA32_PERFEVTSEL4 0x18a
#define IA32_PERFEVTSEL5 0x18b
#define IA32_PERFEVTSEL6 0x18c
#define IA32_PERFEVTSEL7 0x18d

#define IA32_FIXED_CTR0 0x309
#define IA32_FIXED_CTR1 0x30a
#define IA32_FIXED_CTR2 0x30b

#define MAX_FIXED_CTR_NUM 3
#define MAX_PMC_NUM 8

static ALWAYS_INLINE u32 core_pmc_msr(u32 pid) {
    return IA32_PMC0 + pid;
}

static ALWAYS_INLINE u32 core_pmc_evtsel_msr(u32 pid) {
    return IA32_PERFEVTSEL0 + pid;
}

static ALWAYS_INLINE u32 core_fixed_ctr_msr(u32 cid) {
    return IA32_FIXED_CTR0 + cid;
}
