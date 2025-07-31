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
