cmake_minimum_required(VERSION 3.25)
set(XBE_TITLE sdl3-test)
set(XBOX_ISO_DIR ${CMAKE_CURRENT_BINARY_DIR}/xiso)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PrebuildNXDK)
include(FetchContent)

project(sdl3-test C)

add_executable(sdl3-test main.c)

# Not used in this example but pulled in and built for testing purposes
message(STATUS "Fetching SDL_image...")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  SDL3_image
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
  GIT_TAG "release-3.2.4"
)
FetchContent_MakeAvailable(SDL3_image)
target_link_libraries(sdl3-test PRIVATE SDL3::SDL3 SDL3_image::SDL3_image)

message(STATUS "Fetching SDL_ttf...")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE) # Doesn't compile cleanly with nxdk
set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE) # Doesn't compile cleanly with nxdk
FetchContent_Declare(
  SDL3_ttf
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
  GIT_TAG "release-3.2.2"
)
FetchContent_MakeAvailable(SDL3_ttf)
target_link_libraries(sdl3-test PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf)

message(STATUS "Fetching SDL_mixer...")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(VITA 1) # Big ol' hack to disable shared libs
set(SDLMIXER_VENDORED ON CACHE BOOL "" FORCE)

# The below are not compiled cleanly with nxdk
set(SDLMIXER_OPUS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_VORBISFILE OFF CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC_LIBFLAC OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MOD_XMP OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MP3_MPG123 OFF CACHE BOOL "" FORCE)
set(SDLMIXER_WAVPACK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  SDL3_mixer
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
  GIT_TAG main # No release tag yet for SDL3 (FIXME when available)
)
FetchContent_MakeAvailable(SDL3_mixer)
target_link_libraries(sdl3-test PRIVATE SDL3::SDL3 SDL3_mixer::SDL3_mixer)


target_link_libraries(sdl3-test PRIVATE SDL3::SDL3)
target_link_libraries(sdl3-test PUBLIC SDL3::Headers)

# Post build steps to copy assets, generate XBE, and XISO
include (Postbuild)
