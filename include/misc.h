#pragma once

#include "attribs.h"
#include "num_types.h"
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static ALWAYS_INLINE u8 *mmap_private(void *addr, size_t size) {
    u8 *ptr = (u8 *)mmap(addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    return ptr;
}

static ALWAYS_INLINE u8 *mmap_private_init(void *addr, size_t size, u8 init) {
    u8 *ptr = mmap_private(addr, size);
    if (ptr) {
        memset(ptr, init, size);
    }
    return ptr;
}

static ALWAYS_INLINE u8 *mmap_shared(void *addr, size_t size) {
    u8 *ptr = (u8 *)mmap(addr, size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    return ptr;
}

static ALWAYS_INLINE u8 *mmap_shared_init(void *addr, size_t size, u8 init) {
    u8 *ptr = mmap_shared(addr, size);
    if (ptr) {
        memset(ptr, init, size);
    }
    return ptr;
}

static ALWAYS_INLINE void *malloc_shared(size_t size) {
    return mmap_shared(NULL, size);
}

static ALWAYS_INLINE void *calloc_shared(size_t nelem, size_t size) {
    return mmap_shared_init(NULL, nelem * size, 0);
}

static ALWAYS_INLINE u8 *mmap_exec(void *addr, u64 size) {
    u8 *ptr = (u8 *)mmap(addr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return NULL;
    }
    return ptr;
}

static ALWAYS_INLINE u8 *mmap_exec_init(void *addr, u64 size, u8 i) {
    u8 *ptr = mmap_exec(addr, size);
    if (ptr) {
        memset(ptr, i, size);
    }
    return ptr;
}

static ALWAYS_INLINE u8 *mmap_huge_private(void *addr, size_t size) {
    u8 *ptr = (u8 *)mmap((void *)addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    return ptr;
}

static ALWAYS_INLINE u8 *mmap_huge_private_init(void *addr, size_t size, u8 i) {
    u8 *ptr = mmap_huge_private(addr, size);
    if (ptr) {
        memset(ptr, i, size);
    }
    return ptr;
}

static ALWAYS_INLINE u8 *mmap_huge_shared(void *addr, size_t size) {
    u8 *ptr = (u8 *)mmap((void *)addr, size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    return ptr;
}

static ALWAYS_INLINE u8 *mmap_huge_shared_init(void *addr, size_t size, u8 i) {
    u8 *ptr = mmap_huge_shared(addr, size);
    if (ptr) {
        memset(ptr, i, size);
    }
    return ptr;
}

static __always_inline u8 *mmap_file(void *addr, const char *filename,
                                     bool writable) {
    int mask = writable ? O_RDWR : O_RDONLY;
    int fd = open(filename, mask);
    if (fd < 0) {
        return NULL;
    }

    struct stat stats;
    fstat(fd, &stats);

    int prot = PROT_READ;
    if (writable) {
        prot |= PROT_WRITE;
    }
    u8 *ptr = (u8 *)mmap(addr, stats.st_size, prot, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        return NULL;
    } else {
        return ptr;
    }
}

static ALWAYS_INLINE bool set_proc_affinity(uint core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

static ALWAYS_INLINE bool set_affinity_priority(uint core, int prio) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    bool ret1 = sched_setaffinity(getpid(), sizeof(set), &set) != -1;
    bool ret2 = setpriority(PRIO_PROCESS, 0, prio) != -1;
    return ret1 && ret2;
}
