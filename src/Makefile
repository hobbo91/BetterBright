TARGET = BetterBright
OBJS = main.o conf.o memory.o osd.o log.o

# ----------------------------------------------------------------------------
# Path to your ARK-4 source checkout. It must contain:
#   common/include/module2.h                           (SceModule2 struct)
#   libs/SystemCtrlForKernel/libpspsystemctrl_kernel.a (built once, see BUILD.md)
#
# Override on the command line if you don't want to edit this file:
#   make ARKROOT=/path/to/ARK-4
# ----------------------------------------------------------------------------
ARKROOT ?= $(HOME)/ARK-4

USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

BUILD_PRX = 1
PRX_EXPORTS = exports.exp
PSP_FW_VERSION = 660

# INCDIR / LIBDIR are plain directory lists here; build_prx.mak adds -I / -L.
INCDIR = $(ARKROOT)/common/include
LIBDIR = $(ARKROOT)/libs/SystemCtrlForKernel

# -fno-pic and -mno-check-zero-division are REQUIRED on the modern GCC toolchain
# for kernel PRX modules (the old 2011 Makefile didn't need them).
CFLAGS   = -O2 -G0 -Wall -fno-pic -fno-builtin-printf -mno-check-zero-division
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

# With USE_KERNEL_LIBS=1, build_prx.mak already appends:
#   -nostdlib -lpspdebug -lpspdisplay_driver -lpspctrl_driver -lpspmodinfo -lpspsdk -lpspkernel
# That covers sceDisplaySetBrightness/Get, sceCtrlPeekBufferPositive, the kernel
# string functions, threads, file I/O, etc. So we add:
#   -lpspsystemctrl_kernel : sctrlHENSetStartModuleHandler
#   -lpsppower_driver      : scePowerTick (kernel) - keep_display_on
# Do NOT add -lc / -lm / -lpspuser here: in a kernel PRX they pull in the full
# user newlib (stdio/malloc) whose lock/syscall stubs aren't provided -> link fails.
LIBS = -lpspsystemctrl_kernel -lpsppower_driver

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
