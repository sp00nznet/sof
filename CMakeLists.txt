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

# Common sources (shared types, math, containers)
# add_subdirectory(src/common)

# Core engine
# add_subdirectory(src/engine)

# GHOUL damage model system
# add_subdirectory(src/ghoul)

# Game logic
# add_subdirectory(src/game)

# Renderer
# add_subdirectory(src/renderer)

# Sound system
# add_subdirectory(src/sound)

# Client
# add_subdirectory(src/client)

# Server
# add_subdirectory(src/server)

# Tools
# if(SOF_BUILD_TOOLS)
#     add_subdirectory(tools)
# endif()

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
