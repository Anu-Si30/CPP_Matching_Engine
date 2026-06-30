cmake_minimum_required(VERSION 3.20)
project(MatchingEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Compiler flags ───────────────────────────────────────────────────────────
if(MSVC)
    # Windows / MSVC
    add_compile_options(/W4 /std:c++20)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(/O2)
    endif()
else()
    # GCC / Clang (Linux, macOS, WSL)
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -march=native -DNDEBUG)
    else()
        add_compile_options(-g -fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

find_package(Threads REQUIRED)

# ─── Core exchange library ────────────────────────────────────────────────────
# All exchange logic is compiled into a static library that every
# test and benchmark can link against without recompiling.
add_library(exchange_core STATIC
    src/core/order_book.cpp
    src/core/matching_engine.cpp
)
target_include_directories(exchange_core PUBLIC include)
target_link_libraries(exchange_core PUBLIC Threads::Threads)

# ─── Stage 1: Interactive test ────────────────────────────────────────────────
add_executable(stage1_test tests/stage1_test.cpp)
target_link_libraries(stage1_test PRIVATE exchange_core)
