# fpga/Makefile - cocotb + Icarus Verilog simulation entry point...
#

SIM           ?= icarus
TOPLEVEL_LANG ?= verilog

VERILOG_SOURCES     = $(CURDIR)/rtl/axis_reg.v
COCOTB_TOPLEVEL     = axis_reg
COCOTB_TEST_MODULES = test_axis_reg

export PYTHONPATH := $(CURDIR)/tb:$(PYTHONPATH)

include $(shell cocotb-config --makefiles)/Makefile.sim

waves:
	gtkwave axis_reg.vcd &
