file(GLOB SOURCES "*.c")

add_library("CACHE" ${SOURCES})
target_link_libraries("CACHE" PMU pthread m)
