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
        src/vulkan/minimap.cpp
        src/vulkan/pipeline.cpp
        src/vulkan/resources.cpp
        src/vulkan/texture.cpp
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

target_compile_options(flying-rat PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>)

# Compile .slang to SPIR-V.
set(SLANG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
set(SPIRV_DIR ${CMAKE_CURRENT_BINARY_DIR})

target_compile_definitions(flying-rat PRIVATE
    SHADER_DIR="${SPIRV_DIR}"
    DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# Make `data/wall.jpg` available next to the compiled SPIR-V shaders.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/data/wall.jpg
    ${SPIRV_DIR}/wall.jpg COPYONLY)

set(SHADERS wall floor minimap)
set(SHADER_OUTPUTS "")
foreach(name IN LISTS SHADERS)
    foreach(stage IN ITEMS vert frag)
        if(stage STREQUAL vert)
            set(entry VertexMain)
            set(slang_stage vertex)
        else()
            set(entry FragmentMain)
            set(slang_stage fragment)
        endif()
        set(output ${SPIRV_DIR}/${name}.${stage}.spv)
        add_custom_command(
            OUTPUT ${output}
            COMMAND ${SLANGC} ${SLANG_DIR}/${name}.slang
                -target spirv
                -entry ${entry} -stage ${slang_stage}
                -o ${output}
            DEPENDS ${SLANG_DIR}/${name}.slang
            COMMENT "Compiling ${name}.slang (${slang_stage}) to SPIR-V"
            VERBATIM)
        list(APPEND SHADER_OUTPUTS ${output})
    endforeach()
endforeach()

add_custom_target(shaders ALL DEPENDS ${SHADER_OUTPUTS})

add_dependencies(flying-rat shaders)
