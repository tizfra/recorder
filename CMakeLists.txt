cmake_minimum_required(VERSION 3.20)
project(recorder LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig QUIET)

# --- PortAudio ---
# Try pkg-config first (works on Linux and macOS with homebrew/pkg-config installed),
# then fall back to find_library (works with homebrew on macOS without pkg-config),
# then fall back to FetchContent (builds from source).
set(PORTAUDIO_FOUND FALSE)

if(PkgConfig_FOUND)
    pkg_check_modules(PORTAUDIO IMPORTED_TARGET portaudio-2.0)
    if(PORTAUDIO_FOUND)
        add_library(portaudio_dep ALIAS PkgConfig::PORTAUDIO)
    endif()
endif()

if(NOT PORTAUDIO_FOUND)
    find_library(PORTAUDIO_LIB portaudio HINTS /opt/homebrew/lib)
    find_path(PORTAUDIO_INCLUDE portaudio.h HINTS /opt/homebrew/include)
    if(PORTAUDIO_LIB AND PORTAUDIO_INCLUDE)
        add_library(portaudio_dep INTERFACE)
        target_link_libraries(portaudio_dep INTERFACE ${PORTAUDIO_LIB})
        target_include_directories(portaudio_dep INTERFACE ${PORTAUDIO_INCLUDE})
        set(PORTAUDIO_FOUND TRUE)
    endif()
endif()

if(NOT PORTAUDIO_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        portaudio
        GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
        GIT_TAG        v19.7.0
        GIT_SHALLOW    TRUE
    )
    set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(portaudio)
    add_library(portaudio_dep ALIAS portaudio)
endif()

# --- FLAC ---
set(FLAC_DEP_FOUND FALSE)

if(PkgConfig_FOUND)
    pkg_check_modules(FLAC_PKG IMPORTED_TARGET flac)
    if(FLAC_PKG_FOUND)
        add_library(flac_dep ALIAS PkgConfig::FLAC_PKG)
        set(FLAC_DEP_FOUND TRUE)
    endif()
endif()

if(NOT FLAC_DEP_FOUND)
    find_library(FLAC_LIB FLAC HINTS /opt/homebrew/lib)
    find_path(FLAC_INCLUDE FLAC/stream_encoder.h HINTS /opt/homebrew/include)
    if(FLAC_LIB AND FLAC_INCLUDE)
        add_library(flac_dep INTERFACE)
        target_link_libraries(flac_dep INTERFACE ${FLAC_LIB})
        target_include_directories(flac_dep INTERFACE ${FLAC_INCLUDE})
        set(FLAC_DEP_FOUND TRUE)
    endif()
endif()

if(NOT FLAC_DEP_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        flac
        GIT_REPOSITORY https://github.com/xiph/flac.git
        GIT_TAG        1.4.3
        GIT_SHALLOW    TRUE
    )
    set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(flac)
    add_library(flac_dep ALIAS FLAC)
endif()

# --- Application ---
add_executable(recorder
    src/main.cpp
    src/cli.cpp
    src/device_list.cpp
    src/flac_writer.cpp
    src/recorder.cpp
)

target_include_directories(recorder PRIVATE src)
target_link_libraries(recorder PRIVATE portaudio_dep flac_dep)

find_package(Threads REQUIRED)
target_link_libraries(recorder PRIVATE Threads::Threads)
