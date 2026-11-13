# fractonica-core - CMake entry point, in two modes.
#
#   1. ESP-IDF component. When ESP_PLATFORM is set (idf.py sets it) this file
#      registers the sources as a component and stops. Put the repository in
#      the project's components/ directory under the name the component should
#      have, e.g. components/fractonica (see docs/GUIDE.md, "ESP32").
#
#   2. Ordinary CMake project. A static library `fractonica` with the alias
#      `fractonica::core`, for add_subdirectory() / FetchContent or a
#      stand-alone build:
#
#          cmake -S . -B build/cmake -DFRACTONICA_BUILD_TESTS=ON
#          cmake --build build/cmake
#          ctest --test-dir build/cmake --output-on-failure
#
# The library needs no build system at all: it is src/*.c compiled with
# -Iinclude. The Makefile is the reference build and holds the proofs of the
# design rules (`make check`); this file only packages the same sources.
#
# FRACTONICA_SOURCES is the one explicit list (ESP-IDF wants its sources named).
# A new src/*.c must be added to it; the ordinary mode refuses to configure
# when the list and the directory disagree.

cmake_minimum_required(VERSION 3.16)

set(FRACTONICA_SOURCES
    src/frc_arc.c
    src/frc_big.c
    src/frc_catalogue.c
    src/frc_civil.c
    src/frc_cycle.c
    src/frc_earth.c
    src/frc_glyph.c
    src/frc_hierarchy.c
    src/frc_ladder.c
    src/frc_math.c
    src/frc_notation.c
    src/frc_odometer.c
    src/frc_pure.c
    src/frc_span.c
    src/frc_sync.c
    src/frc_text.c
)

# ---------------------------------------------------------------------------
# 1. ESP-IDF component
# ---------------------------------------------------------------------------

if(ESP_PLATFORM)
    # src/ holds the internal headers (frc_internal.h, frc_big.h): private.
    idf_component_register(
        SRCS ${FRACTONICA_SOURCES}
        INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS src
    )
    return()
endif()

# ---------------------------------------------------------------------------
# 2. Ordinary CMake project
# ---------------------------------------------------------------------------

project(fractonica VERSION 0.2.0 LANGUAGES C)

# ON only when this is the top-level project, never for a consumer.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FRACTONICA_TOP_LEVEL ON)
else()
    set(FRACTONICA_TOP_LEVEL OFF)
endif()

option(FRACTONICA_BUILD_TESTS "Build the host tests and add them to CTest" OFF)
option(FRACTONICA_NO_FLOAT "Build without the optional floating-point API (defines FRC_NO_FLOAT)" OFF)
option(FRACTONICA_STRICT "Compile the library with the strict warning set as errors" ${FRACTONICA_TOP_LEVEL})

# The explicit list above must name every source: a file dropped into src/
# and forgotten here would be missing from the ESP-IDF build only.
file(GLOB FRACTONICA_SOURCES_ON_DISK RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS "src/*.c")
set(FRACTONICA_SOURCES_SORTED ${FRACTONICA_SOURCES})
list(SORT FRACTONICA_SOURCES_SORTED)
list(SORT FRACTONICA_SOURCES_ON_DISK)
if(NOT "${FRACTONICA_SOURCES_SORTED}" STREQUAL "${FRACTONICA_SOURCES_ON_DISK}")
    message(FATAL_ERROR
        "FRACTONICA_SOURCES in CMakeLists.txt does not match src/*.c.\n"
        "  listed : ${FRACTONICA_SOURCES_SORTED}\n"
        "  on disk: ${FRACTONICA_SOURCES_ON_DISK}")
endif()

add_library(fractonica STATIC ${FRACTONICA_SOURCES})
add_library(fractonica::core ALIAS fractonica)

target_include_directories(fractonica
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Pure C99, no extensions: the same language the Makefile enforces.
set_target_properties(fractonica PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

if(FRACTONICA_NO_FLOAT)
    # PUBLIC: the header hides the float API from the consumer too.
    target_compile_definitions(fractonica PUBLIC FRC_NO_FLOAT)
endif()

if(FRACTONICA_STRICT AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(fractonica PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
endif()

if(FRACTONICA_TOP_LEVEL)
    include(GNUInstallDirs)
    install(TARGETS fractonica ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/fractonica.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

# ---------------------------------------------------------------------------
# Tests: every tests/test_*.c is one program and one CTest test
# ---------------------------------------------------------------------------

if(FRACTONICA_BUILD_TESTS)
    enable_testing()
    file(GLOB FRACTONICA_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.c")
    foreach(test_source IN LISTS FRACTONICA_TEST_SOURCES)
        get_filename_component(test_name "${test_source}" NAME_WE)
        add_executable(${test_name} "${test_source}")
        # The tests are hosted programs (libc, libm, C11) and reach into the
        # internal headers of src/ and the oracle vectors of tests/vectors/.
        target_include_directories(${test_name} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_link_libraries(${test_name} PRIVATE fractonica)
        if(UNIX)
            target_link_libraries(${test_name} PRIVATE m)
        endif()
        set_target_properties(${test_name} PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()
