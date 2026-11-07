# Host build of rmw_qnx's core and its tests: no QNX header, no guest, no network at test time.
#   cmake -S tests/core -B build/host && cmake --build build/host -j4 && ctest --test-dir build/host
# COVERAGE=ON adds gcov instrumentation (report with gcovr).
cmake_minimum_required(VERSION 3.16)
project(rmw_qnx_core_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(COVERAGE "instrument for coverage" OFF)
option(SANITIZE "build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
  FetchContent_MakeAvailable(googletest)
endif()
find_package(Threads REQUIRED)

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../rmw_qnx/core)
file(GLOB TESTS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
add_executable(core_tests ${TESTS})
target_include_directories(core_tests PRIVATE ${CORE})
target_compile_options(core_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(core_tests PRIVATE GTest::gtest_main Threads::Threads)
if(COVERAGE)
  target_compile_options(core_tests PRIVATE --coverage -O0)
  target_link_options(core_tests PRIVATE --coverage)
endif()

if(SANITIZE)
  target_compile_options(core_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(core_tests PRIVATE -fsanitize=address,undefined)
endif()

include(GoogleTest)
enable_testing()
gtest_discover_tests(core_tests PROPERTIES TIMEOUT 30)
