cmake_minimum_required(VERSION 3.20)
project(matchbook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(matchbook INTERFACE)
target_include_directories(matchbook INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(matchbook INTERFACE cxx_std_20)

set(MATCHBOOK_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wno-interference-size)

option(MATCHBOOK_SANITIZE "Build with ASAN + UBSAN" OFF)
if(MATCHBOOK_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

option(MATCHBOOK_NATIVE "Build with -march=native (benchmarks)" ON)

function(mb_target name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE matchbook)
    target_compile_options(${name} PRIVATE ${MATCHBOOK_WARNINGS})
    if(MATCHBOOK_NATIVE AND CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(${name} PRIVATE -O3 -march=native)
    endif()
endfunction()

mb_target(tests   tests/test_engine.cpp)
mb_target(bench   bench/bench_main.cpp)
mb_target(replay  tools/replay_main.cpp)
mb_target(itchgen tools/itchgen_main.cpp)
mb_target(mmsim   tools/mmsim_main.cpp)
mb_target(mcast   tools/mcast_main.cpp)

find_package(Threads REQUIRED)
target_link_libraries(bench  PRIVATE Threads::Threads)
target_link_libraries(replay PRIVATE Threads::Threads)
target_link_libraries(mcast  PRIVATE Threads::Threads)

enable_testing()
add_test(NAME engine_tests COMMAND tests)
