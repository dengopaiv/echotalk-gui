# EchoTalk build system
#
# Targets:
#   make native   -- build for this machine (Linux/Mac), dynamically
#                     linked, for development and testing only.
#   make win64    -- static 64-bit Windows build.
#   make win32    -- static 32-bit Windows build.
#   make windows  -- both win64 and win32.
#   make test     -- pure-logic unit tests, no ROMs needed.
#   make clean
#
# Shared library, which is what a host like NVDA loads:
#   make win64-dll / win32-dll / dll  -- echotalk.dll plus its import
#                     library, self-contained, no MinGW runtime.
#   make so       -- Linux/macOS libechotalk.so. UNTESTED: the machine
#                     this was developed on is Windows.
#   make test-dll -- load the 64-bit DLL from Python via ctypes, as a
#                     screen reader would, and check the whole surface.
#   make test-dll-load -- the same checks from C via GetProcAddress, for
#                     both DLLs. This is how the 32-bit one gets tested
#                     at all when the only Python present is 64-bit.
#
# --- Windows builds: which environment to run this from ---
#
# Short version: on MSYS2, any shell works for either target. Run
# `make win64` or `make win32` and the right compiler gets selected and
# then verified. If the wrong one somehow gets used, the build stops
# with an explanation instead of producing a mislabeled binary.
#
# The two Windows targets deliberately link different C runtimes:
#
#   win64 -> UCRT (the newer Universal C Runtime), via MSYS2's UCRT64
#            toolchain. UCRT ships with Windows 10 and later.
#   win32 -> MSVCRT (the older runtime), via MSYS2's MINGW32 toolchain.
#            MSYS2 offers no UCRT-based 32-bit environment, so 32-bit
#            necessarily means MSVCRT. That is a feature here rather
#            than a limitation: MSVCRT ships on every Windows version
#            including the older ones a 32-bit NVDA build is most
#            likely to run on.
#
# Install whichever you're missing with:
#   pacman -S mingw-w64-ucrt-x86_64-gcc     (for win64)
#   pacman -S mingw-w64-i686-gcc            (for win32)
#
# Both targets can ALSO be cross-compiled from Linux, using the
# standard x86_64-w64-mingw32-gcc / i686-w64-mingw32-gcc cross
# compilers (`apt-get install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686`
# on Debian/Ubuntu). Override the compiler explicitly at any time:
#   make win64 CC_WIN64=x86_64-w64-mingw32-gcc
#
# Both Windows builds are static (-static): no dependency on any
# MinGW runtime DLL (libgcc_s, libwinpthread, etc.) that wouldn't be
# present on a target machine. They depend only on core Windows system
# DLLs (kernel32.dll and either ucrtbase.dll or msvcrt.dll, both of
# which ship with Windows itself) -- verified by inspecting import
# tables and by actually running both builds under Wine.

CFLAGS_COMMON = -Wall -O2 -Ithird_party/tms5220_core -Itools -Isrc

EMU_SOURCES = third_party/fake6502/fake6502.c \
              third_party/tms5220_core/tms5220_core.c \
              third_party/tms5220_core/tms5220_reset.c \
              src/text_prep.c \
              src/chunker.c

# The canonical v3.1.3 harness: boots via Textalker's own loader.
SOURCES_LOADER = tools/render_text_loader.c $(EMU_SOURCES)
# Superseded direct-init harness ($D003/$FCD6, no loader), kept for A/B
# comparison -- this is the one that shows the onset glitch.
SOURCES = tools/render_text_real_chip.c $(EMU_SOURCES)
SOURCES_V13 = tools/render_v13.c $(EMU_SOURCES)

SOURCES_RESAMPLE = tools/resample_wav.c
SOURCES_WAVSTATS = tools/wav_stats.c

# The library and the tool that drives it. This is the thing anyone
# else is actually meant to run.
LIB_SOURCES = src/echotalk.c src/text_prep.c src/chunker.c src/resample.c \
              third_party/fake6502/fake6502.c \
              third_party/tms5220_core/tms5220_core.c \
              third_party/tms5220_core/tms5220_reset.c
SOURCES_SAY = tools/say.c $(LIB_SOURCES)

# Pure-logic unit tests, no emulator involved.
SOURCES_TEST_PREP = tools/test_text_prep.c src/text_prep.c
SOURCES_TEST_CHUNKER = tools/test_chunker.c src/chunker.c

CC_NATIVE ?= gcc
PYTHON ?= python

