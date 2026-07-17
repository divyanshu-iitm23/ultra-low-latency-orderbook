# test_eth_parser.py - the Ethernet framing layer (3.2).
#

import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles
from cocotbext.axi import AxiStreamBus, AxiStreamSource, AxiStreamSink

import frames as F


async def bringup(dut):
    source = AxiStreamSource(AxiStreamBus.from_prefix(dut, "s_axis"), dut.clk, dut.rst)
    sink   = AxiStreamSink(AxiStreamBus.from_prefix(dut, "m_axis"), dut.clk, dut.rst)
    cocotb.start_soon(Clock(dut.clk, 8, "ns").start())          # 125 MHz
    dut.rst.value = 1
    await ClockCycles(dut.clk, 5)
    dut.rst.value = 0
    await ClockCycles(dut.clk, 2)
    return source, sink


async def run_frame(dut, source, sink, frame):
    """Drive one Ethernet frame; return the emitted payload (b'' if dropped)."""
    exp = F.unwrap_eth(frame)                    # None == not IPv4 == must drop
    await source.send(frame)
    await source.wait()
    await ClockCycles(dut.clk, 40)
    if exp is None:
        assert sink.empty(), "non-IPv4 frame should emit nothing"
        return b""
    rx = await sink.recv()
    return bytes(rx.tdata)


@cocotb.test()
async def plain_ipv4(dut):
    """Untagged IPv4 frames: strip 14 bytes, payload == unwrap_eth."""
    source, sink = await bringup(dut)
    rng = random.Random(1)
    for n in (17, 34, 40, 100, 255):
        msg   = bytes([rng.randrange(0x41, 0x5B)]) + rng.randbytes(n - 1)
        frame = F.wire_frame([msg], seq=1)
        got = await run_frame(dut, source, sink, frame)
        assert got == F.unwrap_eth(frame), f"n={n}"


@cocotb.test()
async def vlan_tagged(dut):
    """802.1Q tag pushes the real ethertype to bytes 16-17: strip 18 bytes."""
    source, sink = await bringup(dut)
    rng = random.Random(2)
    for vid in (1, 100, 4094):
        msg   = bytes([rng.randrange(0x41, 0x5B)]) + rng.randbytes(35)
        frame = F.wire_frame([msg], seq=1, vlan=vid)
        assert frame[12:14].hex() == "8100"     # tag really is present
        got = await run_frame(dut, source, sink, frame)
        assert got == F.unwrap_eth(frame), f"vid={vid}"


@cocotb.test()
async def non_ipv4_is_dropped(dut):
    """ARP / IPv6 ethertypes, tagged and untagged, emit nothing."""
    source, sink = await bringup(dut)
    rng = random.Random(3)
    for kw in ({"ethertype": 0x0806},           # ARP
               {"ethertype": 0x86DD},           # IPv6
               {"ethertype": 0x0806, "vlan": 100},   # ARP behind a VLAN tag
               {"ethertype": 0x86DD, "vlan": 7}):
        msg   = bytes([rng.randrange(0x41, 0x5B)]) + rng.randbytes(35)
        frame = F.wire_frame([msg], seq=1, **kw)
        assert F.unwrap_eth(frame) is None       # reference agrees it's a drop
        await run_frame(dut, source, sink, frame)
    assert sink.empty()


@cocotb.test()
async def mixed_stream_with_backpressure(dut):
    """IPv4 / VLAN / dropped frames interleaved, with stutter + backpressure."""
    source, sink = await bringup(dut)
    rng = random.Random(4)

    def pause():
        while True:
            yield rng.random() < 0.3
    source.set_pause_generator(pause())
    sink.set_pause_generator(pause())

    for _ in range(40):
        msg  = bytes([rng.randrange(0x41, 0x5B)]) + rng.randbytes(rng.randint(16, 60))
        kind = rng.choice(["ipv4", "vlan", "arp"])
        if kind == "ipv4":
            frame = F.wire_frame([msg], seq=1)
        elif kind == "vlan":
            frame = F.wire_frame([msg], seq=1, vlan=rng.randint(1, 4094))
        else:
            frame = F.wire_frame([msg], seq=1, ethertype=0x0806)
        got = await run_frame(dut, source, sink, frame)
        assert got == (F.unwrap_eth(frame) or b""), f"kind={kind}"


@cocotb.test()
async def real_capture_frames(dut):
    """Real Nasdaq messages wrapped by frames.py: strip Ethernet, get the IP packet."""
    from test_itch_parser import REAL_FILE
    if REAL_FILE is None:
        dut._log.info("no capture file; skipping")
        return

    source, sink = await bringup(dut)
    rng  = random.Random(5)
    msgs = F.split_messages(REAL_FILE.read_bytes()[: 1 << 20])[:300]
    wire, _ = F.packetize(msgs, per_packet=(1, 6), rng=rng)
    for frame in wire:
        got = await run_frame(dut, source, sink, frame)
        assert got == F.unwrap_eth(frame)
