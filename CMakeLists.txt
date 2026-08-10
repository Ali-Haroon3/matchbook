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

# Optional Python bindings. Off by default so the core stays
# zero-dependency. An installed pybind11 (e.g. `pip install pybind11`)
# is preferred; otherwise it is fetched -- so offline builds work
# anywhere pybind11 is already present.
option(MATCHBOOK_PYTHON "Build the pybind11 Python module" OFF)
if(MATCHBOOK_PYTHON)
    find_package(pybind11 CONFIG QUIET)
    if(NOT pybind11_FOUND)
        # Hint: pip's cmake dir, if pybind11 is installed but not on the
        # default prefix path.
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env python3 -m pybind11 --cmakedir
            OUTPUT_VARIABLE _pybind11_pip_dir OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_pybind11_pip_dir)
            find_package(pybind11 CONFIG QUIET PATHS "${_pybind11_pip_dir}")
        endif()
    endif()
    if(NOT pybind11_FOUND)
        include(FetchContent)
        FetchContent_Declare(pybind11
            URL https://github.com/pybind/pybind11/archive/refs/tags/v2.13.6.tar.gz
            URL_HASH SHA256=e08cb87f4773da97fa7b5f035de8763abc656d87d5773e62f6da0587d1f0ec20)
        FetchContent_MakeAvailable(pybind11)
    endif()
    pybind11_add_module(pymatchbook bindings/py_matchbook.cpp)
    target_link_libraries(pymatchbook PRIVATE matchbook)
    set_target_properties(pymatchbook PROPERTIES OUTPUT_NAME matchbook)
endif()

enable_testing()
add_test(NAME engine_tests COMMAND tests)
