cmake_minimum_required(VERSION 3.20)
project(recorder LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_GUI "Build with GUI support (requires OpenGL)" ON)

find_package(PkgConfig QUIET)

# --- PortAudio ---
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

# --- minimp3 (decodifica MP3, header-only) ---
include(FetchContent)
FetchContent_Declare(
    minimp3
    GIT_REPOSITORY https://github.com/lieff/minimp3.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(minimp3)
add_library(minimp3_dep INTERFACE)
target_include_directories(minimp3_dep INTERFACE ${minimp3_SOURCE_DIR})

# --- cpp-httplib (server HTTP per il controllo remoto, header-only) ---
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.16.3
    GIT_SHALLOW    TRUE
)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

# --- GUI dependencies (optional) ---
if(BUILD_GUI)
    include(FetchContent)

    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE
    )
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    # Wayland nativo (non solo X11/XWayland): necessario su Raspberry Pi
    # OS Bookworm+, che usa labwc (compositor Wayland) come window
    # manager di default. Richiede i pacchetti di sviluppo Wayland
    # installati sul sistema di build (vedi CMakeLists.txt per i dettagli).
    set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui PUBLIC glfw)

    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui PUBLIC OpenGL::GL)
endif()

# --- Version from git ---
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_VERSION)
    set(GIT_VERSION "unknown")
endif()
configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/version.h @ONLY)

# --- Application ---
set(RECORDER_SOURCES
    src/main.cpp
    src/cli.cpp
    src/device_list.cpp
    src/audio_writer.cpp
    src/flac_writer.cpp
    src/wav_writer.cpp
    src/recorder.cpp
)

if(BUILD_GUI)
    list(APPEND RECORDER_SOURCES
        src/gui.cpp
        src/audio_reader.cpp
        src/audio_player.cpp
        src/remote_control.cpp
    )
endif()

add_executable(recorder ${RECORDER_SOURCES})

target_include_directories(recorder PRIVATE src ${CMAKE_BINARY_DIR})
target_link_libraries(recorder PRIVATE portaudio_dep flac_dep)

if(BUILD_GUI)
    target_link_libraries(recorder PRIVATE imgui minimp3_dep httplib::httplib)
    target_compile_definitions(recorder PRIVATE HAS_GUI=1)
endif()

find_package(Threads REQUIRED)
target_link_libraries(recorder PRIVATE Threads::Threads)

if(APPLE)
    target_link_libraries(recorder PRIVATE "-framework CoreFoundation")
endif()

# --- Tests ---
enable_testing()

add_executable(recorder_tests
    tests/test_pipeline.cpp
    src/audio_writer.cpp
    src/flac_writer.cpp
    src/wav_writer.cpp
    src/device_list.cpp
)

target_include_directories(recorder_tests PRIVATE src)
target_link_libraries(recorder_tests PRIVATE flac_dep Threads::Threads)

# device_list.cpp uses portaudio for scan functions
target_link_libraries(recorder_tests PRIVATE portaudio_dep)

if(APPLE)
    target_link_libraries(recorder_tests PRIVATE "-framework CoreFoundation")
endif()

add_test(NAME pipeline_tests COMMAND recorder_tests)
