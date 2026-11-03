# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

project(openmotion_core
    VERSION 0.1.0
    DESCRIPTION "OpenMotion core engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(nlohmann_json Catch2)

add_library(openmotion_core STATIC
    src/media.cpp
    src/renderer.cpp
    src/image_loader.cpp
    src/keyframe.cpp
    src/compositor.cpp
    src/effects.cpp
    src/project.cpp)

add_library(OpenMotion::openmotion_core ALIAS openmotion_core)

target_include_directories(openmotion_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(openmotion_core
    PRIVATE
        nlohmann_json::nlohmann_json)

target_compile_options(openmotion_core
    PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(openmotion_core
        PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer)
    target_link_options(openmotion_core
        PUBLIC
            -fsanitize=address,undefined)
endif()

include(CTest)

add_executable(om-render
    tools/om_render.cpp)

target_link_libraries(om-render
    PRIVATE
        OpenMotion::openmotion_core)

target_compile_options(om-render
    PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(om-render
        PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer)
    target_link_options(om-render
        PRIVATE
            -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
    add_executable(openmotion_tests
        tests/test_media.cpp
        tests/test_project.cpp
        tests/test_keyframe.cpp
        tests/test_renderer.cpp)

    target_link_libraries(openmotion_tests
        PRIVATE
            OpenMotion::openmotion_core
            Catch2::Catch2WithMain)

    target_compile_definitions(openmotion_tests
        PRIVATE
            OPENMOTION_EXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")

    target_compile_options(openmotion_tests
        PRIVATE
            -Wall -Wextra -Wpedantic -Werror)

    include(Catch)
    catch_discover_tests(openmotion_tests)
endif()
