cmake_minimum_required(VERSION 3.16)
project(SDL3Wrapper LANGUAGES C)

# === Directory Setup ===
set(SDL3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SDL)
set(SDL3_GLUE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/nxdk_glue)

# === Compile Definitions ===
set(SDL3_COMPILE_DEFS
    SDL_DISABLE_ALLOCA
    SDL_DISABLE_ANALYZE_MACROS
    STBI_NO_SIMD
    SDL_DISABLE_MMX
    SDL_platform_defines_h_
)

# === Helper macro to gather filtered sources ===
macro(glob_remove VAR GLOB_PATTERN)
    file(GLOB TEMP_LIST ${GLOB_PATTERN})
    list(REMOVE_ITEM TEMP_LIST ${ARGN})
    set(${VAR} ${TEMP_LIST})
endmacro()

# === Core Sources ===
file(GLOB SDL3_CORE_SRCS
    ${SDL3_DIR}/src/*.c
    ${SDL3_DIR}/src/atomic/*.c
    ${SDL3_DIR}/src/audio/*.c
    ${SDL3_DIR}/src/camera/*.c
    ${SDL3_DIR}/src/core/*.c
    ${SDL3_DIR}/src/cpuinfo/*.c
    ${SDL3_DIR}/src/dialog/*.c
    ${SDL3_DIR}/src/dynapi/*.c
    ${SDL3_DIR}/src/events/*.c
    ${SDL3_DIR}/src/filesystem/*.c
    ${SDL3_DIR}/src/gpu/*.c
    ${SDL3_DIR}/src/haptic/*.c
    ${SDL3_DIR}/src/hidapi/*.c
    ${SDL3_DIR}/src/io/*.c
    ${SDL3_DIR}/src/io/generic/*.c
    ${SDL3_DIR}/src/joystick/*.c
    ${SDL3_DIR}/src/libm/*.c
    ${SDL3_DIR}/src/loadso/*.c
    ${SDL3_DIR}/src/locale/*.c
    ${SDL3_DIR}/src/main/*.c
    ${SDL3_DIR}/src/misc/*.c
    ${SDL3_DIR}/src/power/*.c
    ${SDL3_DIR}/src/process/*.c
    ${SDL3_DIR}/src/render/*.c
    ${SDL3_DIR}/src/sensor/*.c
    ${SDL3_DIR}/src/stdlib/*.c
    ${SDL3_DIR}/src/storage/*.c
    ${SDL3_DIR}/src/test/*.c
    ${SDL3_DIR}/src/thread/*.c
    ${SDL3_DIR}/src/time/*.c
    ${SDL3_DIR}/src/timer/*.c
    ${SDL3_DIR}/src/tray/*.c
    ${SDL3_DIR}/src/video/*.c
    ${SDL3_DIR}/src/video/yuv2rgb/*.c
)

# === Remove/replaced files ===
list(REMOVE_ITEM SDL3_CORE_SRCS
    ${SDL3_DIR}/src/io/SDL_iostream.c
    ${SDL3_DIR}/src/joystick/SDL_steam_virtual_gamepad.c
    ${SDL3_DIR}/src/thread/windows/SDL_systhread.c
)

# Replace with glue version
list(APPEND SDL3_CORE_SRCS
    ${SDL3_GLUE_DIR}/io/SDL_iostream.c
    ${SDL3_GLUE_DIR}/thread/SDL_systhread.c
    ${SDL3_GLUE_DIR}/time/SDL_systime.c
    ${SDL3_GLUE_DIR}/timer/SDL_systimer.c
    ${SDL3_GLUE_DIR}/stubs.c
    ${SDL3_GLUE_DIR}/helper.c
)

# === Platform-specific Glue Sources ===
file(GLOB SDL3_PLATFORM_SRCS
    ${SDL3_GLUE_DIR}/audio/*.c
    ${SDL3_GLUE_DIR}/filesystem/*.c
    ${SDL3_GLUE_DIR}/joystick/*.c
    ${SDL3_GLUE_DIR}/render/*.c
    ${SDL3_GLUE_DIR}/video/*.c
    ${SDL3_DIR}/src/main/generic/*.c
    ${SDL3_DIR}/src/render/software/*.c
    ${SDL3_DIR}/src/storage/generic/*.c
    ${SDL3_DIR}/src/thread/windows/*.c
    ${SDL3_DIR}/src/thread/generic/SDL_syscond.c
    ${SDL3_DIR}/src/thread/generic/SDL_sysrwlock.c

    ${SDL3_DIR}/src/camera/dummy/*.c
    ${SDL3_DIR}/src/dialog/dummy/*.c
    ${SDL3_DIR}/src/haptic/dummy/*.c
    ${SDL3_DIR}/src/loadso/dummy/*.c
    ${SDL3_DIR}/src/locale/dummy/*.c
    ${SDL3_DIR}/src/misc/dummy/*.c
    ${SDL3_DIR}/src/process/dummy/*.c
    ${SDL3_DIR}/src/sensor/dummy/*.c
    ${SDL3_DIR}/src/tray/dummy/*.c
)

list(REMOVE_ITEM SDL3_PLATFORM_SRCS
    ${SDL3_DIR}/src/thread/windows/SDL_systhread.c
)

# === Create SDL3 Library ===
add_library(SDL3-static STATIC ${SDL3_CORE_SRCS} ${SDL3_PLATFORM_SRCS})
add_library(SDL3::SDL3-static ALIAS SDL3-static)
add_library(SDL3::SDL3 ALIAS SDL3-static)
target_include_directories(SDL3-static PRIVATE ${SDL3_DIR}/include)
target_include_directories(SDL3-static PRIVATE ${SDL3_DIR}/src ${SDL3_GLUE_DIR})
target_compile_definitions(SDL3-static PRIVATE ${SDL3_COMPILE_DEFS})

# === Create SDL3 Headers Interface ===
add_library(SDL3_Headers INTERFACE)
add_library(SDL3::Headers ALIAS SDL3_Headers)
target_include_directories(SDL3_Headers INTERFACE ${SDL3_DIR}/include)
target_compile_definitions(SDL3_Headers INTERFACE ${SDL3_COMPILE_DEFS})

# === Link SDL3 Dependencies ===
target_link_libraries(SDL3-static PUBLIC SDL3::Headers)

# Link usb driver for input
set(USB_INCLUDE_DIRS ${NXDK_DIR}/lib/usb/libusbohci/inc ${NXDK_DIR}/lib/usb/libusbohci_xbox)
set(USB_DEFINES USBH_USE_EXTERNAL_CONFIG=\"usbh_config_xbox.h\")
target_include_directories(SDL3-static PRIVATE ${USB_INCLUDE_DIRS})
target_compile_definitions(SDL3-static PRIVATE ${USB_DEFINES})
target_link_libraries(SDL3-static PUBLIC ${NXDK_DIR}/lib/nxdk_usb.lib)

# Link pbkit for graphics
target_link_libraries(SDL3-static PUBLIC ${NXDK_DIR}/lib/libpbkit.lib)

# Link automount support for BasePath D:\\
target_link_libraries(SDL3-static PUBLIC ${NXDK_DIR}/lib/libnxdk_automount_d.lib)
target_link_options(SDL3-static PRIVATE "-include:_automount_d_drive")

# Only build examples if enabled (default OFF)
option(SDL3WRAPPER_BUILD_EXAMPLE "Build example programs" OFF)

if(SDL3WRAPPER_BUILD_EXAMPLE)
    add_subdirectory(example)
endif()
