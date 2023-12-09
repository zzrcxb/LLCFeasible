#include "cache/evset.h"


#define NUM_OFFSETS (PAGE_SIZE / CL_SIZE)

EVSet ***build_l2_evsets_all();

EVCands ***build_evcands_all(EVBuildConfig *conf, EVSet ***l2evsets);
