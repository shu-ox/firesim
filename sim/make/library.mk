# See LICENSE for license details.

# Defined for each platform

simif_dir = $(firesim_base_dir)/midas/src/main/cc
midas_h  = $(shell find $(simif_dir) -name "*.h")
midas_cc = $(shell find $(simif_dir) -name "*.cc")

common_cxx_flags := $(TARGET_CXX_FLAGS) -Wno-unused-variable -DFIRESIM
ifdef ($(shell uname -s),Linux)
common_ld_flags := $(TARGET_LD_FLAGS) -lrt
else
common_ld_flags := $(TARGET_LD_FLAGS)
endif
