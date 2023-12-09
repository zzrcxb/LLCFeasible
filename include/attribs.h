#pragma once

#define UNUSED __attribute__((unused))

#ifdef __clang__
#define OPTNONE __attribute__((optnone))
#else
#define OPTNONE __attribute__((optimize("O0")))
#endif

#define ALWAYS_INLINE inline __attribute__((__always_inline__))

#ifdef __KERNEL__
#define ALWAYS_INLINE_HEADER static ALWAYS_INLINE
#else
#define ALWAYS_INLINE_HEADER ALWAYS_INLINE
#endif
