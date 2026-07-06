# test_axis_reg.py - first cocotb testbench: AXI-Stream register slice.
#

import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles
from cocotbext.axi import AxiStreamBus, AxiStreamSource, AxiStreamSink


async def bringup(dut):

    source = AxiStreamSource(AxiStreamBus.from_prefix(dut, "s_axis"), dut.clk, dut.rst)
    sink   = AxiStreamSink(AxiStreamBus.from_prefix(dut, "m_axis"), dut.clk, dut.rst)

    cocotb.start_soon(Clock(dut.clk, 8, "ns").start())    # 125 MHz
    dut.rst.value = 1
    await ClockCycles(dut.clk, 5)
    dut.rst.value = 0
    await ClockCycles(dut.clk, 2)
    return source, sink


@cocotb.test()
async def smoke(dut):
    """One frame in -> the same frame out."""
    source, sink = await bringup(dut)

    payload = b"hello, simulation"
    await source.send(payload)
    rx = await sink.recv()
    assert bytes(rx.tdata) == payload


@cocotb.test()
async def random_traffic_with_backpressure(dut):
    """50 random-length frames; source stutters, sink backpressures."""
    source, sink = await bringup(dut)

    def stutter():                       # ~30% dead cycles on each side
        while True:
            yield random.random() < 0.3
    source.set_pause_generator(stutter())
    sink.set_pause_generator(stutter())

    frames = [bytes(random.randrange(256) for _ in range(random.randint(1, 64)))
              for _ in range(50)]
    for f in frames:
        await source.send(f)

    for i, expect in enumerate(frames):
        rx = await sink.recv()
        assert bytes(rx.tdata) == expect, f"frame {i} corrupted"
