if(BUILD_TESTING)
    set_tests_properties(agc_driver_dispatch_cache_relocation PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_DISPATCH_RELOCATION=1" TIMEOUT 30)
    add_test(NAME agc_driver_dispatch_cache_relocation_off COMMAND agc_driver_dispatch_cache_relocation_tests)
    set_tests_properties(agc_driver_dispatch_cache_relocation_off PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_DISPATCH_RELOCATION=0" TIMEOUT 30 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_driver_dispatch_cache_relocation_interpreter COMMAND agc_driver_dispatch_cache_relocation_tests)
    set_tests_properties(agc_driver_dispatch_cache_relocation_interpreter PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_DISPATCH_RELOCATION=1;APS5_NO_SRT_TAPE=1" TIMEOUT 30 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_driver_dispatch_cache_relocation_evicted COMMAND agc_driver_dispatch_cache_relocation_tests --evicted)
    set_tests_properties(agc_driver_dispatch_cache_relocation_evicted PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_DISPATCH_RELOCATION=1;APS5_DISPATCH_CACHE_ENTRIES=1" TIMEOUT 30 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_driver_dispatch_cache_relocation_draw_pipeline COMMAND agc_driver_dispatch_cache_relocation_tests)
    set_tests_properties(agc_driver_dispatch_cache_relocation_draw_pipeline PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_DISPATCH_RELOCATION=1;APS5_DRAW_PIPELINE=1;APS5_PIPELINED_DRAWS=8;APS5_PIPELINE_DISPATCH=1;APS5_PIPELINE_DISPATCH_MISSES=1" TIMEOUT 30 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_dispatch_cache_relocation_off agc_driver_dispatch_cache_relocation_interpreter agc_driver_dispatch_cache_relocation_evicted agc_driver_dispatch_cache_relocation_draw_pipeline PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
