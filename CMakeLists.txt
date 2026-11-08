cmake_minimum_required(VERSION 3.16)
project(axon CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB AXONC_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/axonc/src/*.cpp)
add_executable(axon ${AXONC_SOURCES})
if(MSVC)
    target_compile_options(axon PRIVATE /utf-8 /W3)
endif()

# `axonc` is the same program; it compiles a single file when given one.
add_custom_command(TARGET axon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:axon> $<TARGET_FILE_DIR:axon>/axonc${CMAKE_EXECUTABLE_SUFFIX})
