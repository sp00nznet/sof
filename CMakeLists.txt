cmake_minimum_required(VERSION 3.20)
project(sof-recomp
    VERSION 0.1.0
    DESCRIPTION "Soldier of Fortune - Static Recompilation"
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(SOF_RENDERER_VULKAN "Build with Vulkan renderer backend" OFF)
option(SOF_RENDERER_GL4 "Build with OpenGL 4.x renderer backend" ON)
option(SOF_BUILD_TOOLS "Build analysis and development tools" ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Platform detection
if(WIN32)
    add_definitions(-DPLATFORM_WINDOWS)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-DNOMINMAX)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
elseif(UNIX AND NOT APPLE)
    add_definitions(-DPLATFORM_LINUX)
elseif(APPLE)
    add_definitions(-DPLATFORM_MACOS)
endif()

# --- Dependencies ---

# SDL2 (required — replaces Win32 windowing, input, timers)
find_package(SDL2 REQUIRED)

# OpenGL (required for renderer)
find_package(OpenGL REQUIRED)

# --- Main Executable ---
# Unified binary: engine + game + renderer + sound (no DLL boundaries)

add_executable(sof
    # Common (shared utilities, math, types)
    src/common/q_shared.c
    src/common/pmove.c

    # Engine core
    src/engine/main.c
    src/engine/common.c
    src/engine/cmd.c
    src/engine/cvar.c
    src/engine/z_zone.c
    src/engine/files.c
    src/engine/sys_sdl.c
    src/engine/cm_trace.c
    src/engine/net_msg.c

    # Client / Input / Console
    src/client/keys.c
    src/client/in_sdl.c
    src/client/console.c
    src/client/cl_input.c

    # Renderer (OpenGL)
    src/renderer/r_main.c
    src/renderer/r_bsp.c
    src/renderer/r_surf.c
    src/renderer/r_image.c
    src/renderer/r_light.c

    # Game module (was gamex86.dll)
    src/game/g_main.c
    src/game/g_spawn.c
    src/game/g_phys.c

    # Server
    src/server/sv_game.c
    src/server/sv_world.c

    # Sound (replaces Defsnd/EAXSnd/A3Dsnd DLLs)
    src/sound/snd_sdl.c

    # GHOUL model/gore system
    src/ghoul/ghoul_main.c
)

target_include_directories(sof PRIVATE
    src/common
    src/engine
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(sof PRIVATE
    ${SDL2_LIBRARIES}
    OpenGL::GL
)

if(WIN32)
    target_link_libraries(sof PRIVATE
        ws2_32      # Modern Winsock2
        winmm       # Timer
    )
elseif(UNIX)
    target_link_libraries(sof PRIVATE
        m           # Math library
        pthread     # Threading
    )
endif()

# --- Subdirectories (to be enabled as subsystems are implemented) ---

# GHOUL damage model system
# add_subdirectory(src/ghoul)

# Renderer
# add_subdirectory(src/renderer)

# Sound system
# add_subdirectory(src/sound)

message(STATUS "")
message(STATUS "=== Soldier of Fortune: Static Recompilation ===")
message(STATUS "Version:          ${PROJECT_VERSION}")
message(STATUS "Platform:         ${CMAKE_SYSTEM_NAME}")
message(STATUS "Compiler:         ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Vulkan renderer:  ${SOF_RENDERER_VULKAN}")
message(STATUS "OpenGL4 renderer: ${SOF_RENDERER_GL4}")
message(STATUS "Build tools:      ${SOF_BUILD_TOOLS}")
message(STATUS "================================================")
message(STATUS "")
