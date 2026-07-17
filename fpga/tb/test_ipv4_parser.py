# test_ipv4_parser.py - the IPv4 layer (3.3).
#

import random
import struct

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles
from cocotbext.axi import AxiStreamBus, AxiStreamSource, AxiStreamSink

import frames as F


def ip_packet(inner: bytes, **ipkw) -> bytes:
    """An IPv4 packet carrying `inner` as its UDP segment."""
    return F.ipv4(F.udp(inner), **ipkw)


async def bringup(dut):
    source = AxiStreamSource(AxiStreamBus.from_prefix(dut, "s_axis"), dut.clk, dut.rst)
    sink   = AxiStreamSink(AxiStreamBus.from_prefix(dut, "m_axis"), dut.clk, dut.rst)
    cocotb.start_soon(Clock(dut.clk, 8, "ns").start())          # 125 MHz
    dut.rst.value = 1
    await ClockCycles(dut.clk, 5)
    dut.rst.value = 0
    await ClockCycles(dut.clk, 2)
    return source, sink


async def run_pkt(dut, source, sink, pkt):
    """Drive one IP packet; return the emitted payload (b'' if dropped)."""
    exp = F.unwrap_ipv4(pkt)                      # None == reject == emit nothing
    await source.send(pkt)
    await source.wait()
    await ClockCycles(dut.clk, 50)
    if exp is None:
        assert sink.empty(), "rejected IP packet should emit nothing"
        return b""
    rx = await sink.recv()
    return bytes(rx.tdata)


@cocotb.test()
async def plain_udp_packets(dut):
    """version 4, IHL 5, proto 17, good checksum: strip 20, payload == unwrap_ipv4."""
    source, sink = await bringup(dut)
    rng = random.Random(1)
    for n in (20, 40, 100, 300):
        pkt = ip_packet(rng.randbytes(n))
        got = await run_pkt(dut, source, sink, pkt)
        assert got == F.unwrap_ipv4(pkt), f"n={n}"


@cocotb.test()
async def variable_ihl_options(dut):
    """IP options make the header 24-60 bytes: skip = IHL*4, checksum still over
    the whole header. Exercises the variable-length hold window."""
    source, sink = await bringup(dut)
    rng = random.Random(2)
    for ihl_words in (5, 6, 7, 9, 15):
        pkt = ip_packet(rng.randbytes(64), ihl_words=ihl_words)
        assert (pkt[0] & 0x0F) == ihl_words
        got = await run_pkt(dut, source, sink, pkt)
        assert got == F.unwrap_ipv4(pkt), f"ihl_words={ihl_words}"


@cocotb.test()
async def total_length_trims_trailing_bytes(dut):
    """Bytes past total_length (e.g. Ethernet padding) must be trimmed off."""
    source, sink = await bringup(dut)
    rng = random.Random(3)
    for pad in (2, 8, 20):
        pkt = ip_packet(rng.randbytes(30)) + b"\x00" * pad
        exp = F.unwrap_ipv4(pkt)
        assert len(exp) == 8 + 30                 # padding excluded from the payload
        got = await run_pkt(dut, source, sink, pkt)
        assert got == exp, f"pad={pad}"


@cocotb.test()
async def rejects_emit_nothing(dut):
    """Wrong protocol, bad checksum, and wrong version are all dropped."""
    source, sink = await bringup(dut)
    rng = random.Random(4)

    tcp  = ip_packet(rng.randbytes(40), proto=6)
    bad  = ip_packet(rng.randbytes(40), bad_checksum=True)
    good = ip_packet(rng.randbytes(40))
    v6   = bytes([0x60 | (good[0] & 0x0F)]) + good[1:]      # version nibble -> 6

    for pkt in (tcp, bad, v6):
        assert F.unwrap_ipv4(pkt) is None
        await run_pkt(dut, source, sink, pkt)
    assert sink.empty()


@cocotb.test()
async def mixed_stream_with_backpressure(dut):
    """Good / options / dropped packets interleaved, with stutter + backpressure."""
    source, sink = await bringup(dut)
    rng = random.Random(5)

    def pause():
        while True:
            yield rng.random() < 0.3
    source.set_pause_generator(pause())
    sink.set_pause_generator(pause())

    for _ in range(40):
        inner = rng.randbytes(rng.randint(20, 120))
        kind  = rng.choice(["ok", "opt", "tcp", "badcsum"])
        if kind == "ok":
            pkt = ip_packet(inner)
        elif kind == "opt":
            pkt = ip_packet(inner, ihl_words=rng.choice([6, 7, 9]))
        elif kind == "tcp":
            pkt = ip_packet(inner, proto=6)
        else:
            pkt = ip_packet(inner, bad_checksum=True)
        got = await run_pkt(dut, source, sink, pkt)
        assert got == (F.unwrap_ipv4(pkt) or b""), f"kind={kind}"


@cocotb.test()
async def real_capture_packets(dut):
    """Real Nasdaq messages -> Mold -> UDP -> IPv4: strip IP, get the UDP segment."""
    from test_itch_parser import REAL_FILE
    if REAL_FILE is None:
        dut._log.info("no capture file; skipping")
        return

    source, sink = await bringup(dut)
    rng  = random.Random(6)
    msgs = F.split_messages(REAL_FILE.read_bytes()[: 1 << 20])[:300]

    i, seq = 0, 1
    while i < len(msgs):
        n     = rng.randint(1, 6)
        group = msgs[i:i + n]
        pkt   = F.ipv4(F.udp(F.mold_packet(group, seq)))
        got = await run_pkt(dut, source, sink, pkt)
        assert got == F.unwrap_ipv4(pkt), f"i={i}"
        i   += n
        seq += n
