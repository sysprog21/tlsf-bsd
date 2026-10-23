cmake_minimum_required(VERSION 3.14)

project(tlsf LANGUAGES C VERSION 1.0)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(TLSF_ENABLE_ASSERT "Build with TLSF_ENABLE_ASSERT" OFF)
option(TLSF_ENABLE_CHECK "Build with TLSF_ENABLE_CHECK" OFF)
option(TLSF_BUILD_THREAD "Build the thread-safe tlsf_thread library" ON)

set(TLSF_WARNINGS
    -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wconversion)

# Configuration macros change the layout of tlsf_t, so they are PUBLIC: every
# consumer must see the same values the library was compiled with.
add_library(tlsf src/tlsf.c)
add_library(tlsf::tlsf ALIAS tlsf)
target_include_directories(tlsf PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(tlsf PUBLIC
    $<$<BOOL:${TLSF_ENABLE_ASSERT}>:TLSF_ENABLE_ASSERT>
    $<$<BOOL:${TLSF_ENABLE_CHECK}>:TLSF_ENABLE_CHECK>)
set_target_properties(tlsf PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tlsf PRIVATE ${TLSF_WARNINGS})
endif()

set(TLSF_INSTALL_TARGETS tlsf)

if(TLSF_BUILD_THREAD)
    find_package(Threads REQUIRED)
    add_library(tlsf_thread src/tlsf_thread.c)
    add_library(tlsf::tlsf_thread ALIAS tlsf_thread)
    target_link_libraries(tlsf_thread PUBLIC tlsf Threads::Threads)
    set_target_properties(tlsf_thread PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(tlsf_thread PRIVATE ${TLSF_WARNINGS})
    endif()
    list(APPEND TLSF_INSTALL_TARGETS tlsf_thread)
endif()

install(TARGETS ${TLSF_INSTALL_TARGETS}
    EXPORT tlsfTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/tlsf.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(TLSF_BUILD_THREAD)
    install(FILES include/tlsf_thread.h include/tlsf_pmr.hpp
        include/tlsf_thread_pmr.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

set(TLSF_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/tlsf)
install(EXPORT tlsfTargets NAMESPACE tlsf:: DESTINATION ${TLSF_CMAKE_DIR})
configure_package_config_file(cmake/tlsfConfig.cmake.in
    ${PROJECT_BINARY_DIR}/tlsfConfig.cmake
    INSTALL_DESTINATION ${TLSF_CMAKE_DIR})
install(FILES ${PROJECT_BINARY_DIR}/tlsfConfig.cmake
    DESTINATION ${TLSF_CMAKE_DIR})
