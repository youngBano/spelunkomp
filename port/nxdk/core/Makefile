# Native Original Xbox build for DoDonPachi:
# - Zero SDL2 (native XAudio AC'97 DMA + native USB XID gamepad + native MCPX hardware timers)
# - Zero Z80/OKI/FM/SH-3 interpreters (lean M68K native static-recompiled blocks only)
# - Hand-tuned Pentium III i686/MMX/SSE1 assembly kernels (xbox_asm.S)
# - Direct NV2A hardware primitive renderer (nv2a_cave_renderer.h)
XBE_TITLE = spelunkomp-cave68k-core
GEN_XISO = spelunkomp-cave68k-core.iso
OUTPUT_DIR = bin
NXDK_DIR ?= $(CURDIR)/../../../nxdk
CORE_ROOT := $(abspath $(CURDIR)/../../../spelunkomp)
GEN_DIR := $(CORE_ROOT)/work/ddonpach/gen
NXDK_CXX = y

# Link statically-recompiled native x86 68000 blocks when present.
GEN_SRCS := $(sort $(wildcard $(GEN_DIR)/native_*.cpp))
ifeq ($(strip $(GEN_SRCS)),)
  BLOCKS68K_SRCS := $(CORE_ROOT)/runtime/no_blocks68k.cpp
else
  BLOCKS68K_SRCS := $(GEN_SRCS)
endif

# Only DoDonPachi hardware (M68K runtime + Cave68k + CaveVideo latch + YMZ280B + EEPROM93C46).
# Z80 interpreter, OKI M6295, YMFM FM chips, and SH-3 stubs are completely stripped.
CORE_SRCS = \
  $(CORE_ROOT)/machine/m68k.cpp \
  $(CORE_ROOT)/machine/cave68k.cpp \
  $(CORE_ROOT)/machine/cave_video.cpp \
  $(CORE_ROOT)/machine/ymz280b.cpp \
  $(CORE_ROOT)/machine/eeprom93c46.cpp \
  $(CORE_ROOT)/runtime/recomp68k.cpp \
  $(BLOCKS68K_SRCS)

SRCS = $(CURDIR)/main.cpp $(CURDIR)/no_interpreters.cpp $(CURDIR)/xbox_asm.S

include $(NXDK_DIR)/Makefile

CORE_OBJ_DIR := obj
CORE_OBJS := $(patsubst $(CORE_ROOT)/%.cpp,$(CORE_OBJ_DIR)/%.obj,$(CORE_SRCS))
LOCAL_OBJS := $(CURDIR)/main.obj $(CURDIR)/no_interpreters.obj $(CURDIR)/xbox_asm.obj
OBJS := $(LOCAL_OBJS) $(CORE_OBJS)
DEPS := $(CURDIR)/main.cpp.d $(CURDIR)/no_interpreters.cpp.d $(CORE_OBJS:.obj=.cpp.d)

# Target Original Xbox Pentium III (Coppermine-128: i686 + MMX + SSE1, 16KB L1 + 128KB L2).
XBOX_ARCH_FLAGS := -O3 -march=pentium3 -mtune=pentium3 -mmmx -msse -mfpmath=sse -fomit-frame-pointer -Wno-parentheses-equality
NXDK_CXXFLAGS += $(XBOX_ARCH_FLAGS) -std=c++20 -DGAME_ddonpach=1 -DHEADLESS_BUILD=1 \
  -I$(CORE_ROOT) -I$(CORE_ROOT)/machine -I$(CORE_ROOT)/runtime -I$(CORE_ROOT)/third_party -I$(GEN_DIR)

$(CURDIR)/main.obj: $(CURDIR)/nv2a_cave_renderer.h $(CURDIR)/nv2a_presenter.h $(CURDIR)/xbox_audio.h $(CURDIR)/xbox_input.h $(CURDIR)/xbox_timer.h $(CURDIR)/xbox_asm.h
main.exe: $(CORE_OBJS)

$(CORE_OBJ_DIR)/%.obj: $(CORE_ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(VE)$(CXX) $(NXDK_CXXFLAGS) $(CXXFLAGS) -MD -MP -MT '$@' -MF '$(@:.obj=.cpp.d)' -c -o '$@' '$<'

-include $(DEPS)
