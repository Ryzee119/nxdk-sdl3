XBE_TITLE = sdl3-test
GEN_XISO = $(XBE_TITLE).iso
SRCS = $(CURDIR)/main.c

include $(NXDK_DIR)/Makefile
include $(CURDIR)/../config_sdl.make

CFLAGS += $(SDL3_FLAGS)
CXXFLAGS += $(SDL3_FLAGS)
main.exe: libSDL3.lib
