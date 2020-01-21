#!/usr/bin/make -f
# Makefile for mod-spi2jack #
# ------------------------- #
# Created by falkTX
#

# ---------------------------------------------------------------------------------------------------------------------
# Base environment vars

CC  ?= $(CROSS_COMPILE)gcc
CXX ?= $(CROSS_COMPILE)g++

PREFIX ?= /usr/local
BINDIR  = $(PREFIX)/bin

# ---------------------------------------------------------------------------------------------------------------------
# Set build and link flags

BASE_FLAGS = -Wall -Wextra -pipe -fPIC -DPIC -pthread
BASE_OPTS  = -O2 -ffast-math -fdata-sections -ffunction-sections
LINK_OPTS  = -fdata-sections -ffunction-sections -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed

ifneq ($(SKIP_STRIPPING),true)
LINK_OPTS += -Wl,--strip-all
endif

ifeq ($(DEBUG),true)
BASE_FLAGS += -DDEBUG -O0 -g
LINK_OPTS   =
else
BASE_FLAGS += -DNDEBUG $(BASE_OPTS) -fvisibility=hidden
CXXFLAGS   += -fvisibility-inlines-hidden
endif

BUILD_C_FLAGS   = $(BASE_FLAGS) -std=gnu99 $(CFLAGS)
BUILD_CXX_FLAGS = $(BASE_FLAGS) -std=gnu++11 $(CXXFLAGS)
LINK_FLAGS      = $(LINK_OPTS) $(LDFLAGS) -Wl,--no-undefined

# ---------------------------------------------------------------------------------------------------------------------
# Get JACK build and link flags

ALSA_CFLAGS = $(shell pkg-config --cflags alsa)
ALSA_LIBS   = $(shell pkg-config --libs alsa)

JACK_CFLAGS = $(shell pkg-config --cflags jack)
JACK_LIBS   = $(shell pkg-config --libs jack)
JACK_LIBDIR = $(shell pkg-config --variable=libdir jack)

# ---------------------------------------------------------------------------------------------------------------------
# Strict test build

ifeq ($(TESTBUILD),true)
BASE_FLAGS += -Werror -Wabi=98 -Wcast-qual -Wclobbered -Wconversion -Wdisabled-optimization
BASE_FLAGS += -Wdouble-promotion -Wfloat-equal -Wlogical-op -Wpointer-arith -Wsign-conversion
BASE_FLAGS += -Wformat=2 -Woverlength-strings
BASE_FLAGS += -Wformat-truncation=2 -Wformat-overflow=2
BASE_FLAGS += -Wstringop-overflow=4 -Wstringop-truncation
BASE_FLAGS += -Wmissing-declarations -Wredundant-decls
BASE_FLAGS += -Wshadow  -Wundef -Wuninitialized -Wunused
BASE_FLAGS += -Wstrict-aliasing -fstrict-aliasing
BASE_FLAGS += -Wstrict-overflow -fstrict-overflow
BASE_FLAGS += -Wduplicated-branches -Wduplicated-cond -Wnull-dereference
CFLAGS     += -Winit-self -Wjump-misses-init -Wmissing-prototypes -Wnested-externs -Wstrict-prototypes -Wwrite-strings
CXXFLAGS   += -Wc++0x-compat -Wc++11-compat
CXXFLAGS   += -Wnon-virtual-dtor -Woverloaded-virtual
CXXFLAGS   += -Wzero-as-null-pointer-constant
ifneq ($(DEBUG),true)
CXXFLAGS   += -Weffc++
endif
endif

# ---------------------------------------------------------------------------------------------------------------------
# Build rules

TARGETS = mod-spi2jack mod-spi2jack.so mod-jack2spi mod-jack2spi.so

all: $(TARGETS)

mod-spi2jack: spi2jack.c
	$(CC) $< $(ALSA_CFLAGS) $(JACK_CFLAGS) $(BUILD_C_FLAGS) $(JACK_LIBS) $(ALSA_LIBS) $(LINK_FLAGS) -lm -o $@

mod-spi2jack.so: spi2jack.c
	$(CC) $< $(ALSA_CFLAGS) $(JACK_CFLAGS) $(BUILD_C_FLAGS) $(JACK_LIBS) $(ALSA_LIBS) $(LINK_FLAGS) -lm -shared -o $@

mod-jack2spi: jack2spi.c
	$(CC) $< $(ALSA_CFLAGS) $(JACK_CFLAGS) $(BUILD_C_FLAGS) $(JACK_LIBS) $(ALSA_LIBS) $(LINK_FLAGS) -o $@

mod-jack2spi.so: jack2spi.c
	$(CC) $< $(ALSA_CFLAGS) $(JACK_CFLAGS) $(BUILD_C_FLAGS) $(JACK_LIBS) $(ALSA_LIBS) $(LINK_FLAGS) -shared -o $@

clean:
	$(RM) $(TARGETS)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 mod-spi2jack mod-jack2spi $(DESTDIR)$(BINDIR)

	install -d $(DESTDIR)$(JACK_LIBDIR)/jack
	install -m 644 mod-spi2jack.so mod-jack2spi.so $(DESTDIR)$(JACK_LIBDIR)/jack/

# ---------------------------------------------------------------------------------------------------------------------
