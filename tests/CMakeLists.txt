file(GLOB TEST_SRC "*.c")
add_executable(unittest ${TEST_SRC})

target_link_libraries(unittest PUBLIC "CACHE" PMU)
