function(add_plugin)
    cmake_parse_arguments(
        PARSED_ARGS
        ""
        "NAME"
        "SRCS;DEPS"
        ${ARGN}
    )
    if(NOT PARSED_ARGS_NAME)
        message(FATAL_ERROR "You must provide a name for the plugin")
    endif(NOT PARSED_ARGS_NAME)

    add_library(${PARSED_ARGS_NAME} SHARED ${PARSED_ARGS_SRCS})

    # We tried to link both libraries directly to hyrise, which resulted in segmentation faults when the
    # `ENABLE_COVERAGE` compiler flag is set.
    target_link_libraries_system(
        ${PARSED_ARGS_NAME}

        tbb
        ${BOOST_LIBS}
    )

    foreach(dep ${PARSED_ARGS_DEPS})
        target_link_libraries(${PARSED_ARGS_NAME} PRIVATE ${dep})
    endforeach(dep)

    # Link libraries that are required by all plugins and that require system linking.
    target_link_libraries_system(${PARSED_ARGS_NAME} nlohmann_json::nlohmann_json sparse_map)

    # Prevent the linker under macOS from complaining about undefined methods.
    if (APPLE)
        target_link_libraries(${PARSED_ARGS_NAME} PRIVATE "-undefined dynamic_lookup")
    endif()
endfunction(add_plugin)

add_plugin(NAME hyriseMvccDeletePlugin SRCS mvcc_delete_plugin.cpp mvcc_delete_plugin.hpp DEPS gtest magic_enum)
add_plugin(NAME hyriseSecondTestPlugin SRCS second_test_plugin.cpp second_test_plugin.hpp)
add_plugin(NAME hyriseTestNonInstantiablePlugin SRCS non_instantiable_plugin.cpp)
add_plugin(NAME hyriseTestPlugin SRCS test_plugin.cpp test_plugin.hpp DEPS magic_enum sqlparser)
add_plugin(NAME hyriseUccDiscoveryPlugin SRCS ucc_discovery_plugin.cpp ucc_discovery_plugin.hpp DEPS compact_vector magic_enum sqlparser)

# We define TEST_PLUGIN_DIR to always load plugins from the correct directory for testing purposes.
add_definitions(-DTEST_PLUGIN_DIR="${CMAKE_BINARY_DIR}/lib/")
