cmake_minimum_required(VERSION 3.28)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(flying-rat
    VERSION 0.1.0
    DESCRIPTION "Procedural Maze Explorer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

find_package(glfw3 3.3 REQUIRED)

add_executable(flying-rat)

target_sources(flying-rat
    PRIVATE
        src/main.cpp)

target_sources(flying-rat
    PRIVATE
        FILE_SET CXX_MODULES
        FILES
            src/app.cppm)

target_compile_features(flying-rat PRIVATE cxx_std_23)

target_link_libraries(flying-rat PRIVATE glfw)
