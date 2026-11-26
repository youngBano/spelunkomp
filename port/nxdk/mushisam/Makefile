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
NXDK_CXXFLAGS += -DXBOX_CV1000_B -DXBOX_NO_SW_VRAM
ifeq ($(FAKE_PLAY),1)
NXDK_CXXFLAGS += -DXBOX_FAKE_PLAY=1
endif
NXDK_CXXFLAGS += -I$(CURDIR) -I$(ENGINE_DIR) -I$(ENGINE_DIR)/machine -I$(ENGINE_DIR)/runtime
NXDK_CFLAGS += -O2 -march=pentium3 -msse -mmmx -mfpmath=sse -fomit-frame-pointer

include $(NXDK_DIR)/Makefile

$(CURDIR)/main.obj: $(CURDIR)/nv2a_cv1k_renderer.h $(CURDIR)/xbox_audio.h $(CURDIR)/xbox_input.h $(CURDIR)/xbox_timer.h $(CURDIR)/xbox_asm.h $(CURDIR)/xbox_profiler.h $(CURDIR)/mushisam_fake_play.h
