cmake_minimum_required(VERSION 3.16)

project(videocombiner
    VERSION 2.0.0
    DESCRIPTION "Build randomised multi-clip videos from a folder of clips"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single-config generators leave this empty, which silently gives an unoptimised
# binary.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(videocombiner
    src/main.cpp
    src/config.cpp
    src/media.cpp
    src/plan.cpp
    src/util.cpp)

target_include_directories(videocombiner PRIVATE src)

find_package(Threads REQUIRED)
target_link_libraries(videocombiner PRIVATE Threads::Threads)

# GCC before 9 keeps std::filesystem in a separate library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(videocombiner PRIVATE stdc++fs)
endif()

if(MSVC)
    target_compile_options(videocombiner PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(videocombiner PRIVATE -Wall -Wextra)
endif()

install(TARGETS videocombiner RUNTIME DESTINATION bin)
