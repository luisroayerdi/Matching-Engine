cmake_minimum_required(VERSION 3.28)
project(iex_matching_engine LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Platform check — macOS only
# ---------------------------------------------------------------------------
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "This project targets macOS only.")
endif()
add_compile_definitions(IEX_PLATFORM_MACOS)

# ---------------------------------------------------------------------------
# Compiler check
# ---------------------------------------------------------------------------
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Only Clang is supported. Set CXX=$(brew --prefix llvm)/bin/clang++")
endif()

# ---------------------------------------------------------------------------
# Strict warning flags scoped to our targets only via iex_warnings.
# NOT using add_compile_options — that would propagate -Werror into
# FetchContent dependencies (googletest, googlebenchmark) and break them.
# ---------------------------------------------------------------------------
add_library(iex_warnings INTERFACE)
target_compile_options(iex_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wshadow
    -Wno-unused-parameter
    -Werror
)

# ---------------------------------------------------------------------------
# Dependencies via FetchContent
# (vcpkg.json documents the intent; FetchContent is used until a VCPKG_ROOT
#  is established.)
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    GIT_SHALLOW    TRUE
)

set(INSTALL_GTEST             OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
# googlebenchmark adds -Werror to its own targets; turn it off so Clang 22's
# new -Wc2y-extensions (triggered by __COUNTER__ under -pedantic-errors) does
# not break the dep build. This only affects the benchmark library itself.
set(BENCHMARK_ENABLE_WERROR   OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest googlebenchmark)

# Clang 22 added -Wcharacter-conversion (sub-warning of -Wconversion) which
# fires on char8_t→char32_t implicit conversions inside gtest-printers.h.
# Suppress it only on the gtest targets — not our code.
foreach(_tgt IN ITEMS gtest gtest_main gmock gmock_main)
    if(TARGET ${_tgt})
        target_compile_options(${_tgt} PRIVATE -Wno-character-conversion)
    endif()
endforeach()
# Clang 22 under -pedantic treats __COUNTER__ as a C2y extension.
# Suppress it only on the benchmark targets.
foreach(_tgt IN ITEMS benchmark benchmark_main)
    if(TARGET ${_tgt})
        target_compile_options(${_tgt} PRIVATE -Wno-c2y-extensions)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Platform static library
# ---------------------------------------------------------------------------
add_library(iex_platform STATIC
    platform/clock.cpp
    platform/thread.cpp
    platform/memory.cpp
)
target_include_directories(iex_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(iex_platform PUBLIC iex_warnings)

# ---------------------------------------------------------------------------
# Engine static library
# ---------------------------------------------------------------------------
add_library(iex_engine_lib STATIC
    engine/order_pool.cpp
    engine/order_book.cpp
    engine/matching_engine.cpp
)
target_include_directories(iex_engine_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(iex_engine_lib PUBLIC iex_platform iex_warnings)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(unit_tests
    tests/unit/test_types.cpp
    tests/unit/test_order_pool.cpp
    tests/unit/test_order_book.cpp
    tests/unit/test_matching_engine.cpp
)
target_link_libraries(unit_tests PRIVATE iex_engine_lib iex_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(unit_tests)

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
add_executable(engine_benchmarks
    tests/bench/bench_engine.cpp
)
target_link_libraries(engine_benchmarks PRIVATE iex_engine_lib benchmark::benchmark_main)
# Does NOT link iex_warnings — flag order matters here.
# The BENCHMARK() macro expands __COUNTER__, which Clang 22 flags under
# -Wpedantic as -Wc2y-extensions. -Wno-c2y-extensions must appear AFTER
# -Wpedantic or -Wpedantic re-enables it. Specifying flags explicitly gives
# us control over the order.
target_compile_options(engine_benchmarks PRIVATE
    -Wall -Wextra -Wpedantic -Wno-c2y-extensions
    -Wconversion -Wshadow -Wno-unused-parameter -Werror
)
