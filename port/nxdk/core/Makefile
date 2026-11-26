# Standalone Original Xbox native build for DoDonPachi (completely disconnected from parent spelunkomp):
# - Zero SDL2 (native XAudio AC'97 DMA + native USB XID gamepad + native MCPX hardware timers)
# - Zero Z80/OKI/FM/SH-3 interpreters (lean M68K native static-recompiled blocks only)
# - Zero-copy NV2A hardware primitive renderer (nv2a_cave_renderer.h)
# - Hand-tuned Pentium III i686/MMX/SSE1 assembly kernels (xbox_asm.S)
XBE_TITLE = spelunkomp-cave68k-core
GEN_XISO = spelunkomp-cave68k-core.iso
OUTPUT_DIR = bin
NXDK_DIR ?= $(CURDIR)/../../../nxdk
ENGINE_ROOT := $(CURDIR)/engine
GEN_DIR := $(CURDIR)/gen
NXDK_CXX = y

# Link statically-recompiled native x86 68000 blocks when present in ./gen/.
GEN_SRCS := $(sort $(wildcard $(GEN_DIR)/native_*.cpp))
ifeq ($(strip $(GEN_SRCS)),)
  BLOCKS68K_SRCS := $(ENGINE_ROOT)/runtime/no_blocks68k.cpp
else
  BLOCKS68K_SRCS := $(GEN_SRCS)
endif

ENGINE_SRCS = \
  $(ENGINE_ROOT)/machine/m68k.cpp \
  $(ENGINE_ROOT)/machine/cave68k.cpp \
  $(ENGINE_ROOT)/machine/cave_video.cpp \
  $(ENGINE_ROOT)/machine/ymz280b.cpp \
  $(ENGINE_ROOT)/machine/eeprom93c46.cpp \
  $(ENGINE_ROOT)/runtime/recomp68k.cpp \
  $(BLOCKS68K_SRCS)

SRCS = $(CURDIR)/main.cpp $(CURDIR)/xbox_asm.S

include $(NXDK_DIR)/Makefile

ENGINE_OBJ_DIR := obj
ENGINE_OBJS := $(patsubst $(ENGINE_ROOT)/%.cpp,$(ENGINE_OBJ_DIR)/%.obj,$(filter $(ENGINE_ROOT)/%,$(ENGINE_SRCS)))
GEN_OBJS := $(patsubst $(GEN_DIR)/%.cpp,$(ENGINE_OBJ_DIR)/gen/%.obj,$(filter $(GEN_DIR)/%,$(ENGINE_SRCS)))
ALL_ENGINE_OBJS := $(ENGINE_OBJS) $(GEN_OBJS)
OBJS := $(CURDIR)/main.obj $(CURDIR)/xbox_asm.obj $(ALL_ENGINE_OBJS)
DEPS := $(CURDIR)/main.cpp.d $(ALL_ENGINE_OBJS:.obj=.cpp.d)

# Target Original Xbox Pentium III (Coppermine-128: i686 + MMX + SSE1, 16KB L1 + 128KB L2).
XBOX_ARCH_FLAGS := -O3 -march=pentium3 -mtune=pentium3 -mmmx -msse -mfpmath=sse -fomit-frame-pointer -Wno-parentheses-equality
NXDK_CXXFLAGS += $(XBOX_ARCH_FLAGS) -std=c++20 -DGAME_ddonpach=1 -DHEADLESS_BUILD=1 \
  -I$(ENGINE_ROOT) -I$(ENGINE_ROOT)/machine -I$(ENGINE_ROOT)/runtime -I$(GEN_DIR)

$(CURDIR)/main.obj: $(CURDIR)/nv2a_cave_renderer.h $(CURDIR)/nv2a_presenter.h $(CURDIR)/xbox_audio.h $(CURDIR)/xbox_input.h $(CURDIR)/xbox_timer.h $(CURDIR)/xbox_asm.h
main.exe: $(ALL_ENGINE_OBJS)

$(ENGINE_OBJ_DIR)/%.obj: $(ENGINE_ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(VE)$(CXX) $(NXDK_CXXFLAGS) $(CXXFLAGS) -MD -MP -MT '$@' -MF '$(@:.obj=.cpp.d)' -c -o '$@' '$<'

$(ENGINE_OBJ_DIR)/gen/%.obj: $(GEN_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(VE)$(CXX) $(NXDK_CXXFLAGS) $(CXXFLAGS) -MD -MP -MT '$@' -MF '$(@:.obj=.cpp.d)' -c -o '$@' '$<'

# Regenerate native 68000 blocks from local bin/ddonpach ROMs using standalone port/nxdk/tools/m68k/recomp.py
.PHONY: gen
gen:
	@mkdir -p $(GEN_DIR)
	python3 -c 'b1=open("bin/ddonpach/b1.u27","rb").read(); b2=open("bin/ddonpach/b2.u26","rb").read(); p=bytearray(len(b1)*2); p[0::2]=b1; p[1::2]=b2; open("$(GEN_DIR)/program.bin","wb").write(p)'
	python3 ../tools/m68k/recomp.py --program $(GEN_DIR)/program.bin --entries $(GEN_DIR)/entries.txt --out $(GEN_DIR) --guess $(GEN_DIR)/guesses.txt --list $(GEN_DIR)/program.lst

-include $(DEPS)
