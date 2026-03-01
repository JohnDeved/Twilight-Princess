cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
project(tp-pc LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- ccache support (auto-detected if installed) ---
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "PC port: ccache found — ${CCACHE_PROGRAM}")
endif()

# --- Platform version ---
set(TP_VERSION "PC" CACHE STRING "Target platform: PC or NX_HB")

if(TP_VERSION STREQUAL "PC")
    add_compile_definitions(VERSION=13)
elseif(TP_VERSION STREQUAL "NX_HB")
    add_compile_definitions(VERSION=14)
else()
    message(FATAL_ERROR "TP_VERSION must be PC or NX_HB")
endif()

# --- bgfx rendering library (GX shim backend) ---
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    bgfx
    GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
    GIT_TAG        a952acef35b431a39c790d39736867c515f32aff  # v1.129.8958-499 pinned to immutable SHA
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(bgfx)

# --- SDL3 for windowing only (all other subsystems disabled for faster builds) ---
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO ON CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
set(SDL_HAPTIC ON CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_MISC OFF CACHE BOOL "" FORCE)
set(SDL_LOCALE OFF CACHE BOOL "" FORCE)
set(SDL_FILE_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_SYSTEM_ICONV OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    sdl3
    URL            https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.8.tar.gz
    URL_HASH       SHA256=76f3f3e19d21c55d7572d14247d30ad676854ba815204d90da957864df5bf82e
)
FetchContent_MakeAvailable(sdl3)

# --- Pre-compiled GX TEV shaders ---
# Shader binaries are pre-compiled and checked in at include/pal/gx/gx_tev_shaders_precompiled.h
# To recompile shaders: build with BGFX_BUILD_TOOLS=ON, then run shaderc manually.
# See src/pal/gx/shaders/ for source .sc files.

# --- Parse source file list from Shield splits.txt (non-debug) ---
file(STRINGS "${CMAKE_SOURCE_DIR}/config/Shield/splits.txt" SPLITS_LINES)

set(GAME_SOURCES "")
set(EXCLUDE_PREFIXES
    "dolphin/"
    "revolution/"
    "lingcod/"
    "PowerPC_EABI_Support/"
    "TRK_MINNOW_DOLPHIN/"
    "odemuexi2/"
    "odenotstub/"
    "amcstubs/"
    "NdevExi2A/"
    "REL/"
)

foreach(LINE IN LISTS SPLITS_LINES)
    # Match lines like "path/to/file.cpp:" or "path/to/file.c:"
    if(LINE MATCHES "^([A-Za-z0-9_/\\.-]+\\.(cpp|c)):$")
        set(SRC_PATH "${CMAKE_MATCH_1}")
        set(EXCLUDED FALSE)
        foreach(PREFIX IN LISTS EXCLUDE_PREFIXES)
            if(SRC_PATH MATCHES "^${PREFIX}")
                set(EXCLUDED TRUE)
                break()
            endif()
        endforeach()
        if(NOT EXCLUDED)
            if(EXISTS "${CMAKE_SOURCE_DIR}/src/${SRC_PATH}")
                list(APPEND GAME_SOURCES "src/${SRC_PATH}")
            endif()
        endif()
    endif()
endforeach()

list(LENGTH GAME_SOURCES NUM_GAME_SOURCES)
message(STATUS "PC port: ${NUM_GAME_SOURCES} game source files from splits.txt")

# --- PAL sources (new port code) ---
set(PAL_SOURCES
    src/pal/pal_crash.cpp
    src/pal/pal_endian.cpp
    src/pal/pal_j3d_swap.cpp
    src/pal/pal_error.cpp
    src/pal/pal_milestone.cpp
    src/pal/pal_os_stubs.cpp
    src/pal/pal_gx_stubs.cpp
    src/pal/pal_sdk_stubs.cpp
    src/pal/pal_math_stubs.cpp
    src/pal/pal_game_stubs.cpp
    src/pal/pal_remaining_stubs.cpp
    src/pal/pal_profile_list.cpp
    src/pal/pal_window.cpp
    src/pal/pal_input.cpp
    src/pal/pal_audio.cpp
    src/pal/pal_save.cpp
    src/pal/gx/gx_stub_tracker.cpp
    src/pal/gx/gx_fifo.cpp
    src/pal/gx/gx_render.cpp
    src/pal/gx/gx_state.cpp
    src/pal/gx/gx_texture.cpp
    src/pal/gx/gx_tev.cpp
    src/pal/gx/gx_displaylist.cpp
    src/pal/gx/gx_capture.cpp
    src/pal/pal_verify.cpp
)