# --- Compiler selection ---
#
# MSYS2 sets MSYSTEM to name which subsystem shell you launched: UCRT64,
# MINGW32, MINGW64, CLANG64, or MSYS. Selecting on its mere PRESENCE
# (which this Makefile used to do) is a trap: `make win32` run from the
# UCRT64 shell would pick up UCRT64's `gcc`, which is a 64-bit compiler,
# and silently write a 64-bit binary into build/win32/. Nothing caught
# it, because `which gcc` succeeds either way.
#
# So: select on MSYSTEM's VALUE, and when the current shell is the wrong
# one, reach for the right compiler by absolute path instead of failing.
# Every MSYS2 shell can see /ucrt64 and /mingw32 regardless of which
# subsystem it was launched as, so this makes both Windows targets build
# correctly from any MSYS2 shell.
#
# Whatever gets selected is then verified against `gcc -dumpmachine`
# below, so a wrong compiler is a hard error rather than a mislabeled
# binary.

ifeq ($(MSYSTEM),)
  # Not in MSYS2: assume Linux/Mac cross-compilation.
  CC_WIN64 ?= x86_64-w64-mingw32-gcc
  CC_WIN32 ?= i686-w64-mingw32-gcc
else
  ifneq ($(filter UCRT64 MINGW64,$(MSYSTEM)),)
    CC_WIN64 ?= gcc
  else
    CC_WIN64 ?= /ucrt64/bin/gcc
  endif
  ifeq ($(MSYSTEM),MINGW32)
    CC_WIN32 ?= gcc
  else
    CC_WIN32 ?= /mingw32/bin/gcc
  endif
endif

BUILD_DIR = build

.PHONY: all native win64 win32 windows test clean check-mingw64 check-mingw32 \
        win64-dll win32-dll dll so test-dll test-dll-load listen

all: native

native: $(BUILD_DIR)/native/say \
        $(BUILD_DIR)/native/render_text_loader \
        $(BUILD_DIR)/native/render_text_real_chip \
        $(BUILD_DIR)/native/render_v13 \
        $(BUILD_DIR)/native/resample_wav \
        $(BUILD_DIR)/native/wav_stats

# A MinGW gcc loads its own support DLLs (libgcc, libisl, libmpc, ...)
# from its own bin directory by way of PATH. If you invoke one MSYS2
# subsystem's gcc from a different subsystem's shell -- which is exactly
# what the compiler selection above now does on purpose -- the OTHER
# subsystem's bin directory sits ahead of it on PATH, holding DLLs with
# identical names but the wrong architecture. gcc then dies on startup
# with no diagnostic at all and just a nonzero exit code.
#
# Putting the selected compiler's own directory first on PATH for the
# duration of the recipe fixes it, and is harmless when the shell
# already matches the target.
# Only prepend when the compiler was named by path; a bare command name
# means the current shell is already the right one, and prepending "./"
# to PATH would be both pointless and untidy.
WIN64_PATH = $(if $(findstring /,$(CC_WIN64)),$(dir $(CC_WIN64)):,)$$PATH
WIN32_PATH = $(if $(findstring /,$(CC_WIN32)),$(dir $(CC_WIN32)):,)$$PATH

win64: check-mingw64
	mkdir -p $(BUILD_DIR)/win64
	PATH="$(WIN64_PATH)" $(CC_WIN64) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win64/say.exe $(SOURCES_SAY)
	PATH="$(WIN64_PATH)" $(CC_WIN64) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win64/render_text_loader.exe $(SOURCES_LOADER)
	PATH="$(WIN64_PATH)" $(CC_WIN64) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win64/render_text_real_chip.exe $(SOURCES)
	PATH="$(WIN64_PATH)" $(CC_WIN64) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win64/render_v13.exe $(SOURCES_V13)
	PATH="$(WIN64_PATH)" $(CC_WIN64) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win64/resample_wav.exe $(SOURCES_RESAMPLE)
	PATH="$(WIN64_PATH)" $(CC_WIN64) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win64/wav_stats.exe $(SOURCES_WAVSTATS)
	@echo "win64 build complete: $(BUILD_DIR)/win64/"

win32: check-mingw32
	mkdir -p $(BUILD_DIR)/win32
	PATH="$(WIN32_PATH)" $(CC_WIN32) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win32/say.exe $(SOURCES_SAY)
	PATH="$(WIN32_PATH)" $(CC_WIN32) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win32/render_text_loader.exe $(SOURCES_LOADER)
	PATH="$(WIN32_PATH)" $(CC_WIN32) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win32/render_text_real_chip.exe $(SOURCES)
	PATH="$(WIN32_PATH)" $(CC_WIN32) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win32/render_v13.exe $(SOURCES_V13)
	PATH="$(WIN32_PATH)" $(CC_WIN32) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win32/resample_wav.exe $(SOURCES_RESAMPLE)
	PATH="$(WIN32_PATH)" $(CC_WIN32) $(CFLAGS_COMMON) -static -o $(BUILD_DIR)/win32/wav_stats.exe $(SOURCES_WAVSTATS)
	@echo "win32 build complete: $(BUILD_DIR)/win32/"

