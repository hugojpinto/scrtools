#~~~~   SETTINGS   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~#
# scrtools - ZX Spectrum SCREEN$ utility (part of the LOADZX toolset).
# Single translation unit (scrtools.c) plus the vendored stb_image.h header.
CC?=cc
CFLAGS?=-O3 -Wall -Wextra
OUT=scrtools
OBJ=scrtools.o
#~~~~   PLATFORM DETECTION   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~#
# Common compile-time size optimisations (supported by both gcc and clang).
CFLAGS+=-ffunction-sections -fdata-sections -fno-ident

ifeq ($(OS),Windows_NT)
	# --- Windows ---
	OUT:=$(OUT).exe
	BUILD_TS=$(shell echo %date:~8,2%%date:~3,2%%date:~0,2%%time:~0,2%%time:~3,2%)
	LDFLAGS=-s -fmerge-all-constants -Wl,--gc-sections -static-libgcc
	CLN=del /Q
else
	BUILD_TS=$(shell date +%y%m%d%H%M)
	CLN=rm -f
	UNAME_S:=$(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		# --- macOS (Apple clang / ld) ---
		# The GNU-only flags (--gc-sections, --build-id, -static-libgcc,
		# -fmerge-all-constants) are unsupported; use the ld64 equivalents.
		LDFLAGS=-Wl,-dead_strip
	else
		# --- Linux / other GNU toolchains ---
		LDFLAGS=-s -fmerge-all-constants -Wl,--gc-sections \
		        -Wl,--build-id=none -static-libgcc
	endif
endif

CFLAGS+=-DBUILD_TS="$(BUILD_TS)"

#~~~~   CROSS-COMPILE TOOLCHAINS (override as needed)   ~~~~~~~~~~~~~~~~~~~#
# macOS:   brew install FiloSottile/musl-cross/musl-cross   (Linux target)
#          brew install mingw-w64                           (Windows target)
CC_MAC   ?= cc
CC_LINUX ?= x86_64-linux-musl-gcc
CC_WIN   ?= x86_64-w64-mingw32-gcc
#~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~#

.PHONY: all clean mac linux windows release test

all: $(OUT)

$(OUT): $(OBJ)
	$(info Linking to "$@")
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c stb_image.h
	$(info Compiling "$<" )
	$(CC) $< -c $(CFLAGS)

# macOS universal binary (arm64 + x86_64), built from source in one step.
mac: scrtools.c
	$(info Building macOS universal "scrtools")
	$(CC_MAC) scrtools.c -o scrtools $(CFLAGS) -arch arm64 -arch x86_64 -Wl,-dead_strip

# Linux x86_64, statically linked (musl).
linux: scrtools.c
	$(info Building Linux static "scrtools-linux")
	$(CC_LINUX) scrtools.c -o scrtools-linux $(CFLAGS) -static -s \
		-fmerge-all-constants -Wl,--gc-sections -Wl,--build-id=none

# Windows x86_64, statically linked (mingw).
windows: scrtools.c
	$(info Building Windows static "scrtools.exe")
	$(CC_WIN) scrtools.c -o scrtools.exe $(CFLAGS) -static -s \
		-fmerge-all-constants -Wl,--gc-sections -static-libgcc

# All three release binaries.
release: mac linux windows

# Build natively, then run the regression suite.
test: all
	bash scripts/regression.sh ./$(OUT)

clean:
	$(info Cleaning ..)
	$(CLN) $(OUT)
	$(CLN) $(OBJ)
	$(CLN) scrtools-linux scrtools.exe
