cmake_minimum_required(VERSION 3.19)

project(zyrax_vulkan_layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)

add_library(VkLayer_zyrax SHARED
    layer.cpp
    layer.h
)

target_link_libraries(VkLayer_zyrax PRIVATE
    Vulkan::Headers
)

target_compile_options(VkLayer_zyrax PRIVATE
    -Wall
    -Wextra
)

set_target_properties(VkLayer_zyrax PROPERTIES
    OUTPUT_NAME "VkLayer_zyrax"
    PREFIX "lib"
)
