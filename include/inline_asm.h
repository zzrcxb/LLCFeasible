#pragma once

#include <attribs.h>
#include <num_types.h>

#define _maccess(P)                                                            \
    do {                                                                       \
        typeof(*(P)) _NO_USE;                                                  \
        __asm__ __volatile__("mov (%1), %0\n"                                  \
                             : "=r"(_NO_USE)                                   \
                             : "r"((P))                                        \
                             : "memory");                                      \
    } while (0)

#define _mwrite(P, V)                                                          \
    do {                                                                       \
        __asm__ __volatile__("mov %1, %0\n"                                    \
                             : "=m"(*(P))                                      \
                             : "r"((V))                                        \
                             : "memory");                                      \
    } while (0)

#define _force_addr_calc(PTR)                                                  \
    do {                                                                       \
        __asm__ __volatile__("mov %0, %0\n\t" : "+r"(PTR)::"memory");          \
    } while (0)

static __always_inline void _prefetch_t0(const void *p) {
    __asm__ __volatile__("prefetcht0 (%0)" ::"r"(p) : "memory");
}

static __always_inline void _prefetch_t1(const void *p) {
    __asm__ __volatile__("prefetcht1 (%0)" ::"r"(p) : "memory");
}

static __always_inline void _prefetch_t2(const void *p) {
    __asm__ __volatile__("prefetcht2 (%0)" ::"r"(p) : "memory");
}

static __always_inline void _prefetch_nta(const void *p) {
    __asm__ __volatile__("prefetchnta (%0)" ::"r"(p) : "memory");
}

static __always_inline void _wbinvd(void) {
    __asm__ __volatile__("wbinvd" ::: "memory");
}

static ALWAYS_INLINE void _clflush(const void *p) {
    __asm__ __volatile__("clflush 0(%0)" : : "r"(p) : "memory");
}

static ALWAYS_INLINE void _lfence(void) {
    __asm__ __volatile__("lfence" ::: "memory");
}

static ALWAYS_INLINE void _mfence(void) {
    __asm__ __volatile__("mfence" ::: "memory");
}

// ====== timer related ======
static ALWAYS_INLINE u64 _rdtscp(void) {
    u64 rax;
    __asm__ __volatile__(
        "rdtscp\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %0\n\t"
        :"=a"(rax)
        :: "rcx", "rdx", "memory", "cc"
    );
    return rax;
}

static ALWAYS_INLINE u64 _rdtscp_aux(u32 *aux) {
    u64 rax;
    __asm__ __volatile__(
        "rdtscp\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %0\n\t"
        "mov %%ecx, %1\n\t"
        :"=a"(rax), "=r"(*aux)
        :: "rcx", "rdx", "memory", "cc"
    );
    return rax;
}

static ALWAYS_INLINE u64 _rdtsc(void) {
    u64 rax;
    __asm__ __volatile__(
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %0\n\t"
        :"=a"(rax)
        :: "rdx", "memory", "cc"
    );
    return rax;
}

// https://github.com/google/highwayhash/blob/master/highwayhash/tsc_timer.h
static ALWAYS_INLINE u64 _rdtsc_google_begin(void) {
    u64 t;
    __asm__ __volatile__("mfence\n\t"
                         "lfence\n\t"
                         "rdtsc\n\t"
                         "shl $32, %%rdx\n\t"
                         "or %%rdx, %0\n\t"
                         "lfence"
                         : "=a"(t)
                         :
                         // "memory" avoids reordering. rdx = TSC >> 32.
                         // "cc" = flags modified by SHL.
                         : "rdx", "memory", "cc");
    return t;
}

static ALWAYS_INLINE u64 _rdtscp_google_end(void) {
    u64 t;
    __asm__ __volatile__("rdtscp\n\t"
                         "shl $32, %%rdx\n\t"
                         "or %%rdx, %0\n\t"
                         "lfence"
                         : "=a"(t)
                         :
                         // "memory" avoids reordering.
                         // rcx = TSC_AUX. rdx = TSC >> 32.
                         // "cc" = flags modified by SHL.
                         : "rcx", "rdx", "memory", "cc");
    return t;
}

