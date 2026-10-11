# USF Maker - build for GCC/Clang (Linux, macOS, MinGW).
# The supported way to build on Windows is the Visual Studio solution
# (USFMaker.sln); this Makefile is mainly meant for development and testing.
#
#   make            -> build/usfmaker-cli (the command line tool)
#   make CROSS=x86_64-w64-mingw32- EXE=.exe BUILD=build-win
#                   -> Windows command line tool and USFMaker.exe with MinGW-w64

CROSS ?=
# MinGW comes in two thread flavours on some distributions; std::thread
# needs the "posix" one: make CROSS=x86_64-w64-mingw32- SUFFIX=-posix EXE=.exe
SUFFIX ?=
CC  = $(CROSS)gcc$(SUFFIX)
CXX = $(CROSS)g++$(SUFFIX)
AR  = $(CROSS)ar
EXE ?=
OPT ?= -O2
BUILD ?= build

include src/lazyusf2/sources.mk

CORE_OBJS   = $(patsubst %.c,$(BUILD)/core/%.o,$(LAZYUSF2_SRCS))
ENGINE_SRCS = $(wildcard src/engine/*.cpp)
ENGINE_OBJS = $(patsubst src/engine/%.cpp,$(BUILD)/engine/%.o,$(ENGINE_SRCS))
CLI_OBJS    = $(BUILD)/cli/main.o

# The core relies on type punning all over the place.
CORE_CFLAGS = $(OPT) -g -fno-strict-aliasing -w -DARCH_MIN_SSE2 -Isrc/lazyusf2
CXXFLAGS    = $(OPT) -g -std=c++17 -Wall -Wextra -Isrc
LDLIBS      = -lm -lpthread
ifneq ($(CROSS),)
LDFLAGS    += -static -municode
endif

TARGETS = $(BUILD)/usfmaker-cli$(EXE)

# The graphical front end is Win32 only.
ifneq ($(findstring mingw,$(CROSS)),)
WINDRES   = $(CROSS)windres
GUI_SRCS  = $(wildcard src/gui/*.cpp)
GUI_OBJS  = $(patsubst src/gui/%.cpp,$(BUILD)/gui/%.o,$(GUI_SRCS)) $(BUILD)/gui/resources.o
TARGETS  += $(BUILD)/USFMaker.exe
endif

all: $(TARGETS)

# -MMD/-MP: the core keeps its whole state in one struct that nearly every
# file includes, so objects must be rebuilt whenever a header changes.
$(BUILD)/core/%.o: src/lazyusf2/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/engine/%.o: src/engine/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/cli/%.o: src/cli/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/gui/%.o: src/gui/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DUNICODE -D_UNICODE -MMD -MP -c -o $@ $<

$(BUILD)/gui/resources.o: src/gui/USFMaker.rc src/gui/USFMaker.manifest src/gui/USFMaker.ico src/gui/resource.h
	@mkdir -p $(dir $@)
	$(WINDRES) -Isrc/gui -O coff -o $@ $<

$(BUILD)/USFMaker.exe: $(GUI_OBJS) $(BUILD)/libusfengine.a $(BUILD)/liblazyusf2.a
	$(CXX) $(LDFLAGS) -mwindows -o $@ $^ $(LDLIBS) -lcomctl32 -lcomdlg32 -lole32 -lshell32 -luuid -lwinmm

$(BUILD)/liblazyusf2.a: $(CORE_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/libusfengine.a: $(ENGINE_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/usfmaker-cli$(EXE): $(CLI_OBJS) $(BUILD)/libusfengine.a $(BUILD)/liblazyusf2.a
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -rf $(BUILD)

-include $(CORE_OBJS:.o=.d) $(ENGINE_OBJS:.o=.d) $(CLI_OBJS:.o=.d) $(GUI_OBJS:.o=.d)

.PHONY: all clean
