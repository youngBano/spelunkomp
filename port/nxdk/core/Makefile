# Link the Cave 68000 board core into its own XBE, separate from the bootstrap.
XBE_TITLE = spelunkomp-cave68k-core
GEN_XISO = spelunkomp-cave68k-core.iso
OUTPUT_DIR = bin
NXDK_DIR ?= $(CURDIR)/../../../nxdk
CORE_ROOT := $(abspath $(CURDIR)/../../../spelunkomp)
NXDK_CXX = y
NXDK_SDL = y

CORE_SRCS = \
  $(CORE_ROOT)/machine/m68k.cpp \
  $(CORE_ROOT)/machine/cave68k.cpp \
  $(CORE_ROOT)/machine/cave_video.cpp \
  $(CORE_ROOT)/machine/ymz280b.cpp \
  $(CORE_ROOT)/machine/okim6295.cpp \
  $(CORE_ROOT)/machine/eeprom93c46.cpp \
  $(CORE_ROOT)/machine/cave_z80.cpp \
  $(CORE_ROOT)/machine/z80.cpp \
  $(CORE_ROOT)/third_party/ymfm_opn.cpp \
  $(CORE_ROOT)/third_party/ymfm_ssg.cpp \
  $(CORE_ROOT)/third_party/ymfm_adpcm.cpp \
  $(CORE_ROOT)/third_party/ymfm_opm.cpp \
  $(CORE_ROOT)/runtime/recomp68k.cpp \
  $(CORE_ROOT)/runtime/recomp_z80.cpp \
  $(CORE_ROOT)/runtime/no_blocks.cpp \
  $(CORE_ROOT)/runtime/no_blocks68k.cpp \
  $(CORE_ROOT)/runtime/no_z80_blocks.cpp
# The nxdk makefile handles its libraries and main.cpp; core objects are mapped locally below.
SRCS = $(CURDIR)/main.cpp

include $(NXDK_DIR)/Makefile

# Keep build intermediates outside OUTPUT_DIR so the XISO contains only the XBE.
CORE_OBJ_DIR := obj
CORE_OBJS := $(patsubst $(CORE_ROOT)/%.cpp,$(CORE_OBJ_DIR)/%.obj,$(CORE_SRCS))
# nxdk's main.exe prerequisites were expanded from SRCS before this file resumes.
# Keep main.obj at the path nxdk already tracks; only core objects need staging out.
OBJS := $(CURDIR)/main.obj $(CORE_OBJS)
DEPS := $(CURDIR)/main.cpp.d $(CORE_OBJS:.obj=.cpp.d)
# Target Original Xbox Pentium III (Coppermine-128: i686 + MMX + SSE1, 16KB L1 + 128KB L2).
XBOX_ARCH_FLAGS := -O3 -march=pentium3 -mtune=pentium3 -mmmx -msse -mfpmath=sse -fomit-frame-pointer
NXDK_CXXFLAGS += $(XBOX_ARCH_FLAGS) -std=c++20 -DGAME_ddonpach=1 -DHEADLESS_BUILD=1 -I$(CORE_ROOT) -I$(CORE_ROOT)/machine -I$(CORE_ROOT)/runtime -I$(CORE_ROOT)/third_party -I$(CORE_ROOT)/generated/ddonpach
$(CURDIR)/main.obj: $(CURDIR)/nv2a_presenter.h
main.exe: $(CORE_OBJS)

$(CORE_OBJ_DIR)/%.obj: $(CORE_ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(VE)$(CXX) $(NXDK_CXXFLAGS) $(CXXFLAGS) -MD -MP -MT '$@' -MF '$(@:.obj=.cpp.d)' -c -o '$@' '$<'

-include $(DEPS)
