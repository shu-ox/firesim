# See LICENSE for license details.

# Defined for each platform

simif_dir = $(firesim_base_dir)/midas/src/main/cc
midas_h  = $(shell find $(simif_dir) -name "*.h")
midas_cc = $(shell find $(simif_dir) -name "*.cc")

driver_pic_cxx_flags :=
ifneq ($(filter 1 yes YES true TRUE,$(FIRESIM_DRIVER_PIC)),)
driver_pic_cxx_flags += -fPIC
endif

common_cxx_flags := $(TARGET_CXX_FLAGS) $(driver_pic_cxx_flags) -Wno-unused-variable -DFIRESIM
ifdef ($(shell uname -s),Linux)
common_ld_flags := $(TARGET_LD_FLAGS) -lrt
else
common_ld_flags := $(TARGET_LD_FLAGS)
endif
