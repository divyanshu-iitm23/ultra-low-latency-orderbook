# fpga/Makefile - cocotb + Icarus Verilog simulation entry point.
#
#   make               simulate the default DUT (itch_parser)
#   make DUT=axis_reg  simulate another block
#   make pytests       pure-Python testbench helpers (no simulator)
#   make regress       run every testbench
#   make waves         open the DUT's dump in gtkwave

SIM           ?= icarus
TOPLEVEL_LANG ?= verilog

DUT ?= itch_parser

VERILOG_SOURCES     = $(CURDIR)/rtl/$(DUT).v
COCOTB_TOPLEVEL     = $(DUT)
COCOTB_TEST_MODULES = test_$(DUT)
SIM_BUILD           = sim_build/$(DUT)

export PYTHONPATH := $(CURDIR)/tb:$(PYTHONPATH)

include $(shell cocotb-config --makefiles)/Makefile.sim

# frames.py is stimulus AND ground truth for the step-3 blocks, so it is tested
# on its own -- a bug there would be "verified" straight into the hardware
pytests:
	cd $(CURDIR)/tb && python -m pytest -q test_frames.py

regress: pytests
	$(MAKE) DUT=axis_reg
	$(MAKE) DUT=axis_skip_align
	$(MAKE) DUT=itch_parser

waves:
	gtkwave $(DUT).vcd &
