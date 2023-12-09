#include "bitwise.h"
#include "cache/cache_param.h"

bool __has_hugepage = false;
cpu_caches detected_caches = {0};
cache_param *detected_l1d, *detected_l1i, *detected_l2, *detected_l3;

static const char *const cache_type_str[4] = {"Invalid cache", "Data cache",
                                              "Inst. cache", "Unified cache"};

void set_cache_num_slices(cache_param *param, u32 n_slices) {
    param->n_slices = n_slices;
    if (param->n_sets % n_slices != 0) {
        _warn("%u sets cannot be divided by %u slices, "
              "please double check the number of slices\n",
              param->n_sets, n_slices);
    }

    param->n_sets = param->n_sets / n_slices;
    if (!check_power_of_two(param->n_sets)) {
        _warn("Number of sets: %u is not a power of 2, \n"
              "please double check the number of slices",
              param->n_sets);
    }
    param->num_set_idx_bits = log2_ceil(param->n_sets);
}

static int detect_cache_param_level(cache_param *param, u32 cid) {
    cpuid_query cpuid = {0, 0, 0, 0};
    cpuid.eax = 0x4u; // cache param leaf
    cpuid.ecx = cid;
    _cpuid(&cpuid);

    param->type = _read_bit_range(cpuid.eax, 5, 0);
    if (param->type == CACHE_INVAL) {
        return 0;
    }

    param->level = _read_bit_range(cpuid.eax, 8, 5);
    param->self_init = _read_bit_range(cpuid.eax, 9, 8);
    param->fully_assoc = _read_bit_range(cpuid.eax, 10, 9);
    param->inclusive = _read_bit_range(cpuid.edx, 2, 1);
    param->complex_idx = _read_bit_range(cpuid.edx, 3, 2);
    param->line_size = _read_bit_range(cpuid.ebx, 12, 0) + 1;
    param->n_ways = _read_bit_range(cpuid.ebx, 32, 22) + 1;
    param->n_sets = cpuid.ecx + 1;
    param->n_slices = 1;
    param->size = (u64)param->line_size * param->n_sets * param->n_ways;

    if (param->line_size != CL_SIZE) {
        _warn("Level %u %s has a cacheline size of %uB while the default "
              "cacheline size is %luB!\n",
              param->level, cache_type_str[param->type], param->line_size,
              CL_SIZE);
    }

    if (!check_power_of_two(param->line_size)) {
        _warn("Cache line size: %u is not a power of 2\n", param->line_size);
    }
    param->num_cl_bits = log2_ceil(param->line_size);

    // try to automatically determine how many slices do L3 have
    u32 slice_cnt = 0;
    if (param->level == 3) {
        char *s = getenv("NUM_L3_SLICES"), *end;
        if (s) {
            slice_cnt = strtoul(s, &end, 0);
            if (s == end) {
                _error("Invalid int value in $NUM_L3_SLICES\n");
                slice_cnt = 0;
            } else {
                set_cache_num_slices(param, slice_cnt);
            }
        }

        if (!slice_cnt) {
            char vendor[VENDOR_STR_LEN] = {0};
            _detect_vendor(vendor);
            if (strcmp(vendor, "GenuineIntel") == 0) {
                bool has_smt = false;
                u32 smt_way = 1;
                cpuid.eax = 0x1;
                _cpuid(&cpuid);
                has_smt = _TEST_BIT(cpuid.edx, 28);

                // this heuristic assumes that the number of L3 slices is
                // the same as the number of physical cores. This assumption
                // can be false on some Intel processors
                cpuid.eax = 0xb;
                cpuid.ecx = 0x1;
                _cpuid(&cpuid);
                if (cpuid.ebx) {
                    slice_cnt = cpuid.ebx & 0xffff;
                    if (has_smt) {
                        cpuid.eax = 0xb;
                        cpuid.ecx = 0x0;
                        _cpuid(&cpuid);
                        smt_way = cpuid.ebx & 0xffff;
                        slice_cnt /= smt_way;
                    }
                } else {
                    // Sometimes ebx doesn't tell you about the number of cores.
                    // If that happens, we make the assumption that the number
                    // of sets per slice is a power of two and try to find
                    // the largest slice count to satisfy this assumption.
                    // If this heuristic doesn't work, try to manually set
                    // the environment variable NUM_L3_SLICES.
                    u32 max_core;
                    cpuid.eax = 0x1;
                    _cpuid(&cpuid);
                    max_core = (cpuid.ebx >> 16) & 0xff;
                    if (has_smt) {
                        max_core /= 2;
                    }

                    for (slice_cnt = max_core; slice_cnt > 0; slice_cnt--) {
                        if ((param->n_sets % slice_cnt == 0) &&
                            check_power_of_two(param->n_sets / slice_cnt)) {
                            break;
                        }
                    }
                }

                if (slice_cnt > 0) {
                    set_cache_num_slices(param, slice_cnt);
                } else {
                    set_cache_num_slices(param, 1);
                    _error("Failed to detect the number of L3 slices\n");
                }
            } else {
                set_cache_num_slices(param, 1);
                _warn("Do not support automatic L3 slice detect on %s, "
                      "please manually set the slice count\n", vendor);
            }
        }
    } else {
        set_cache_num_slices(param, 1);
    }

    return param->size;
}

