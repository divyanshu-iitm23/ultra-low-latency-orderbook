# fpga/Makefile - cocotb + Icarus Verilog simulation entry point

SIM           ?= icarus
TOPLEVEL_LANG ?= verilog

DUT ?= itch_parser

# blocks that instantiate other modules list them here (space-separated)
DEPS_eth_parser  = axis_skip_align
DEPS_ipv4_parser = axis_skip_align
DEPS_udp_parser  = axis_skip_align
DEPS_mold_parser = axis_skip_align
DEPS_feed_top    = eth_parser ipv4_parser udp_parser mold_parser itch_parser axis_skip_align

VERILOG_SOURCES     = $(CURDIR)/rtl/$(DUT).v \
                      $(foreach d,$(DEPS_$(DUT)),$(CURDIR)/rtl/$(d).v)
COCOTB_TOPLEVEL     = $(DUT)
COCOTB_TEST_MODULES = test_$(DUT)
SIM_BUILD           = sim_build/$(DUT)

export PYTHONPATH := $(CURDIR)/tb:$(PYTHONPATH)

# feed_top instantiates every block; each has its own `ifndef SYNTHESIS` dump
# block, and feed_top's own $dumpvars would recurse the whole hierarchy on top.
# Disable the dumps (dump-only, no logic under the guard) so the full-chain sim
# runs at speed instead of drowning in a redundant VCD.
ifeq ($(DUT),feed_top)
    COMPILE_ARGS += -DSYNTHESIS
endif

include $(shell cocotb-config --makefiles)/Makefile.sim

# frames.py is stimulus AND ground truth for the step-3 blocks, so it is tested
# on its own -- a bug there would be "verified" straight into the hardware
pytests:
	cd $(CURDIR)/tb && python -m pytest -q test_frames.py

regress: pytests
	$(MAKE) DUT=axis_reg
	$(MAKE) DUT=axis_skip_align
	$(MAKE) DUT=eth_parser
	$(MAKE) DUT=ipv4_parser
	$(MAKE) DUT=udp_parser
	$(MAKE) DUT=mold_parser
	$(MAKE) DUT=itch_parser
	$(MAKE) DUT=feed_top

waves:
	gtkwave $(DUT).vcd &
