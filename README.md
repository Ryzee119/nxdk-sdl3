# SDL3 implementation for nxdk

## Overview
This is a SDL3 implementation that works with [nxdk](https://github.com/XboxDev/nxdk.git).
![Screenshot1](/.github/image.png?)

## Features
* Fully hardware accelerated renderer
* Input via the joystick and gamecontroller API.
* Sound
* Threads and timers
* Filesystem API
* Time API (System time and local time)

## Not supported
* SDL_gpu.h API

## How to use
### CMake
```
add_subdirectory(path/to/nxdk-sdl3)
target_link_libraries(myapp PRIVATE SDL3::SDL3)
target_link_libraries(myapp PUBLIC SDL3::Headers)
```
or
```
include(FetchContent)
message(STATUS "Fetching nxdk-sdl3...")
FetchContent_Declare(
  nxdk_sdl3
  GIT_REPOSITORY https://github.com/Ryzee119/nxdk-sdl3.git
  GIT_TAG master
)

FetchContent_MakeAvailable(nxdk_sdl3)
target_link_libraries(myapp PRIVATE SDL3::SDL3)
target_link_libraries(myapp PUBLIC SDL3::Headers)
```
### Makefile (nxdk style)
```
...
include $(NXDK_DIR)/Makefile
include path/to/nxdk-sdl3/config_sdl.make

CFLAGS += $(SDL3_FLAGS)
CXXFLAGS += $(SDL3_FLAGS)
main.exe: libSDL3.lib
```

## Other Libraries
I've had success pulling in other SDL libraries with CMake. These are atleast compiling and linking.
Not tested

#### SDL_image
```
message(STATUS "Fetching SDL_image...")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  SDL3_image
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
  GIT_TAG "release-3.2.4"
)
FetchContent_MakeAvailable(SDL3_image)
target_link_libraries(myapp PRIVATE SDL3::SDL3 SDL3_image::SDL3_image)
```

#### SDL_ttf
```
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
target_link_libraries(myapp PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf)
```

#### SDL_mixer
```
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
target_link_libraries(myapp PRIVATE SDL3::SDL3 SDL3_mixer::SDL3_mixer)
```
