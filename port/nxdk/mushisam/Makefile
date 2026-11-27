XBE_TITLE = Mushihimesama CV1000 Xbox
GEN_XISO = mushisam-cv1000-xbox.iso

ENGINE_DIR = $(CURDIR)/engine
GEN_DIR = $(CURDIR)/gen

SRCS = \
	$(CURDIR)/main.cpp \
	$(CURDIR)/xbox_asm.S \
	$(ENGINE_DIR)/machine/amm_decoder.cpp \
	$(ENGINE_DIR)/machine/blitter.cpp \
	$(ENGINE_DIR)/machine/cv1000.cpp \
	$(ENGINE_DIR)/machine/sh3.cpp \
	$(ENGINE_DIR)/machine/ymz770.cpp \
	$(ENGINE_DIR)/runtime/dispatch.cpp \
	$(sort $(wildcard $(GEN_DIR)/blocks_*.cpp)) \
	$(GEN_DIR)/registry.cpp

NXDK_CXX = y
NXDK_CXXFLAGS += -std=c++20 -O2 -march=pentium3 -msse -mmmx -mfpmath=sse -fomit-frame-pointer -fno-math-errno -fno-exceptions -fno-rtti
NXDK_CXXFLAGS += -DXBOX_CV1000_B -DXBOX_NO_SW_VRAM -DXBOX_FAST_CPU -DXBOX_IMMUTABLE_CODE
ifeq ($(FAKE_PLAY),1)
NXDK_CXXFLAGS += -DXBOX_FAKE_PLAY=1
endif
ifeq ($(STAGE_SWEEP),1)
NXDK_CXXFLAGS += -DXBOX_STAGE_SWEEP=1
endif
ifeq ($(AUDIO_OFF),1)
NXDK_CXXFLAGS += -DXBOX_AUDIO_OFF=1
endif
CUSTOM_NXDK ?= 1
ifeq ($(CUSTOM_NXDK),1)
NXDK_CXXFLAGS += -DXBOX_CUSTOM_NXDK=1
endif
ifneq ($(START_STAGE),)
NXDK_CXXFLAGS += -DXBOX_START_STAGE=$(START_STAGE)
endif
THIN_LTO ?= 1
ifeq ($(THIN_LTO),1)
NXDK_CXXFLAGS += -flto=thin
NXDK_CFLAGS += -flto=thin
endif
NXDK_CXXFLAGS += -I$(CURDIR) -I$(ENGINE_DIR) -I$(ENGINE_DIR)/machine -I$(ENGINE_DIR)/runtime -I$(GEN_DIR)
NXDK_CFLAGS += -O2 -march=pentium3 -msse -mmmx -mfpmath=sse -fomit-frame-pointer
NXDK_LDFLAGS += -map:main.map

include $(NXDK_DIR)/Makefile

$(ENGINE_DIR)/machine/amm_decoder.obj: NXDK_CXXFLAGS += -ffast-math
# Profile-guided hot/cold codegen for Pentium III 16 KB L1i / 128 KB L2 cache:
BLOCKS_OPT ?= -Oz
$(GEN_DIR)/blocks_hot.obj: NXDK_CXXFLAGS += -O3
$(GEN_DIR)/blocks_0%.obj: NXDK_CXXFLAGS += $(BLOCKS_OPT)

$(CURDIR)/main.obj: $(CURDIR)/nv2a_cv1k_renderer.h $(CURDIR)/xbox_audio.h $(CURDIR)/xbox_input.h $(CURDIR)/xbox_timer.h $(CURDIR)/xbox_asm.h $(CURDIR)/xbox_profiler.h $(CURDIR)/mushisam_fake_play.h $(ENGINE_DIR)/machine/cv1000.h $(ENGINE_DIR)/machine/ymz770.h $(ENGINE_DIR)/machine/sh3.h $(ENGINE_DIR)/machine/blitter.h $(ENGINE_DIR)/machine/nand.h
$(ENGINE_DIR)/machine/cv1000.obj: $(ENGINE_DIR)/machine/cv1000.h $(ENGINE_DIR)/machine/ymz770.h $(ENGINE_DIR)/machine/sh3.h $(ENGINE_DIR)/machine/blitter.h $(ENGINE_DIR)/machine/nand.h
