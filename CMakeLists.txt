cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(qdags VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Same flags as the original repo (qdags_gdj/CMakeLists.txt). Without them the build is unoptimized
# and about 2.8x-3.3x slower on every query.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DNDEBUG")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

find_package(Threads REQUIRED)

# Manual sdsl-lite installs (./install.sh) ship no standard *Config.cmake, so look it up by include/lib,
# as the original qdags_gdj CMakeLists.txt did (~/include, ~/lib), plus the usual system paths.
find_path(SDSL_INCLUDE_DIR sdsl/bit_vectors.hpp
    HINTS $ENV{HOME}/include /usr/local/include /usr/include)
find_library(SDSL_LIBRARY sdsl
    HINTS $ENV{HOME}/lib /usr/local/lib /usr/lib)
find_library(DIVSUFSORT_LIBRARY divsufsort
    HINTS $ENV{HOME}/lib /usr/local/lib /usr/lib)
find_library(DIVSUFSORT64_LIBRARY divsufsort64
    HINTS $ENV{HOME}/lib /usr/local/lib /usr/lib)

if(NOT SDSL_INCLUDE_DIR OR NOT SDSL_LIBRARY OR NOT DIVSUFSORT_LIBRARY OR NOT DIVSUFSORT64_LIBRARY)
    message(FATAL_ERROR
        "sdsl-lite not found. Install sdsl-lite (https://github.com/simongog/sdsl-lite) "
        "or pass -DCMAKE_PREFIX_PATH=/path/to/sdsl to cmake.")
endif()

# --- qdags::join: dense multiway join engine (<=6 attributes) over qdag<RankBackend>, templated from the start
#     (see include/qdags/join) ---
add_library(qdags_join INTERFACE)
add_library(qdags::join ALIAS qdags_join)

target_include_directories(qdags_join INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${SDSL_INCLUDE_DIR})

target_link_libraries(qdags_join INTERFACE
    Threads::Threads
    ${SDSL_LIBRARY}
    ${DIVSUFSORT_LIBRARY}
    ${DIVSUFSORT64_LIBRARY})

target_compile_features(qdags_join INTERFACE cxx_std_17)

# --- qdags::query: RDF/BGP query layer (triple_pattern, resource_manager loading indexes from disk) ---
add_library(qdags_query INTERFACE)
add_library(qdags::query ALIAS qdags_query)

target_include_directories(qdags_query INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${SDSL_INCLUDE_DIR})

target_link_libraries(qdags_query INTERFACE
    Threads::Threads
    ${SDSL_LIBRARY}
    ${DIVSUFSORT_LIBRARY}
    ${DIVSUFSORT64_LIBRARY})

target_compile_features(qdags_query INTERFACE cxx_std_17)

# --- qdags::strategy: query execution strategies over qdags::join + qdags::query ---
add_library(qdags_strategy INTERFACE)
add_library(qdags::strategy ALIAS qdags_strategy)

target_include_directories(qdags_strategy INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${SDSL_INCLUDE_DIR})

target_link_libraries(qdags_strategy INTERFACE
    Threads::Threads
    ${SDSL_LIBRARY}
    ${DIVSUFSORT_LIBRARY}
    ${DIVSUFSORT64_LIBRARY})

target_compile_features(qdags_strategy INTERFACE cxx_std_17)

option(QDAGS_BUILD_TESTS "Build the qdags validators" ON)
# tests/ is optional: it is skipped without error if the folder is missing.
if(QDAGS_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

option(QDAGS_BUILD_EXAMPLES "Build the qdags examples/tools" ON)
if(QDAGS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
