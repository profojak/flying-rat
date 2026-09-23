cmake_minimum_required(VERSION 3.28)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

project(flying-rat
    VERSION 0.1.0
    DESCRIPTION "Procedural Maze Explorer"
    LANGUAGES CXX)

find_package(glfw3 REQUIRED)
find_package(Vulkan REQUIRED)
find_package(glm REQUIRED)
find_program(SLANGC NAMES slangc REQUIRED)

add_executable(flying-rat)

target_sources(flying-rat
    PRIVATE
        src/main.cpp
        src/vulkan/core.cpp
        src/vulkan/device.cpp
        src/vulkan/pipeline.cpp
        src/vulkan/resources.cpp
    PRIVATE
        FILE_SET CXX_MODULES
        FILES
            src/config.cppm
            src/maze.cppm
            src/camera.cppm
            src/player.cppm
            src/renderer.cppm
            src/app.cppm)

target_link_libraries(flying-rat PRIVATE glfw Vulkan::Vulkan glm::glm)

target_compile_definitions(flying-rat PRIVATE SHADER_DIR="${SPIRV_DIR}")

# Compile .slang to SPIR-V.
set(SLANG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
set(SPIRV_DIR ${CMAKE_CURRENT_BINARY_DIR})

add_custom_command(
    OUTPUT ${SPIRV_DIR}/wall.vert.spv
    COMMAND ${SLANGC} ${SLANG_DIR}/wall.slang
        -target spirv
        -entry VertexMain -stage vertex
        -o ${SPIRV_DIR}/wall.vert.spv
    DEPENDS ${SLANG_DIR}/wall.slang
    VERBATIM)

add_custom_command(
    OUTPUT ${SPIRV_DIR}/wall.frag.spv
    COMMAND ${SLANGC} ${SLANG_DIR}/wall.slang
        -target spirv
        -entry FragmentMain -stage fragment
        -o ${SPIRV_DIR}/wall.frag.spv
    DEPENDS ${SLANG_DIR}/wall.slang
    VERBATIM)

add_custom_command(
    OUTPUT ${SPIRV_DIR}/floor.vert.spv
    COMMAND ${SLANGC} ${SLANG_DIR}/floor.slang
        -target spirv
        -entry VertexMain -stage vertex
        -o ${SPIRV_DIR}/floor.vert.spv
    DEPENDS ${SLANG_DIR}/floor.slang
    VERBATIM)

add_custom_command(
    OUTPUT ${SPIRV_DIR}/floor.frag.spv
    COMMAND ${SLANGC} ${SLANG_DIR}/floor.slang
        -target spirv
        -entry FragmentMain -stage fragment
        -o ${SPIRV_DIR}/floor.frag.spv
    DEPENDS ${SLANG_DIR}/floor.slang
    VERBATIM)

add_custom_target(shaders ALL
    DEPENDS
        ${SPIRV_DIR}/wall.vert.spv
        ${SPIRV_DIR}/wall.frag.spv
        ${SPIRV_DIR}/floor.vert.spv
        ${SPIRV_DIR}/floor.frag.spv)

add_dependencies(flying-rat shaders)
