# Demon Blaster - 9 Level Version
# GAME_VERSION is the single source of truth - keep in sync with git tag
GAME_VERSION = 0.0.13

TARGET = demon_blaster
OBJS = demon_blaster.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -ffast-math -DGAME_VERSION='"V$(GAME_VERSION)"'
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lpspaudio -lm

# PSP Build Settings (matching working Tutorial example)
BUILD_PRX = 1
PSP_FW_VERSION = 500
PSP_LARGE_MEMORY = 1

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Demon Blaster v$(GAME_VERSION)
PSP_EBOOT_ICON = ICON0.PNG

# Use PSPSDK build system
PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
