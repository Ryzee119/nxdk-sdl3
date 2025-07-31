CONFIG_SDL_PATH := $(realpath $(lastword $(MAKEFILE_LIST)))
CONFIG_SDL_DIR := $(dir $(CONFIG_SDL_PATH))

SDL3_DIR = $(CONFIG_SDL_DIR)SDL
SDL3_GLUE_DIR = $(CONFIG_SDL_DIR)nxdk_glue

# Core files
SDL3_SRCS := \
	$(wildcard $(SDL3_DIR)/src/*.c) \
	$(wildcard $(SDL3_DIR)/src/atomic/*.c) \
	$(wildcard $(SDL3_DIR)/src/audio/*.c) \
	$(wildcard $(SDL3_DIR)/src/camera/*.c) \
	$(wildcard $(SDL3_DIR)/src/core/*.c) \
	$(wildcard $(SDL3_DIR)/src/cpuinfo/*.c) \
	$(wildcard $(SDL3_DIR)/src/dialog/*.c) \
	$(wildcard $(SDL3_DIR)/src/dynapi/*.c) \
	$(wildcard $(SDL3_DIR)/src/events/*.c) \
	$(wildcard $(SDL3_DIR)/src/filesystem/*.c) \
	$(wildcard $(SDL3_DIR)/src/gpu/*.c) \
	$(wildcard $(SDL3_DIR)/src/haptic/*.c) \
	$(wildcard $(SDL3_DIR)/src/hidapi/*.c) \
	$(filter-out $(SDL3_DIR)/src/io/SDL_iostream.c, \
            $(wildcard $(SDL3_DIR)/src/io/*.c) \
			$(SDL3_GLUE_DIR)/io/SDL_iostream.c) \
	$(wildcard $(SDL3_DIR)/src/io/generic/*.c) \
	$(filter-out $(SDL3_DIR)/src/joystick/SDL_steam_virtual_gamepad.c, \
              $(wildcard $(SDL3_DIR)/src/joystick/*.c)) \
	$(wildcard $(SDL3_DIR)/src/libm/*.c) \
	$(wildcard $(SDL3_DIR)/src/loadso/*.c) \
	$(wildcard $(SDL3_DIR)/src/locale/*.c) \
	$(wildcard $(SDL3_DIR)/src/main/*.c) \
	$(wildcard $(SDL3_DIR)/src/misc/*.c) \
	$(wildcard $(SDL3_DIR)/src/power/*.c) \
	$(wildcard $(SDL3_DIR)/src/process/*.c) \
	$(wildcard $(SDL3_DIR)/src/render/*.c) \
	$(wildcard $(SDL3_DIR)/src/sensor/*.c) \
	$(wildcard $(SDL3_DIR)/src/stdlib/*.c) \
	$(wildcard $(SDL3_DIR)/src/storage/*.c) \
	$(wildcard ${SDL3_DIR}/src/test/*.c) \
	$(wildcard $(SDL3_DIR)/src/thread/*.c) \
	$(wildcard $(SDL3_DIR)/src/time/*.c) \
	$(wildcard $(SDL3_DIR)/src/timer/*.c) \
	$(wildcard $(SDL3_DIR)/src/tray/*.c) \
	$(wildcard $(SDL3_DIR)/src/video/*.c) \
	$(wildcard $(SDL3_DIR)/src/video/yuv2rgb/*.c)

# Platform Drivers
SDL3_SRCS += \
	$(wildcard $(SDL3_GLUE_DIR)/audio/*.c) \
	$(wildcard $(SDL3_DIR)/src/camera/dummy/*.c) \
	$(wildcard $(SDL3_DIR)/src/dialog/dummy/*.c) \
	$(wildcard $(SDL3_GLUE_DIR)/filesystem/*.c) \
	$(wildcard $(SDL3_DIR)/src/haptic/dummy/*.c) \
	$(wildcard $(SDL3_GLUE_DIR)/joystick/*.c) \
	$(wildcard $(SDL3_DIR)/src/loadso/dummy/*.c) \
	$(wildcard $(SDL3_DIR)/src/locale/dummy/*.c) \
	$(wildcard $(SDL3_DIR)/src/main/generic/*.c) \
	$(wildcard $(SDL3_DIR)/src/misc/dummy/*.c) \
	$(wildcard $(SDL3_DIR)/src/process/dummy/*.c) \
	$(wildcard $(SDL3_DIR)/src/render/software/*.c) \
	$(wildcard $(SDL3_GLUE_DIR)/render/*.c) \
	$(wildcard $(SDL3_DIR)/src/sensor/dummy/*.c) \
	$(wildcard $(SDL3_DIR)/src/storage/generic/*.c) \
	$(filter-out $(SDL3_DIR)/src/thread/windows/SDL_systhread.c, \
			  $(wildcard $(SDL3_DIR)/src/thread/windows/*.c) \
			  $(SDL3_GLUE_DIR)/thread/SDL_systhread.c \
			  ${SDL3_DIR}/src/thread/generic/SDL_syscond.c \
			  ${SDL3_DIR}/src/thread/generic/SDL_sysrwlock.c) \
	$(SDL3_GLUE_DIR)/time/SDL_systime.c \
	$(SDL3_GLUE_DIR)/timer/SDL_systimer.c \
	$(wildcard $(SDL3_DIR)/src/tray/dummy/*.c) \
	$(wildcard $(SDL3_GLUE_DIR)/video/*.c) \

SDL3_SRCS += \
	$(SDL3_GLUE_DIR)/stubs.c $(SDL3_GLUE_DIR)/helper.c 

SDL3_FLAGS = -I$(SDL3_GLUE_DIR) -I$(SDL3_DIR)/include -I$(SDL3_DIR)/src
SDL3_FLAGS += -DSDL_DISABLE_ALLOCA -DSDL_DISABLE_ANALYZE_MACROS -DSTBI_NO_SIMD -DSDL_DISABLE_MMX -DSDL_platform_defines_h_
SDL3_FLAGS += -Wno-microsoft-include

SDL3_OBJS = $(addsuffix .obj, $(basename $(SDL3_SRCS)))
libSDL3.lib: $(SDL3_OBJS)