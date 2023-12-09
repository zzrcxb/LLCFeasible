set(HAS_PTEDITOR FALSE)

function(check_pteditor HAS_PTEDITOR)
    if(EXISTS "${CMAKE_EXTERNAL_LIB_DIR}/PTEditor")
        message(STATUS "External library PTEditor detected")

        set(${HAS_PTEDITOR} TRUE PARENT_SCOPE)
        add_definitions(-DPTEDITOR)

        # link the header
        set(src_header "${CMAKE_EXTERNAL_LIB_DIR}/PTEditor/ptedit_header.h")
        set(sym_header "${CMAKE_CURRENT_SOURCE_DIR}/include/ptedit_header.h")
        if(NOT EXISTS ${sym_header})
            execute_process(COMMAND ln -s ${src_header} ${sym_header})
        endif()
    endif()
endfunction()

function(check_external)
    check_pteditor(HAS_PTEDITOR)
endfunction()

