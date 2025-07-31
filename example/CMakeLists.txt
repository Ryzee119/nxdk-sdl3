cmake_minimum_required(VERSION 3.25)
set(XBE_TITLE sdl3-test)
set(XBOX_ISO_DIR ${CMAKE_CURRENT_BINARY_DIR}/xiso)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PrebuildNXDK)

project(sdl3-test C)

add_executable(sdl3-test main.c)
target_link_libraries(sdl3-test PRIVATE SDL3::SDL3)
target_link_libraries(sdl3-test PUBLIC SDL3::Headers)

# Post build steps to copy assets, generate XBE, and XISO
include (Postbuild)