windows: win64 win32

# --- Shared library ---
#
# The DLL is what a host like NVDA actually loads, so it must not drag
# in a runtime the host does not have: -static pulls libgcc and the
# MinGW support DLLs in, leaving only KERNEL32 and the C runtime the
# target subsystem implies (UCRT for win64, MSVCRT for win32). Check
# with `objdump -p echotalk.dll | grep 'DLL Name'` after changing this.
#
# --out-implib produces the import library a C or C++ consumer links
# against; a host loading it dynamically (ctypes, LoadLibrary) does not
# need it, but its absence is the kind of thing that is only noticed
# much later.
DLL_FLAGS = -shared -static -DECHOTALK_BUILD_DLL

win64-dll: check-mingw64
	mkdir -p $(BUILD_DIR)/win64
	PATH="$(WIN64_PATH)" $(CC_WIN64) $(CFLAGS_COMMON) $(DLL_FLAGS) \
	  -o $(BUILD_DIR)/win64/echotalk.dll $(LIB_SOURCES) \
	  -Wl,--out-implib,$(BUILD_DIR)/win64/libechotalk.dll.a
	@echo "win64 DLL: $(BUILD_DIR)/win64/echotalk.dll"

win32-dll: check-mingw32
	mkdir -p $(BUILD_DIR)/win32
	PATH="$(WIN32_PATH)" $(CC_WIN32) $(CFLAGS_COMMON) $(DLL_FLAGS) \
	  -o $(BUILD_DIR)/win32/echotalk.dll $(LIB_SOURCES) \
	  -Wl,--out-implib,$(BUILD_DIR)/win32/libechotalk.dll.a
	@echo "win32 DLL: $(BUILD_DIR)/win32/echotalk.dll"

dll: win64-dll win32-dll

# Linux/macOS shared object. Untested on the Windows box this was
# developed on -- the header's visibility attribute is in place and the
# sources are portable C, but nobody has run it.
so:
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -fvisibility=hidden -fPIC -shared \
	  -DECHOTALK_BUILD_DLL \
	  -o $(BUILD_DIR)/native/libechotalk.so $(LIB_SOURCES)
	@echo "shared object: $(BUILD_DIR)/native/libechotalk.so"

# Loads the DLL through ctypes exactly as a screen reader would and
# checks the whole exported surface. Needs a Python whose bitness
# matches the DLL, and the ROMs.
test-dll: win64-dll
	$(PYTHON) tools/test_dll.py $(BUILD_DIR)/win64/echotalk.dll \
	  roms/textalker.ram.bin roms/textalker.obj.bin

# Writes a WAV in which the speech announces what each section is about
# to demonstrate, so it can be checked by ear straight through. Also the
# way to check a Linux build, since it only needs ctypes:
#   make so && make listen ECHOTALK_LIB=build/native/libechotalk.so
ECHOTALK_LIB ?= $(BUILD_DIR)/win64/echotalk.dll

listen:
	$(PYTHON) tools/listen_check.py $(ECHOTALK_LIB) \
	  roms/textalker.ram.bin roms/textalker.obj.bin listen.wav

# The same checks from C, resolving every export through GetProcAddress
# rather than linking. This is how the 32-bit DLL gets verified at all
# on a machine whose only Python is 64-bit -- a bitness mismatch is
# refused at load time, so ctypes cannot reach it.
test-dll-load: win64-dll win32-dll
	PATH="$(WIN64_PATH)" $(CC_WIN64) $(CFLAGS_COMMON) -o \
	  $(BUILD_DIR)/win64/test_dll_load.exe tools/test_dll_load.c
	PATH="$(WIN32_PATH)" $(CC_WIN32) $(CFLAGS_COMMON) -o \
	  $(BUILD_DIR)/win32/test_dll_load.exe tools/test_dll_load.c
	$(BUILD_DIR)/win64/test_dll_load.exe $(BUILD_DIR)/win64/echotalk.dll \
	  roms/textalker.ram.bin roms/textalker.obj.bin
	$(BUILD_DIR)/win32/test_dll_load.exe $(BUILD_DIR)/win32/echotalk.dll \
	  roms/textalker.ram.bin roms/textalker.obj.bin

