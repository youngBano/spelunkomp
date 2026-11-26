CXX = g++
CXXFLAGS = -std=c++20 -O2 -DXBOX_CV1000_B -DXBOX_NO_SW_VRAM -DXBOX_FAST_CPU -DXBOX_IMMUTABLE_CODE \
           -I.. -I../engine -I../engine/machine -I../engine/runtime -I../gen
ifeq ($(PROFILE_HOT),1)
CXXFLAGS += -DMUSHISAM_PROFILE_HOT
endif

OBJ_DIR = obj
ENGINE_SRCS = \
	../engine/machine/amm_decoder.cpp \
	../engine/machine/blitter.cpp \
	../engine/machine/cv1000.cpp \
	../engine/machine/sh3.cpp \
	../engine/machine/ymz770.cpp \
	../engine/runtime/dispatch.cpp

GEN_SRCS = $(sort $(wildcard ../gen/blocks_*.cpp)) ../gen/registry.cpp
TEST_SRCS = mushisam_parity_test.cpp

OBJS = \
	$(patsubst ../%.cpp,$(OBJ_DIR)/%.o,$(ENGINE_SRCS)) \
	$(patsubst ../%.cpp,$(OBJ_DIR)/%.o,$(GEN_SRCS)) \
	$(OBJ_DIR)/mushisam_parity_test.o

all: mushisam_parity_test.exe

mushisam_parity_test.exe: $(OBJS)
	$(CXX) $(CXXFLAGS) -static $^ -o $@

$(OBJ_DIR)/%.o: ../%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/mushisam_parity_test.o: mushisam_parity_test.cpp ../mushisam_fake_play.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) mushisam_parity_test.exe
