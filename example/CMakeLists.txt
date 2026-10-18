cmake_minimum_required(VERSION 3.25)
set(XBE_TITLE sdl3-test)
set(XBOX_ISO_DIR ${CMAKE_CURRENT_BINARY_DIR}/xiso)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Prebuild NXDK dependencies only when cross-compiling for Xbox with NXDK
if(CMAKE_TOOLCHAIN_FILE)
  include(PrebuildNXDK OPTIONAL)
endif()

include(FetchContent)

project(sdl3-test C)

# If SDL3 target is not already provided by parent CMake project, add upstream SDL
if(NOT TARGET SDL3::SDL3)
  set(SDL_TEST_DEFAULT OFF)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../SDL" "${CMAKE_CURRENT_BINARY_DIR}/SDL3-build" EXCLUDE_FROM_ALL)
endif()

add_executable(sdl3-test main.c)

# Copy assets to build directory for native desktop builds
if(NOT CMAKE_TOOLCHAIN_FILE)
  file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/"
       DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/assets")
endif()

# Not used in this example but pulled in and built for testing purposes
message(STATUS "Fetching SDL_image...")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  SDL3_image
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
  GIT_TAG "release-3.4.8"
)
FetchContent_MakeAvailable(SDL3_image)
target_link_libraries(sdl3-test PRIVATE SDL3::SDL3 SDL3_image::SDL3_image)

message(STATUS "Fetching SDL_ttf...")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
if(CMAKE_TOOLCHAIN_FILE)
  set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE) # Doesn't compile cleanly with nxdk
  set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE) # Doesn't compile cleanly with nxdk
endif()
FetchContent_Declare(
  SDL3_ttf
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
  GIT_TAG "release-3.2.2"
)
FetchContent_MakeAvailable(SDL3_ttf)
target_link_libraries(sdl3-test PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf)

message(STATUS "Fetching SDL_mixer...")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(CMAKE_TOOLCHAIN_FILE)
  set(VITA 1) # Big ol' hack to disable shared libs on nxdk
  set(SDLMIXER_VENDORED ON CACHE BOOL "" FORCE)
  # The below are not compiled cleanly with nxdk
  set(SDLMIXER_OPUS OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_VORBIS_VORBISFILE OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_FLAC_LIBFLAC OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_MOD_XMP OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_MP3_MPG123 OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_WAVPACK OFF CACHE BOOL "" FORCE)
  set(SDLMIXER_GME OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(
  SDL3_mixer
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
  GIT_TAG "release-3.2.4"
)
FetchContent_MakeAvailable(SDL3_mixer)
target_link_libraries(sdl3-test PRIVATE SDL3::SDL3 SDL3_mixer::SDL3_mixer)

target_link_libraries(sdl3-test PRIVATE SDL3::SDL3)
if(TARGET SDL3::Headers)
  target_link_libraries(sdl3-test PUBLIC SDL3::Headers)
elseif(TARGET SDL3::SDL3_headers)
  target_link_libraries(sdl3-test PUBLIC SDL3::SDL3_headers)
endif()

# Math library on Unix / Linux platforms
if(UNIX AND NOT APPLE)
  target_link_libraries(sdl3-test PRIVATE m)
endif()

# Post build steps to copy assets, generate XBE, and XISO (NXDK only)
if(CMAKE_TOOLCHAIN_FILE)
  include(Postbuild OPTIONAL)
endif()
