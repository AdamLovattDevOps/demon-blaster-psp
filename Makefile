# Demon Hunters - Full 19 Level Version

TARGET = demon_hunters_full
OBJS = demon_hunters_full.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -ffast-math
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
PSP_EBOOT_TITLE = Demon Hunters Full
PSP_EBOOT_ICON = ICON0.PNG

# Use PSPSDK build system
PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
