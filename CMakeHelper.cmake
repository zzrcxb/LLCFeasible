function(detect_arch)
    execute_process(COMMAND bash -c "gcc -march=native -Q --help=target | grep march | \
                                    head -n 1 | tr -d '[:space:]' | cut -d'=' -f2"
                    OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)

    message(STATUS "Native arch=${ARCH}")
    if(ARCH MATCHES "^haswell.*")
        message(STATUS "Haswell detected")
        add_definitions(-DHASWELL)
        set(UARCH_FOUND TRUE)
    endif()

    if(ARCH MATCHES "^broadwell.*")
        message(STATUS "Broadwell detected")
        add_definitions(-DHASWELL)
        set(UARCH_FOUND TRUE)
    endif()

    if(ARCH MATCHES "^skylake.*")
        message(STATUS "Skylake detected")
        add_definitions(-DSKYLAKE)
        set(UARCH_FOUND TRUE)
    endif()

    if(ARCH MATCHES "^cascadelake.*")
        message(STATUS "Cascade Lake detected")
        add_definitions(-DCASCADE)
        set(UARCH_FOUND TRUE)
    endif()

    if(ARCH MATCHES "^icelake.*")
        message(STATUS "Icelake detected")
        add_definitions(-DICELAKE)
        set(UARCH_FOUND TRUE)
    endif()

    if(ARCH MATCHES "^alderlake.*")
        message(STATUS "Alderlake detected")
        add_definitions(-DALDERLAKE)
        set(UARCH_FOUND TRUE)
    endif()

    if(NOT DEFINED UARCH_FOUND)
        message(FATAL_ERROR "Unsupported micro-architecture: ${ARCH}")
    endif()
endfunction()

function(detect_pti)
    execute_process(COMMAND cat /sys/devices/system/cpu/vulnerabilities/meltdown
    OUTPUT_VARIABLE PTI OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "Meltdown mitigation: ${PTI}")
    if(PTI MATCHES ".*PTI")
    message(STATUS "KPTI detected")
    else()
    message(STATUS "KPTI not detected")
    add_definitions(-DNOPTI)
    endif()
endfunction()
