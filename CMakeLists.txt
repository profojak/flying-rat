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

# Slang ships no CMake package, wrap it.
find_path(SLANG_INCLUDE_DIR NAMES slang.h REQUIRED)
find_library(SLANG_LIBRARY NAMES slang REQUIRED)
add_library(Slang::Slang UNKNOWN IMPORTED)
set_target_properties(Slang::Slang PROPERTIES
    IMPORTED_LOCATION "${SLANG_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SLANG_INCLUDE_DIR}")

add_executable(flying-rat)

target_sources(flying-rat
    PRIVATE
        src/main.cpp
    PRIVATE
        FILE_SET CXX_MODULES
        FILES
            src/app.cppm)

target_link_libraries(flying-rat PRIVATE
    glfw
    Vulkan::Vulkan
    glm::glm
    Slang::Slang)