# --- REL modules that are dynamically loaded on console but statically linked on PC ---
# Note: f_pc_profile_lst.cpp references hundreds of actor profiles from other RELs.
# Instead of compiling it, we call ModuleProlog directly (which sets g_fpcPf_ProfileList_p)
# via a stub in pal_remaining_stubs.cpp that only includes profiles already compiled.
set(REL_SOURCES
    src/d/actor/d_a_title.cpp
    src/d/actor/d_a_obj_ikada.cpp
    src/d/actor/d_a_shop_item.cpp
    src/d/actor/d_a_swc00.cpp
    src/d/actor/d_a_andsw2.cpp
    src/d/actor/d_a_npc_mk.cpp
    src/d/actor/d_a_kytag01.cpp
    src/d/actor/d_a_tag_event.cpp
    src/d/actor/d_a_tag_msg.cpp
    src/d/actor/d_a_vrbox.cpp
    src/d/actor/d_a_vrbox2.cpp
    src/d/actor/d_a_alldie.cpp
)

# --- Include directories ---
set(INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/dolphin
    ${CMAKE_SOURCE_DIR}/include/JSystem
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/assets/ShieldD
)

# --- Executable ---
add_executable(tp-pc ${GAME_SOURCES} ${PAL_SOURCES} ${REL_SOURCES})

target_include_directories(tp-pc PRIVATE ${INCLUDE_DIRS})

# Link bgfx rendering backend and SDL3 platform layer
target_link_libraries(tp-pc PRIVATE bgfx bimg bx SDL3::SDL3-static)

# --- Compiler settings ---
target_compile_definitions(tp-pc PRIVATE
    __REVOLUTION_SDK__
    WIDESCREEN_SUPPORT=1
    TP_DEFAULT_DATA_DIR="${CMAKE_SOURCE_DIR}/data"
    $<$<CONFIG:Debug>:DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:DEBUG=0>
)

# Suppress specific warnings for decompilation code
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tp-pc PRIVATE
        -include ${CMAKE_SOURCE_DIR}/include/pal/pal_platform.h
        -pipe
        -Wno-multichar
        -Wno-invalid-offsetof
        -Wno-write-strings
        -Wno-sign-compare
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-unused-but-set-variable
        -Wno-narrowing
        -Wno-missing-braces
        -Wno-comment
        -Wno-return-type
        -Wno-parentheses
        -Wno-delete-non-virtual-dtor
        -Wno-reorder
        -Wno-switch
        -Wno-enum-compare
        -Wno-format
        -fpermissive
        -fno-exceptions
    )
endif()

# --- Game data setup (auto-download if ROMS_TOKEN is available) ---
# Automatically downloads and extracts the disc image before building tp-pc.
# Can also be run manually: ninja -C build setup_game_data
find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(GH_CLI gh)
if(Python3_FOUND AND GH_CLI AND DEFINED ENV{ROMS_TOKEN})
    add_custom_target(setup_game_data
        COMMAND ${CMAKE_COMMAND} -E env
            "ROMS_TOKEN=$ENV{ROMS_TOKEN}"
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/setup_game_data.py
            --data-dir ${CMAKE_SOURCE_DIR}/data
            --rom-dir ${CMAKE_SOURCE_DIR}/orig/GZ2E01
            --extract-dir ${CMAKE_BINARY_DIR}/extract/GZ2E01
            --tools-dir ${CMAKE_BINARY_DIR}/tools
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Downloading and extracting game data..."
        VERBATIM
    )
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/data/res")
        # Auto-download: make tp-pc depend on game data setup
        add_dependencies(tp-pc setup_game_data)
        message(STATUS "PC port: Game data will be auto-downloaded before build (ROMS_TOKEN set)")
    else()
        message(STATUS "PC port: Game data found in data/")
    endif()
elseif(EXISTS "${CMAKE_SOURCE_DIR}/data/res")
    message(STATUS "PC port: Game data found in data/")
else()
    message(STATUS "PC port: No game data (set ROMS_TOKEN and install gh CLI to auto-download)")
endif()

message(STATUS "PC port: target tp-pc configured (VERSION=${TP_VERSION})")
