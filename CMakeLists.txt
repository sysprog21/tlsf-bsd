cmake_minimum_required(VERSION 3.15)
project(TLSF_BSD)

option(BUILD_TLSF_THREAD "Builds tlsf_thread static library" ON)

option(BUILD_TESTS_TLSF "Builds tests for tlsf executable" ON)
option(BUILD_TESTS_TLSF_THREAD "Builds tests for tlsf_thread executable" ON)
option(BUILD_TLSF_BENCH "Builds benchmark for tlsf executable" ON)
option(BUILD_TLSF_WCET "Builds WCET(Worst-Case Execution Time) for tlsf executable" ON)

if(BUILD_TESTS_TLSF_THREAD AND NOT BUILD_TLSF_THREAD)
    message(FATAL_ERROR 
        "Configuration Error: Impossible to build tests for tlsf_thread, "
        "if BUILD_TLSF_THREAD is off."
        "Switch on BUILD_TLSF_THREAD or swicth off BUILD_TESTS_TLSF_THREAD.")
endif()

add_library(tlsf STATIC 
    src/tlsf.c
)

target_include_directories(tlsf PUBLIC include)

if(BUILD_TESTS_TLSF_THREAD)
    add_library(tlsf_thread STATIC 
        src/tlsf_thread.c
    )
    target_include_directories(tlsf_thread PUBLIC include)    
    target_link_libraries(tlsf_thread PUBLIC tlsf)
endif()

if(BUILD_TESTS_TLSF)
    add_executable(test tests/test.c)
    target_link_libraries(test PRIVATE tlsf)
endif()

if(BUILD_TESTS_TLSF_THREAD)
    add_executable(test_thread tests/test_thread.c)
    target_link_libraries(test_thread PRIVATE tlsf_thread)
endif()

if(BUILD_TLSF_BENCH)
    add_executable(bench tests/bench.c)
    target_link_libraries(bench PRIVATE tlsf)
endif()

if(BUILD_TLSF_WCET)
    add_executable(wcet tests/wcet.c)
    target_link_libraries(wcet PRIVATE tlsf)
endif()