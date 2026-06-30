cmake_minimum_required(VERSION 3.20)
project(MyEngine)

set(CMAKE_CXX_STANDARD 20)

file(GLOB_RECURSE SOURCES "src/*.cpp")
add_executable(MyEngine ${SOURCES})

target_include_directories(MyEngine PRIVATE include)

target_link_libraries(MyEngine PRIVATE
    math_lib
    noise_lib
    worldgen_lib
)

target_include_directories(MyEngine PRIVATE
    libs/SDL3/include
    libs/glm/include
)
target_link_libraries(MyEngine PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/SDL3/lib/SDL3.lib
)