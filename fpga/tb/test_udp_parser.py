# test_udp_parser.py - the UDP layer (3.4).
#
# Input is a UDP segment (what ipv4_parser hands downstream). Reference model is
# frames.unwrap_udp(): strip 8 bytes, keep it only if the destination port
# matches, then trim to the UDP length field. Rejects emit nothing.

import random
import struct

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


async def run_seg(dut, source, sink, seg):
    """Drive one UDP segment; return the emitted payload (b'' if dropped)."""
    exp = F.unwrap_udp(seg)                       # None/b'' == emit nothing
    await source.send(seg)
    await source.wait()
    await ClockCycles(dut.clk, 50)
    if not exp:
        assert sink.empty(), "rejected/empty segment should emit nothing"
        return b""
    rx = await sink.recv()
    return bytes(rx.tdata)


@cocotb.test()
async def plain_segments(dut):
    """Right port, good length: strip 8, payload == unwrap_udp."""
    source, sink = await bringup(dut)
    rng = random.Random(1)
    for n in (1, 7, 8, 9, 20, 100, 300):
        seg = F.udp(rng.randbytes(n))
        got = await run_seg(dut, source, sink, seg)
        assert got == F.unwrap_udp(seg), f"n={n}"


@cocotb.test()
async def wrong_port_is_dropped(dut):
    """Anything not addressed to the feed port emits nothing."""
    source, sink = await bringup(dut)
    rng = random.Random(2)
    for dport in (0, 1, 9999, F.FEED_PORT + 1, 65535):
        seg = F.udp(rng.randbytes(40), dport=dport)
        assert F.unwrap_udp(seg) is None
        await run_seg(dut, source, sink, seg)
    assert sink.empty()


@cocotb.test()
async def udp_length_trims_trailing_bytes(dut):
    """Bytes past the UDP length field are not payload and must be trimmed."""
    source, sink = await bringup(dut)
    rng = random.Random(3)
    for pad in (1, 8, 25):
        seg = F.udp(rng.randbytes(30)) + b"\x00" * pad
        exp = F.unwrap_udp(seg)
        assert len(exp) == 30                     # trailing bytes excluded
        got = await run_seg(dut, source, sink, seg)
        assert got == exp, f"pad={pad}"


@cocotb.test()
async def runts_and_empty_payloads_emit_nothing(dut):
    """A segment shorter than the 8-byte header, or carrying no payload."""
    source, sink = await bringup(dut)
    rng = random.Random(4)

    full = F.udp(rng.randbytes(40))
    for seg in (full[:3], full[:7],               # runts: no complete header
                F.udp(b"")):                      # valid header, zero payload
        await run_seg(dut, source, sink, seg)
    assert sink.empty()


@cocotb.test()
async def mixed_stream_with_backpressure(dut):
    """Good / wrong-port / padded segments interleaved, stutter + backpressure."""
    source, sink = await bringup(dut)
    rng = random.Random(5)

    def pause():
        while True:
            yield rng.random() < 0.3
    source.set_pause_generator(pause())
    sink.set_pause_generator(pause())

    for _ in range(40):
        payload = rng.randbytes(rng.randint(10, 120))
        kind    = rng.choice(["ok", "ok", "badport", "padded"])
        if kind == "ok":
            seg = F.udp(payload)
        elif kind == "badport":
            seg = F.udp(payload, dport=rng.randint(1, 60000))
            if seg[2:4] == struct.pack(">H", F.FEED_PORT):
                seg = F.udp(payload, dport=F.FEED_PORT + 1)
        else:
            seg = F.udp(payload) + b"\x00" * rng.randint(1, 16)
        got = await run_seg(dut, source, sink, seg)
        assert got == (F.unwrap_udp(seg) or b""), f"kind={kind}"


@cocotb.test()
async def real_capture_segments(dut):
    """Real Nasdaq messages -> Mold -> UDP: strip UDP, get the Mold datagram."""
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
        seg   = F.udp(F.mold_packet(group, seq))
        got = await run_seg(dut, source, sink, seg)
        assert got == F.unwrap_udp(seg), f"i={i}"
        i   += n
        seq += n
