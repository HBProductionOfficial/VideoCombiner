cmake_minimum_required(VERSION 3.16)

project(videocombiner
    VERSION 2.2.0
    DESCRIPTION "Build randomised multi-clip videos from a folder of clips"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single-config generators leave this empty, which silently gives an
# unoptimised binary.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

if(WIN32)
    # The icon both executables carry.
    enable_language(RC)
    set(VC_ICON res/videocombiner.rc)
endif()

# Shared by the console tool and the window.
set(VC_CORE
    src/config.cpp
    src/engine.cpp
    src/media.cpp
    src/plan.cpp
    src/sheet.cpp
    src/util.cpp)

function(vc_configure target)
    target_include_directories(${target} PRIVATE src res)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    # GCC before 9 keeps std::filesystem in a separate library.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
        target_link_libraries(${target} PRIVATE stdc++fs)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

add_executable(videocombiner src/main.cpp ${VC_CORE} ${VC_ICON})
vc_configure(videocombiner)
install(TARGETS videocombiner RUNTIME DESTINATION bin)

# The window is Windows only. The console tool builds everywhere.
if(WIN32)
    add_executable(videocombiner-gui WIN32 src/gui_win32.cpp ${VC_CORE} ${VC_ICON})
    vc_configure(videocombiner-gui)
    target_link_libraries(videocombiner-gui PRIVATE comctl32 shell32 ole32)
    # MSVC takes these from the #pragma lines in gui_win32.cpp. MinGW needs them
    # spelled out, plus -municode because the entry point is wWinMain.
    if(MINGW)
        target_sources(videocombiner-gui PRIVATE res/mingw.rc)
        target_link_libraries(videocombiner-gui PRIVATE comdlg32 uuid)
        target_link_options(videocombiner-gui PRIVATE -municode)
    endif()
    install(TARGETS videocombiner-gui RUNTIME DESTINATION bin)
endif()

option(VIDEOCOMBINER_TESTS "Build the tests" OFF)
if(VIDEOCOMBINER_TESTS)
    enable_testing()
    add_executable(count_vs_enumerate
        tests/count_vs_enumerate.cpp
        src/plan.cpp
        src/util.cpp
        src/config.cpp)
    vc_configure(count_vs_enumerate)
    add_test(NAME combinations COMMAND count_vs_enumerate)
endif()
