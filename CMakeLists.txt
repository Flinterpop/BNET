cmake_minimum_required(VERSION 3.20)
project(BNET VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "BNET is a plain Win32 app and builds with MSVC only.")
endif()

# Static CRT, so the exe carries no VC++ redistributable dependency. Verify a
# release build with: dumpbin /dependents BNET.exe
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(BNET WIN32
    src/main.cpp
    src/NetConfig.cpp
    src/Presets.cpp
    res/BNET.rc
)

target_include_directories(BNET PRIVATE src res)

target_compile_definitions(BNET PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(BNET PRIVATE /W4 /WX /permissive-)

target_link_libraries(BNET PRIVATE
    iphlpapi
    ws2_32
    comctl32
    wbemuuid
    ole32
    oleaut32
)

# The manifest is embedded from the .rc, so the linker must not add its own.
target_link_options(BNET PRIVATE /MANIFEST:NO)

# Note: no VS_DPI_AWARE here -- that makes CMake emit its own manifest, which
# would fight the one embedded from the .rc. DPI awareness is declared there.
