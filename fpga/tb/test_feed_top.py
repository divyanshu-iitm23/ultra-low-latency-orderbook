# test_feed_top.py - the full front-end (3.6)

import random
import struct

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, FallingEdge
from cocotbext.axi import AxiStreamBus, AxiStreamSource

import frames as F
from test_itch_parser import golden_lines, fmt, gen_message   # same golden model as step 2


async def bringup(dut):
    source = AxiStreamSource(AxiStreamBus.from_prefix(dut, "s_axis"), dut.clk, dut.rst)
    cocotb.start_soon(Clock(dut.clk, 8, "ns").start())          # 125 MHz
    dut.rst.value = 1
    await ClockCycles(dut.clk, 5)
    dut.rst.value = 0
    await ClockCycles(dut.clk, 2)
    return source


def start_monitor(dut):
    """Collect one canonical line per decoded message, plus seq-gap strobes."""
    got, gaps = [], []

    async def mon():
        while True:
            await FallingEdge(dut.clk)
            if dut.msg_valid.value:
                got.append(fmt(dut))
            if dut.seq_gap.value:
                gaps.append(int(dut.pkt_seq.value))

    cocotb.start_soon(mon())
    return got, gaps


async def drive(dut, source, frames, drain=40):
    for f in frames:
        await source.send(f)
    await source.wait()
    await ClockCycles(dut.clk, drain)


def framed(bodies):
    """The BinaryFILE stream itch_dump expects: [2B len][body]..."""
    return b"".join(struct.pack(">H", len(b)) + b for b in bodies)


# the headline end-to-end tests

@cocotb.test()
async def end_to_end_synthetic(dut):
    """Ethernet frames built from synthetic ITCH -> decoded == Phase-1 parser."""
    source = await bringup(dut)
    got, _ = start_monitor(dut)

    raw    = b"".join(gen_message() for _ in range(500))
    bodies = F.split_messages(raw)
    frames, expect = F.packetize(bodies, per_packet=(1, 6), rng=random.Random(1))
    assert expect == raw                          # packetize re-frames identically

    await drive(dut, source, frames)
    exp = golden_lines(raw)
    assert got == exp, f"decoded {len(got)} vs golden {len(exp)}"


@cocotb.test()
async def end_to_end_real_capture(dut):
    """Real Nasdaq messages, wrapped into wire frames, decoded through the stack."""
    from test_itch_parser import REAL_FILE
    if REAL_FILE is None:
        dut._log.info("no capture file; skipping")
        return

    source = await bringup(dut)
    got, gaps = start_monitor(dut)

    bodies = F.split_messages(REAL_FILE.read_bytes()[: 1 << 20])[:1500]
    frames, expect = F.packetize(bodies, per_packet=(1, 8), rng=random.Random(2))

    await drive(dut, source, frames)
    exp = golden_lines(expect)
    assert got == exp, f"decoded {len(got)} vs golden {len(exp)}"
    assert gaps == [], f"unexpected gaps on in-order capture: {gaps}"
    modeled = sum(" mod=1 " in ln for ln in exp)
    dut._log.info(f"end-to-end: {len(exp)} messages, {modeled} modeled, {len(frames)} frames")


# the front-end must actually filter

@cocotb.test()
async def dropped_frames_contribute_nothing(dut):
    """Frames that must be filtered (wrong ethertype/proto/port/checksum) carry
    real ITCH messages -- none of them may appear in the decoded output."""
    source = await bringup(dut)
    got, _ = start_monitor(dut)

    good = F.split_messages(b"".join(gen_message() for _ in range(40)))
    drop_kw = [dict(ethertype=0x0806), dict(proto=6),
               dict(dport=9999), dict(bad_checksum=True)]

    frames, seq = [], 1
    for k, body in enumerate(good):
        if k % 3 == 1:                            # slip a doomed frame in first
            junk = F.split_messages(gen_message())[0]
            frames.append(F.wire_frame([junk], seq=777, **drop_kw[k % len(drop_kw)]))
        frames.append(F.wire_frame([body], seq=seq))
        seq += 1

    await drive(dut, source, frames)
    exp = golden_lines(framed(good))              # only the good messages
    assert got == exp, f"decoded {len(got)} vs golden {len(exp)} (a drop leaked?)"


@cocotb.test()
async def control_datagrams_emit_no_messages(dut):
    """Heartbeats and end-of-session frames interleaved must decode to nothing."""
    source = await bringup(dut)
    got, _ = start_monitor(dut)

    good = F.split_messages(b"".join(gen_message() for _ in range(30)))
    frames, seq = [], 1
    for k, body in enumerate(good):
        frames.append(F.wire_frame([body], seq=seq))
        seq += 1
        if k % 5 == 4:
            hb = F.mold_heartbeat(seq=seq) if k % 2 else F.mold_end_of_session(seq=seq)
            frames.append(F.eth(F.ipv4(F.udp(hb))))

    await drive(dut, source, frames)
    exp = golden_lines(framed(good))
    assert got == exp, f"decoded {len(got)} vs golden {len(exp)}"


# side-band still works through the whole chain

@cocotb.test()
async def sequence_gap_flagged_end_to_end(dut):
    """An out-of-order datagram must strobe seq_gap from the top-level port."""
    source = await bringup(dut)
    _got, gaps = start_monitor(dut)
    rng = random.Random(3)

    def frame(seq, n):
        bodies = F.split_messages(b"".join(gen_message() for _ in range(n)))[:n]
        return F.wire_frame(bodies, seq=seq)

    # in order: 1(+2) -> 3(+2), then jump to 100 -> one gap, then resync 101
    await drive(dut, source, [frame(1, 2), frame(3, 2)], drain=20)
    assert gaps == []
    await drive(dut, source, [frame(100, 1)], drain=20)
    assert gaps == [100], f"gap not flagged end-to-end: {gaps}"
    await drive(dut, source, [frame(101, 1)], drain=20)
    assert gaps == [100], f"spurious gap after resync: {gaps}"


@cocotb.test()
async def with_source_stutter(dut):
    """The whole chain under a stuttering source."""
    source = await bringup(dut)
    got, _ = start_monitor(dut)

    def pause():
        while True:
            yield random.random() < 0.25
    source.set_pause_generator(pause())

    raw    = b"".join(gen_message() for _ in range(300))
    bodies = F.split_messages(raw)
    frames, expect = F.packetize(bodies, per_packet=(1, 5), rng=random.Random(4))

    await drive(dut, source, frames)
    exp = golden_lines(raw)
    assert got == exp, f"decoded {len(got)} vs golden {len(exp)}"
