
if(BUILD_TESTING)
    add_test(NAME agc_driver_depth_storage_copies COMMAND agc_driver_depth_storage_tests)
    add_test(NAME agc_driver_depth_storage_restricted_depth COMMAND agc_driver_depth_storage_tests --restricted-depth)
    foreach(mode IN ITEMS enabled disabled)
        set(test agc_driver_depth_storage_index_${mode})
        add_test(NAME ${test} COMMAND agc_driver_depth_storage_tests --lookup-index)
        set_tests_properties(${test} PROPERTIES
                ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        if(mode STREQUAL "disabled")
            set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT APS5_NO_DEPTH_STORAGE_INDEX=1)
        endif()
        if(WIN32)
            set_tests_properties(${test} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    set_tests_properties(agc_driver_depth_storage_copies PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_DEPTH_TRANSFER_PASS=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(agc_driver_depth_storage_restricted_depth PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_depth_storage_copies agc_driver_depth_storage_restricted_depth PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
