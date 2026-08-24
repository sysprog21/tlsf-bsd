cmake_minimum_required(VERSION 3.15)
project(TLSF_BSD)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

option(BUILD_TLSF_THREAD "Builds tlsf_thread static library" ON)

option(BUILD_TESTS_TLSF "Builds tests for tlsf executable" ON)
option(BUILD_TESTS_TLSF_THREAD "Builds tests for tlsf_thread executable" ON)
option(BUILD_TLSF_BENCH "Builds benchmark for tlsf executable" ON)
option(BUILD_TLSF_WCET "Builds WCET(Worst-Case Execution Time) for tlsf executable" ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
    set(TLSF_ASSERT_DEFAULT ON)
else()
    set(TLSF_ASSERT_DEFAULT OFF)
endif()

option(TLSF_ENABLE_ASSERTIONS "Enable TLSF internal safety asserts and checks" ${TLSF_ASSERT_DEFAULT})

if(TLSF_ENABLE_ASSERTIONS)
    add_compile_definitions(TLSF_ENABLE_ASSERT TLSF_ENABLE_CHECK)
    message(STATUS "TLSF Assertions: ENABLED (Performance will be reduced)")
else()
    message(STATUS "TLSF Assertions: DISABLED (Optimized for speed)")
endif()

if(BUILD_TESTS_TLSF_THREAD AND NOT BUILD_TLSF_THREAD)
    message(FATAL_ERROR 
        "Configuration Error: Impossible to build tests for tlsf_thread, "
        "if BUILD_TLSF_THREAD is off."
        "Switch on BUILD_TLSF_THREAD or swicth off BUILD_TESTS_TLSF_THREAD.")
endif()

function(configure_test_target TARGET_NAME)
    if(TARGET ${TARGET_NAME})
        if(TLSF_ENABLE_ASSERTIONS)
            string(REGEX REPLACE "(^| )[-/]DNDEBUG($| )" " " NEW_FLAGS "${CMAKE_C_FLAGS_RELEASE}")
            set_target_properties(${TARGET_NAME} PROPERTIES COMPILE_FLAGS_RELEASE "${NEW_FLAGS}")
            
            get_target_property(COMPILE_DEFS ${TARGET_NAME} COMPILE_DEFINITIONS)
            if(COMPILE_DEFS)
                list(REMOVE_ITEM COMPILE_DEFS "NDEBUG")
                set_target_properties(${TARGET_NAME} PROPERTIES COMPILE_DEFINITIONS "${COMPILE_DEFS}")
            endif()
        else()

        endif()
    endif()
endfunction()

set(THREADS_PREFER_PTHREAD_FLAG ON) 
find_package(Threads REQUIRED)

add_library(tlsf STATIC 
    src/tlsf.c
)

target_include_directories(tlsf PUBLIC include)

if(BUILD_TLSF_THREAD)
    add_library(tlsf_thread STATIC 
        src/tlsf_thread.c
    )
    target_include_directories(tlsf_thread PUBLIC include)    
    target_link_libraries(tlsf_thread PUBLIC tlsf)
    target_link_libraries(tlsf_thread PUBLIC Threads::Threads)
endif()

if(BUILD_TESTS_TLSF)
    add_executable(test tests/test.c)
    target_link_libraries(test PRIVATE tlsf)
endif()

if(BUILD_TESTS_TLSF_THREAD)
    add_executable(test_thread tests/test_thread.c)
    target_link_libraries(test_thread PRIVATE tlsf_thread Threads::Threads)
endif()

if(BUILD_TLSF_BENCH)
    find_library(MATH_LIBRARY m)
    add_executable(bench tests/bench.c)
    target_link_libraries(bench PRIVATE tlsf)
    if(MATH_LIBRARY)
        target_link_libraries(bench PRIVATE ${MATH_LIBRARY})
    endif()
    if(WIN32)
        target_link_libraries(bench PRIVATE psapi)
    endif()
endif()

if(BUILD_TLSF_WCET)
    find_library(MATH_LIBRARY m)
    add_executable(wcet tests/wcet.c)
    target_link_libraries(wcet PRIVATE tlsf)
    if(MATH_LIBRARY)
        target_link_libraries(bench PRIVATE ${MATH_LIBRARY})
    endif()
endif()

configure_test_target(tlsf)
configure_test_target(tlsf_thread)
configure_test_target(test)
configure_test_target(test_thread)
configure_test_target(bench)
configure_test_target(wcet)