# Both checks verify the selected compiler EXISTS and actually targets
# the architecture the target name promises, by asking it directly via
# -dumpmachine (e.g. "x86_64-w64-mingw32" or "i686-w64-mingw32"). This
# is what prevents a 64-bit compiler from quietly producing build/win32/
# contents, which is exactly what the old presence-only check allowed.

check-mingw64:
	@command -v $(CC_WIN64) > /dev/null 2>&1 || \
	  (echo "error: 64-bit compiler '$(CC_WIN64)' not found."; \
	   echo "  If cross-compiling from Linux: apt-get install gcc-mingw-w64-x86-64"; \
	   echo "  If on Windows: install it with 'pacman -S mingw-w64-ucrt-x86_64-gcc'"; \
	   echo "  (any MSYS2 shell will do -- this Makefile finds /ucrt64/bin/gcc itself)."; \
	   exit 1)
	@target=`PATH="$(WIN64_PATH)" $(CC_WIN64) -dumpmachine`; \
	 case "$$target" in \
	   x86_64-*) echo "win64: using $(CC_WIN64) (target $$target)" ;; \
	   *) echo "error: '$(CC_WIN64)' targets $$target, which is not 64-bit."; \
	      echo "  Refusing to write a non-64-bit binary into $(BUILD_DIR)/win64/."; \
	      echo "  Override explicitly, e.g. make win64 CC_WIN64=/ucrt64/bin/gcc"; \
	      exit 1 ;; \
	 esac

check-mingw32:
	@command -v $(CC_WIN32) > /dev/null 2>&1 || \
	  (echo "error: 32-bit compiler '$(CC_WIN32)' not found."; \
	   echo "  If cross-compiling from Linux: apt-get install gcc-mingw-w64-i686"; \
	   echo "  If on Windows: install it with 'pacman -S mingw-w64-i686-gcc'"; \
	   echo "  (any MSYS2 shell will do -- this Makefile finds /mingw32/bin/gcc itself)."; \
	   echo "  Note MSYS2 has no UCRT-based 32-bit environment; 32-bit means MSVCRT."; \
	   exit 1)
	@target=`PATH="$(WIN32_PATH)" $(CC_WIN32) -dumpmachine`; \
	 case "$$target" in \
	   i?86-*) echo "win32: using $(CC_WIN32) (target $$target)" ;; \
	   *) echo "error: '$(CC_WIN32)' targets $$target, which is not 32-bit."; \
	      echo "  This is the classic trap: running 'make win32' from the UCRT64"; \
	      echo "  shell picks up a 64-bit gcc. Refusing to write a 64-bit binary"; \
	      echo "  into $(BUILD_DIR)/win32/."; \
	      echo "  Override explicitly, e.g. make win32 CC_WIN32=/mingw32/bin/gcc"; \
	      exit 1 ;; \
	 esac

$(BUILD_DIR)/native/say: $(SOURCES_SAY) src/echotalk.h
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -o $@ $(SOURCES_SAY)

$(BUILD_DIR)/native/render_text_loader: $(SOURCES_LOADER) tools/render_common.h
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -o $@ $(SOURCES_LOADER)

$(BUILD_DIR)/native/render_text_real_chip: $(SOURCES) tools/render_common.h
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -o $@ $(SOURCES)

$(BUILD_DIR)/native/render_v13: $(SOURCES_V13) tools/render_common.h
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -o $@ $(SOURCES_V13)

$(BUILD_DIR)/native/resample_wav: $(SOURCES_RESAMPLE)
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -o $@ $(SOURCES_RESAMPLE)

$(BUILD_DIR)/native/wav_stats: $(SOURCES_WAVSTATS)
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -o $@ $(SOURCES_WAVSTATS)

$(BUILD_DIR)/native/test_text_prep: $(SOURCES_TEST_PREP)
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -o $@ $(SOURCES_TEST_PREP)

$(BUILD_DIR)/native/test_chunker: $(SOURCES_TEST_CHUNKER)
	mkdir -p $(BUILD_DIR)/native
	$(CC_NATIVE) $(CFLAGS_COMMON) -o $@ $(SOURCES_TEST_CHUNKER)

# Runs the pure-logic unit tests. Does not need any ROM.
test: $(BUILD_DIR)/native/test_text_prep $(BUILD_DIR)/native/test_chunker
	@$(BUILD_DIR)/native/test_text_prep
	@echo
	@$(BUILD_DIR)/native/test_chunker

clean:
	rm -rf $(BUILD_DIR)
