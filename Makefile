# EchoTalk build system
#
# Targets:
#   make native   -- build for this machine (Linux/Mac), dynamically
#                     linked, for development and testing only.
#   make win64    -- static 64-bit Windows build.
#   make win32    -- static 32-bit Windows build.
#   make windows  -- both win64 and win32.
#   make clean
#
# --- Windows builds: which environment to run this from ---
#
# win64 wants the UCRT runtime (as opposed to the older MSVCRT). In
# MSYS2 that means running this from the "MSYS2 UCRT64" shell, where
# the shell's own `gcc` already targets ucrt64 -- just run `make
# win64` directly, no special flags needed.
#
# win32 is different: MSYS2 does NOT offer a UCRT-based 32-bit
# environment (UCRT64 is 64-bit only). For a 32-bit build -- needed
# because older NVDA releases are 32-bit processes -- run this from
# the "MSYS2 MinGW32" shell instead, which targets the older MSVCRT
# runtime. MSVCRT ships on every Windows version (including the older
# ones a 32-bit NVDA build is likely to target), so this is fine, just
# a different runtime than win64 links against. `make win32` from that
# shell works the same way.
#
# Both targets can ALSO be cross-compiled directly from Linux (this is
# how this Makefile was actually developed and tested), using the
# standard x86_64-w64-mingw32-gcc / i686-w64-mingw32-gcc cross
# compilers (`apt-get install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686`
# on Debian/Ubuntu). The CC_WIN64 / CC_WIN32 variables below
# auto-detect which case you're in based on whether MSYSTEM is set
# (MSYS2 shells set this automatically); override them explicitly if
# the auto-detection guesses wrong, e.g.:
#   make win64 CC_WIN64=x86_64-w64-mingw32-gcc
#
# Both Windows builds are static (-static): no dependency on any
# MinGW runtime DLL (libgcc_s, libwinpthread, etc.) that wouldn't be
# present on a target machine. They depend only on core Windows system
# DLLs (kernel32.dll and either ucrtbase.dll or msvcrt.dll, both of
# which ship with Windows itself) -- verified by inspecting import
# tables and by actually running both builds under Wine.

CFLAGS_COMMON = -Wall -O2 -Ithird_party/tms5220_core
SOURCES = tools/render_text_real_chip.c \
          third_party/fake6502/fake6502.c \
          third_party/tms5220_core/tms5220_core.c \
          third_party/tms5220_core/tms5220_reset.c

SOURCES_V13 = tools/render_v13.c \
              third_party/fake6502/fake6502.c \
              third_party/tms5220_core/tms5220_core.c \
              third_party/tms5220_core/tms5220_reset.c

SOURCES_RESAMPLE = tools/resample_wav.c

CC_NATIVE ?= gcc
CC_WIN64  ?= $(if $(MSYSTEM),gcc,x86_64-w64-mingw32-gcc)
CC_WIN32  ?= $(if $(MSYSTEM),gcc,i686-w64-mingw32-gcc)

BUILD_DIR = build

.PHONY: all native win64 win32 windows clean check-mingw64 check-mingw32

all: native

native: $(BUILD_DIR)/native/render_text_real_chip $(BUILD_DIR)/native/render_v13 $(BUILD_DIR)/native/resample_wav

win64: check-mingw64
	mkdir -p $(BUILD_DIR)/win64
	$(CC_WIN64) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win64/render_text_real_chip.exe $(SOURCES)
	$(CC_WIN64) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win64/render_v13.exe $(SOURCES_V13)
	$(CC_WIN64) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win64/resample_wav.exe $(SOURCES_RESAMPLE)
	@echo "win64 build complete: $(BUILD_DIR)/win64/"

win32: check-mingw32
	mkdir -p $(BUILD_DIR)/win32
	$(CC_WIN32) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win32/render_text_real_chip.exe $(SOURCES)
	$(CC_WIN32) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win32/render_v13.exe $(SOURCES_V13)
	$(CC_WIN32) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win32/resample_wav.exe $(SOURCES_RESAMPLE)
	@echo "win32 build complete: $(BUILD_DIR)/win32/"

windows: win64 win32

check-mingw64:
	@which $(CC_WIN64) > /dev/null 2>&1 || \
	  (echo "error: '$(CC_WIN64)' not found."; \
	   echo "  If cross-compiling from Linux: apt-get install gcc-mingw-w64-x86-64"; \
	   echo "  If on Windows: run this from the MSYS2 UCRT64 shell."; \
	   exit 1)

check-mingw32:
	@which $(CC_WIN32) > /dev/null 2>&1 || \
	  (echo "error: '$(CC_WIN32)' not found."; \
	   echo "  If cross-compiling from Linux: apt-get install gcc-mingw-w64-i686"; \
	   echo "  If on Windows: run this from the MSYS2 MinGW32 shell (NOT UCRT64 --"; \
	   echo "  MSYS2 does not offer a UCRT-based 32-bit environment)."; \
	   exit 1)

$(BUILD_DIR)/native/render_text_real_chip: $(SOURCES)
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -o $@ $(SOURCES)

$(BUILD_DIR)/native/render_v13: $(SOURCES_V13)
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -o $@ $(SOURCES_V13)

$(BUILD_DIR)/native/resample_wav: $(SOURCES_RESAMPLE)
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -o $@ $(SOURCES_RESAMPLE)

clean:
	rm -rf $(BUILD_DIR)
