cmake_minimum_required(VERSION 3.20)
project(tp-pc LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

set(BGFX_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    bgfx
    GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
    GIT_TAG        v1.129.8958-499
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(bgfx)

# --- SDL3 for windowing, input, and audio ---
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.8
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(sdl3)

# --- Compile GX TEV shaders with bgfx shaderc ---
set(SHADER_DIR "${CMAKE_SOURCE_DIR}/src/pal/gx/shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
set(BGFX_SHADER_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/build/_deps/bgfx-src/bgfx/src")
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Find bgfx shader include directory (for bgfx_shader.sh)
set(BGFX_SHADER_INCLUDE "${bgfx_SOURCE_DIR}/bgfx/src")

# Shader list: vertex shaders
set(VS_SHADERS vs_gx_tev)
# Shader list: fragment shaders
set(FS_SHADERS fs_gx_passclr fs_gx_replace fs_gx_modulate fs_gx_blend fs_gx_decal)

# Determine platform flags for shaderc
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SHADERC_PLATFORM "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(SHADERC_PLATFORM "osx")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(SHADERC_PLATFORM "windows")
else()
    set(SHADERC_PLATFORM "linux")
endif()

set(COMPILED_SHADER_HEADERS "")

# Compile vertex shaders for each backend
foreach(SHADER ${VS_SHADERS})
    set(SHADER_SRC "${SHADER_DIR}/${SHADER}.sc")
    # GLSL 140
    set(GLSL_OUT "${SHADER_OUTPUT_DIR}/${SHADER}_glsl.bin.h")
    add_custom_command(
        OUTPUT ${GLSL_OUT}
        COMMAND shaderc
            -f ${SHADER_SRC}
            -o ${GLSL_OUT}
            --type vertex
            --platform ${SHADERC_PLATFORM}
            --profile 140
            -i ${BGFX_SHADER_INCLUDE}
            --varyingdef ${SHADER_DIR}/varying.def.sc
            --bin2c ${SHADER}_glsl
        DEPENDS ${SHADER_SRC} ${SHADER_DIR}/varying.def.sc shaderc
        COMMENT "Compiling vertex shader ${SHADER} (GLSL 140)"
    )
    # ESSL 100
    set(ESSL_OUT "${SHADER_OUTPUT_DIR}/${SHADER}_essl.bin.h")
    add_custom_command(
        OUTPUT ${ESSL_OUT}
        COMMAND shaderc
            -f ${SHADER_SRC}
            -o ${ESSL_OUT}
            --type vertex
            --platform android
            --profile 100_es
            -i ${BGFX_SHADER_INCLUDE}
            --varyingdef ${SHADER_DIR}/varying.def.sc
            --bin2c ${SHADER}_essl
        DEPENDS ${SHADER_SRC} ${SHADER_DIR}/varying.def.sc shaderc
        COMMENT "Compiling vertex shader ${SHADER} (ESSL 100)"
    )
    # SPIR-V
    set(SPIRV_OUT "${SHADER_OUTPUT_DIR}/${SHADER}_spv.bin.h")
    add_custom_command(
        OUTPUT ${SPIRV_OUT}
        COMMAND shaderc
            -f ${SHADER_SRC}
            -o ${SPIRV_OUT}
            --type vertex
            --platform ${SHADERC_PLATFORM}
            --profile spirv
            -i ${BGFX_SHADER_INCLUDE}
            --varyingdef ${SHADER_DIR}/varying.def.sc
            --bin2c ${SHADER}_spv
        DEPENDS ${SHADER_SRC} ${SHADER_DIR}/varying.def.sc shaderc
        COMMENT "Compiling vertex shader ${SHADER} (SPIR-V)"
    )
    list(APPEND COMPILED_SHADER_HEADERS ${GLSL_OUT} ${ESSL_OUT} ${SPIRV_OUT})
endforeach()

# Compile fragment shaders for each backend
foreach(SHADER ${FS_SHADERS})
    set(SHADER_SRC "${SHADER_DIR}/${SHADER}.sc")
    # GLSL 140
    set(GLSL_OUT "${SHADER_OUTPUT_DIR}/${SHADER}_glsl.bin.h")
    add_custom_command(
        OUTPUT ${GLSL_OUT}
        COMMAND shaderc
            -f ${SHADER_SRC}
            -o ${GLSL_OUT}
            --type fragment
            --platform ${SHADERC_PLATFORM}
            --profile 140
            -i ${BGFX_SHADER_INCLUDE}
            --varyingdef ${SHADER_DIR}/varying.def.sc
            --bin2c ${SHADER}_glsl
        DEPENDS ${SHADER_SRC} ${SHADER_DIR}/varying.def.sc shaderc
        COMMENT "Compiling fragment shader ${SHADER} (GLSL 140)"
    )
    # ESSL 100
    set(ESSL_OUT "${SHADER_OUTPUT_DIR}/${SHADER}_essl.bin.h")
    add_custom_command(
        OUTPUT ${ESSL_OUT}
        COMMAND shaderc
            -f ${SHADER_SRC}
            -o ${ESSL_OUT}
            --type fragment
            --platform android
            --profile 100_es
            -i ${BGFX_SHADER_INCLUDE}
            --varyingdef ${SHADER_DIR}/varying.def.sc
            --bin2c ${SHADER}_essl
        DEPENDS ${SHADER_SRC} ${SHADER_DIR}/varying.def.sc shaderc
        COMMENT "Compiling fragment shader ${SHADER} (ESSL 100)"
    )
    # SPIR-V
    set(SPIRV_OUT "${SHADER_OUTPUT_DIR}/${SHADER}_spv.bin.h")
    add_custom_command(
        OUTPUT ${SPIRV_OUT}
        COMMAND shaderc
            -f ${SHADER_SRC}
            -o ${SPIRV_OUT}
            --type fragment
            --platform ${SHADERC_PLATFORM}
            --profile spirv
            -i ${BGFX_SHADER_INCLUDE}
            --varyingdef ${SHADER_DIR}/varying.def.sc
            --bin2c ${SHADER}_spv
        DEPENDS ${SHADER_SRC} ${SHADER_DIR}/varying.def.sc shaderc
        COMMENT "Compiling fragment shader ${SHADER} (SPIR-V)"
    )
    list(APPEND COMPILED_SHADER_HEADERS ${GLSL_OUT} ${ESSL_OUT} ${SPIRV_OUT})
endforeach()

# Generate a combined header that includes all shader binaries
set(SHADER_COMBINED_HEADER "${SHADER_OUTPUT_DIR}/gx_tev_shaders.h")
set(SHADER_HEADER_CONTENT "/* Auto-generated shader binaries — do not edit */\n#pragma once\n")
foreach(VS ${VS_SHADERS})
    string(APPEND SHADER_HEADER_CONTENT "#include \"${VS}_glsl.bin.h\"\n")
    string(APPEND SHADER_HEADER_CONTENT "#include \"${VS}_essl.bin.h\"\n")
    string(APPEND SHADER_HEADER_CONTENT "#include \"${VS}_spv.bin.h\"\n")
endforeach()
foreach(FS ${FS_SHADERS})
    string(APPEND SHADER_HEADER_CONTENT "#include \"${FS}_glsl.bin.h\"\n")
    string(APPEND SHADER_HEADER_CONTENT "#include \"${FS}_essl.bin.h\"\n")
    string(APPEND SHADER_HEADER_CONTENT "#include \"${FS}_spv.bin.h\"\n")
endforeach()
file(WRITE ${SHADER_COMBINED_HEADER} ${SHADER_HEADER_CONTENT})

# Custom target to compile all shaders
add_custom_target(compile_shaders DEPENDS ${COMPILED_SHADER_HEADERS})

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
    src/pal/pal_milestone.cpp
    src/pal/pal_os_stubs.cpp
    src/pal/pal_gx_stubs.cpp
    src/pal/pal_sdk_stubs.cpp
    src/pal/pal_math_stubs.cpp
    src/pal/pal_game_stubs.cpp
    src/pal/pal_remaining_stubs.cpp
    src/pal/pal_profile_list.cpp
    src/pal/pal_window.cpp
    src/pal/gx/gx_stub_tracker.cpp
    src/pal/gx/gx_fifo.cpp
    src/pal/gx/gx_bgfx.cpp
    src/pal/gx/gx_state.cpp
    src/pal/gx/gx_texture.cpp
    src/pal/gx/gx_tev.cpp
)

# --- REL modules that are dynamically loaded on console but statically linked on PC ---
# Note: f_pc_profile_lst.cpp references hundreds of actor profiles from other RELs.
# Instead of compiling it, we call ModuleProlog directly (which sets g_fpcPf_ProfileList_p)
# via a stub in pal_remaining_stubs.cpp that only includes profiles already compiled.
set(REL_SOURCES
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

target_include_directories(tp-pc PRIVATE ${INCLUDE_DIRS} ${SHADER_OUTPUT_DIR})

# Ensure shaders are compiled before tp-pc
add_dependencies(tp-pc compile_shaders)

# Link bgfx rendering backend and SDL3 platform layer
target_link_libraries(tp-pc PRIVATE bgfx bimg bx SDL3::SDL3-static)

# --- Compiler settings ---
target_compile_definitions(tp-pc PRIVATE
    __REVOLUTION_SDK__
    WIDESCREEN_SUPPORT=1
    $<$<CONFIG:Debug>:DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:DEBUG=0>
)

# Suppress specific warnings for decompilation code
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tp-pc PRIVATE
        -include ${CMAKE_SOURCE_DIR}/include/pal/pal_platform.h
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
# Creates a 'setup_game_data' target that downloads and extracts the disc image.
# Run: cmake --build build --target setup_game_data
# Or:  ninja -C build setup_game_data
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
    # Check if data/ exists; if not, remind the user
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/data/res")
        message(STATUS "PC port: Game data not found in data/ — run 'ninja setup_game_data' to download")
    else()
        message(STATUS "PC port: Game data found in data/")
    endif()
elseif(EXISTS "${CMAKE_SOURCE_DIR}/data/res")
    message(STATUS "PC port: Game data found in data/")
else()
    message(STATUS "PC port: No game data (set ROMS_TOKEN and install gh CLI to auto-download)")
endif()

message(STATUS "PC port: target tp-pc configured (VERSION=${TP_VERSION})")