static ALWAYS_INLINE u64 _rdtscp_google_end_aux(u32 *aux) {
    u64 t;
    __asm__ __volatile__("rdtscp\n\t"
                         "shl $32, %%rdx\n\t"
                         "or %%rdx, %0\n\t"
                         "lfence"
                         : "=a"(t), "=c"(*aux)
                         :
                         // "memory" avoids reordering.
                         // rcx = TSC_AUX. rdx = TSC >> 32.
                         // "cc" = flags modified by SHL.
                         : "rdx", "memory", "cc");
    return t;
}

// use google's method by default
#define _timer_start _rdtsc_google_begin
#define _timer_end   _rdtscp_google_end
#define _timer_end_aux   _rdtscp_google_end_aux

static ALWAYS_INLINE u64 _timer_warmup(void) {
    u64 lat = _timer_start();
    lat = _timer_end() - lat;
    return lat;
}

#define _time_p_action(P, ACTION)                                              \
    ({                                                                         \
        typeof((P)) __ptr = (P);                                               \
        uint64_t __tsc;                                                        \
        /* make sure that address computation is done before _timer_start */   \
        _force_addr_calc(__ptr);                                               \
        __tsc = _timer_start();                                                \
        ACTION(__ptr);                                                         \
        _timer_end() - __tsc;                                                  \
    })

#define _time_p_action_aux(P, ACTION, end_tsc, end_aux)                        \
    ({                                                                         \
        typeof((P)) __ptr = (P);                                               \
        uint64_t __tsc;                                                        \
        /* make sure that address computation is done before _timer_start */   \
        _force_addr_calc(__ptr);                                               \
        __tsc = _timer_start();                                                \
        ACTION(__ptr);                                                         \
        (end_tsc) = _timer_end_aux(&(end_aux));                                \
        (end_tsc) - __tsc;                                                     \
    })

#define _time_maccess(P) _time_p_action(P, _maccess)
#define _time_icall(P) _time_p_action(P, _icall)
#define _time_maccess_aux(P, end_tsc, end_aux)                                 \
    _time_p_action_aux(P, _maccess, end_tsc, end_aux)

// CPUID related
typedef struct {
    u32 eax, ebx, ecx, edx;
} cpuid_query;

static __always_inline void _cpuid(cpuid_query *cpuid) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(cpuid->eax), "=b"(cpuid->ebx), "=c"(cpuid->ecx), "=d"(cpuid->edx)
        : "a"(cpuid->eax), "c"(cpuid->ecx)
        : "memory"
    );
    return;
}

#define VENDOR_STR_LEN 13u

static __always_inline void _detect_vendor(char *output) {
    cpuid_query cpuid = {0};
    _cpuid(&cpuid);

    ((u32 *)output)[0] = cpuid.ebx;
    ((u32 *)output)[1] = cpuid.edx;
    ((u32 *)output)[2] = cpuid.ecx;
    output[VENDOR_STR_LEN - 1] = '\0';
}

static __always_inline u64 _count_ones(u64 val) {
    u64 ret;
    __asm__ __volatile__("popcnt %1, %0" : "=r"(ret) : "r"(val) : "cc");
    return ret;
}

// misc
static __always_inline void _relax_cpu(void) {
    __asm__ __volatile__("rep; nop" ::: "memory");
}

static __always_inline void _icall(void *addr) {
    __asm__ __volatile__("call *%0\n" ::"r"((uintptr_t)addr));
}

static __always_inline void _barrier(void) {
    __asm__ __volatile__("" ::: "memory");
}

// MSR manipulation
static ALWAYS_INLINE u64 _rdmsr(u32 addr) {
    u32 edx = 0, eax = 0;
    __asm__ __volatile__("rdmsr" : "=a"(eax), "=d"(edx) : "c"(addr) : "memory");
    return (u64)eax | ((u64)edx << 0x20);
}

static ALWAYS_INLINE void _wrmsr(u32 addr, u64 val) {
    u32 eax = (u32)val, edx = (u32)(val >> 0x20);
    __asm__ __volatile__("wrmsr" ::"c"(addr), "a"(eax), "d"(edx) : "memory");
}

// TSX related
static ALWAYS_INLINE unsigned int _XBEGIN(void) {
    unsigned status;
    __asm__ __volatile__("mov $0xffffffff, %%eax;"
                         "xbegin _txnL1%=;"
                         "_txnL1%=:"
                         "mov %%eax, %0;"
                         : "=r"(status)::"eax", "memory");
    return status;
}

static ALWAYS_INLINE void _XEND(void) {
    __asm__ __volatile__("xend" ::: "memory");
}

#define _XBEGIN_STARTED (~0u)
