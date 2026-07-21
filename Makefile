TARGET = xmb_gamecache
OBJS = main.o hooks.o gcache.o piso.o mru.o imports.o

CFLAGS = -O2 -G0 -Wall -fno-pic -fno-builtin
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

PSP_FW_VERSION = 660

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