static bool cache_level_valid(u32 cid) {
    cpuid_query cpuid = {0};
    cpuid.eax = 0x4u;
    cpuid.ecx = cid;
    _cpuid(&cpuid);
    return _read_bit_range(cpuid.eax, 5, 0) != CACHE_INVAL;
}

int detect_cpu_caches(cpu_caches *this) {
    while (cache_level_valid(this->num_caches)) {
        u32 cid = this->num_caches;
        if (cid < MAXIMUM_CACHES) {
            detect_cache_param_level(&this->caches[cid], cid);
            if (this->verbose) {
                pprint_cache_param(&this->caches[cid]);
            }
            this->num_caches += 1;
        } else {
            _warn("The system may have more than %u caches, however, we can "
                  "only save %u caches\n",
                  this->num_caches, MAXIMUM_CACHES);
            break;
        }
    }
    return this->num_caches;
}

void find_common_caches(cpu_caches *this, cache_param **l1i, cache_param **l1d,
                        cache_param **l2, cache_param **l3) {
    if (l1i) {
        *l1i = find_cpu_cache(this, 1, CACHE_INST);
        if (!*l1i) {
            _warn("Cannot find L1I cache\n");
        }
    }

    if (l1d) {
        *l1d = find_cpu_cache(this, 1, CACHE_DATA);
        if (!*l1d) {
            _warn("Cannot find L1D cache\n");
        }
    }

    if (l2) {
        *l2 = find_cpu_cache(this, 2, CACHE_UNIF);
        if (!*l2) {
            _warn("Cannot find L2 cache\n");
        }
    }

    if (l3) {
        *l3 = find_cpu_cache(this, 3, CACHE_UNIF);
        if (!*l3) {
            _warn("Cannot find L3 cache\n");
        }
    }
}

cache_param *find_cpu_cache(cpu_caches *this, u32 level, cache_type type) {
    u32 i;
    for (i = 0; i < this->num_caches; i++) {
        if ((this->caches[i].level == level) &&
            (this->caches[i].type == type)) {
            return &this->caches[i];
        }
    }
    return NULL;
}

void pprint_cache_param(cache_param *param) {
    u32 size = 0;
    const char *unit = NULL;

    if (param->size >= 4 * 1024 * 1024) {
        size = param->size / 1024 / 1024;
        unit = "MB";
    } else if (param->size >= 4 * 1024) {
        size = param->size / 1024;
        unit = "kB";
    } else {
        size = param->size;
        unit = "B";
    }

    _print("---------------------------------------------------------\n");
    _print("Level %u cache; Type: %s; Size: %u%s\n", param->level,
           cache_type_str[param->type], size, unit);
    _print("Self init: %u; Fully Assoc.: %u; Inclu.: %u; Complex Idx: %u\n",
           param->self_init, param->fully_assoc, param->inclusive,
           param->complex_idx);
    _print("Line size: %uB; #Sets: %u; #Ways: %u; #Slices: %u\n",
           param->line_size, param->n_sets, param->n_ways, param->n_slices);
    _print("---------------------------------------------------------\n");
}
