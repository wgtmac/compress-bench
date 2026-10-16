cmake_minimum_required(VERSION 3.16)
project(compress-bench VERSION 1.0.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-march=native" COMPRESS_BENCH_HAS_MARCH_NATIVE)
if(NOT COMPRESS_BENCH_HAS_MARCH_NATIVE)
  message(FATAL_ERROR "The selected compiler does not support required -march=native")
endif()

set(COMPRESS_BENCH_ZSTD_SOURCE_DIR "" CACHE PATH "Path to the zstd source tree")
set(COMPRESS_BENCH_LZ4_SOURCE_DIR "" CACHE PATH "Path to the lz4 source tree")
if(NOT EXISTS "${COMPRESS_BENCH_ZSTD_SOURCE_DIR}/build/cmake/CMakeLists.txt")
  message(FATAL_ERROR "ZSTD_SOURCE_DIR must point to a zstd source tree")
endif()
if(NOT EXISTS "${COMPRESS_BENCH_LZ4_SOURCE_DIR}/build/cmake/CMakeLists.txt")
  message(FATAL_ERROR "LZ4_SOURCE_DIR must point to an lz4 source tree")
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-march=native)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3 -DNDEBUG)
  set(BENCH_COMPILE_FLAGS "-O3 -DNDEBUG -march=native")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-O0 -g)
  set(BENCH_COMPILE_FLAGS "-O0 -g -march=native")
else()
  set(BENCH_COMPILE_FLAGS "-march=native")
endif()

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory("${COMPRESS_BENCH_ZSTD_SOURCE_DIR}/build/cmake" "${CMAKE_BINARY_DIR}/zstd" EXCLUDE_FROM_ALL)

set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
set(LZ4_POSITION_INDEPENDENT_LIB ON CACHE BOOL "" FORCE)
add_subdirectory("${COMPRESS_BENCH_LZ4_SOURCE_DIR}/build/cmake" "${CMAKE_BINARY_DIR}/lz4" EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)

set(BENCH_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(BENCH_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(BENCH_COMPILER_PATH "${CMAKE_CXX_COMPILER}")
configure_file(src/build_info.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h" @ONLY)

add_executable(compress-bench
  src/main.cc
  src/codec.cc
  src/data.cc
  src/system.cc
  src/benchmark.cc
)

target_include_directories(compress-bench PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  "${COMPRESS_BENCH_ZSTD_SOURCE_DIR}/lib"
  "${COMPRESS_BENCH_LZ4_SOURCE_DIR}/lib"
)
target_link_libraries(compress-bench PRIVATE libzstd_static lz4_static Threads::Threads)
target_compile_features(compress-bench PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(compress-bench PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(APPLE)
  target_compile_definitions(compress-bench PRIVATE COMPRESS_BENCH_PLATFORM_MACOS=1)
elseif(UNIX)
  target_compile_definitions(compress-bench PRIVATE COMPRESS_BENCH_PLATFORM_LINUX=1)
endif()

enable_testing()
add_test(NAME self-test COMMAND compress-bench self-test)
