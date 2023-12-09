add_executable(osc-single-evset osc-single-evset.c)
target_link_libraries(osc-single-evset PUBLIC "CACHE" m pthread PMU)

add_executable(osc-multi-evset osc-multi-evset.c)
target_link_libraries(osc-multi-evset PUBLIC "CACHE" m pthread PMU)

add_executable(osc-activity osc-activity.c)
target_link_libraries(osc-activity PUBLIC "CACHE" m pthread)

add_executable(osc-covert osc-covert.c)
target_link_libraries(osc-covert PUBLIC "CACHE" m pthread PMU)
