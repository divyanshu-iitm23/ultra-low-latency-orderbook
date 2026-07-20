# test_mold_parser.py - the MoldUDP64 layer (3.5).
#

import random
import struct

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, FallingEdge
from cocotbext.axi import AxiStreamBus, AxiStreamSource, AxiStreamSink

import frames as F


def expected_payload(dgram: bytes) -> bytes:
    """What the front-end owes itch_parser.v for this datagram."""
    m = F.unwrap_mold(dgram)
    if m is None:
        return b""
    _seq, count, payload = m
    return b"" if count in (0, 0xFFFF) else payload


async def bringup(dut):
    source = AxiStreamSource(AxiStreamBus.from_prefix(dut, "s_axis"), dut.clk, dut.rst)
    sink   = AxiStreamSink(AxiStreamBus.from_prefix(dut, "m_axis"), dut.clk, dut.rst)
    cocotb.start_soon(Clock(dut.clk, 8, "ns").start())          # 125 MHz
    dut.rst.value = 1
    await ClockCycles(dut.clk, 5)
    dut.rst.value = 0
    await ClockCycles(dut.clk, 2)
    return source, sink


def start_monitor(dut):
    """Collect (seq, count) per decoded header, and the seqs that flagged a gap."""
    hdrs, gaps = [], []

    async def mon():
        while True:
            await FallingEdge(dut.clk)
            if dut.hdr_valid.value:
                hdrs.append((int(dut.pkt_seq.value), int(dut.pkt_count.value)))
            if dut.seq_gap.value:
                gaps.append(int(dut.pkt_seq.value))

    cocotb.start_soon(mon())
    return hdrs, gaps


async def run_dgram(dut, source, sink, dgram):
    """Drive one Mold datagram; return the emitted payload (b'' if nothing)."""
    exp = expected_payload(dgram)
    await source.send(dgram)
    await source.wait()
    await ClockCycles(dut.clk, 50)
    if not exp:
        assert sink.empty(), "datagram carrying no messages should emit nothing"
        return b""
    rx = await sink.recv()
    return bytes(rx.tdata)


def msgs(n, rng):
    return [bytes([rng.randrange(0x41, 0x5B)]) + rng.randbytes(ln - 1)
            for ln in (rng.choice([19, 23, 31, 36]) for _ in range(n))]


@cocotb.test()
async def strips_header_and_emits_blocks(dut):
    """Strip 20 bytes: the payload is exactly the length-prefixed ITCH blocks."""
    source, sink = await bringup(dut)
    rng = random.Random(1)
    for n in (1, 2, 5, 12):
        group = msgs(n, rng)
        dg    = F.mold_packet(group, seq=1)
        got = await run_dgram(dut, source, sink, dg)
        assert got == expected_payload(dg), f"n={n}"
        assert got == b"".join(struct.pack(">H", len(m)) + m for m in group)


@cocotb.test()
async def header_fields_are_decoded(dut):
    """seq straddles beats 1-2 and count lands in beat 2: both must come out right."""
    source, sink = await bringup(dut)
    hdrs, _gaps = start_monitor(dut)
    rng = random.Random(2)

    cases = [(1, 1), (2, 3), (0xFFFF_FFFF, 2), (0x1234_5678_9ABC_DEF0, 4)]
    seqs_sent = []
    for seq, n in cases:
        dg = F.mold_packet(msgs(n, rng), seq=seq)
        await run_dgram(dut, source, sink, dg)
        seqs_sent.append((seq, n))

    assert hdrs == seqs_sent, f"decoded {hdrs}, sent {seqs_sent}"


@cocotb.test()
async def heartbeat_and_end_of_session_emit_nothing(dut):
    """count==0 and count==0xFFFF carry no messages."""
    source, sink = await bringup(dut)
    hdrs, _gaps = start_monitor(dut)

    for dg in (F.mold_heartbeat(seq=5), F.mold_end_of_session(seq=5)):
        await run_dgram(dut, source, sink, dg)
    assert sink.empty()
    assert hdrs == [(5, 0), (5, 0xFFFF)]          # headers still decoded


@cocotb.test()
async def sequence_gaps_are_detected(dut):
    """expected = prev_seq + prev_count; a mismatch strobes seq_gap, then resyncs."""
    source, sink = await bringup(dut)
    _hdrs, gaps = start_monitor(dut)
    rng = random.Random(3)

    # in-order run: 1(+2) -> 3(+2) -> 5(+2); no gaps
    for seq, n in ((1, 2), (3, 2), (5, 2)):
        await run_dgram(dut, source, sink, F.mold_packet(msgs(n, rng), seq=seq))
    assert gaps == [], f"unexpected gaps {gaps}"

    # jump: expected 7, got 100 -> one gap
    await run_dgram(dut, source, sink, F.mold_packet(msgs(1, rng), seq=100))
    assert gaps == [100], f"gap not flagged: {gaps}"

    # resync: after seq=100 count=1, expected is 101 -> no further gap
    await run_dgram(dut, source, sink, F.mold_packet(msgs(3, rng), seq=101))
    assert gaps == [100], f"spurious gap after resync: {gaps}"


@cocotb.test()
async def heartbeat_does_not_advance_expectation(dut):
    """A heartbeat (count==0) carries the next expected seq and advances nothing."""
    source, sink = await bringup(dut)
    _hdrs, gaps = start_monitor(dut)
    rng = random.Random(4)

    await run_dgram(dut, source, sink, F.mold_packet(msgs(2, rng), seq=10))  # -> expect 12
    await run_dgram(dut, source, sink, F.mold_heartbeat(seq=12))             # still 12
    await run_dgram(dut, source, sink, F.mold_packet(msgs(1, rng), seq=12))  # ok
    assert gaps == [], f"unexpected gaps {gaps}"


@cocotb.test()
async def runts_emit_nothing(dut):
    """Datagrams that die inside the 20-byte header."""
    source, sink = await bringup(dut)
    hdrs, _gaps = start_monitor(dut)
    rng = random.Random(5)

    full = F.mold_packet(msgs(1, rng), seq=1)
    for dg in (full[:5], full[:15], full[:19]):
        await run_dgram(dut, source, sink, dg)
    assert sink.empty()
    assert hdrs == [], "no header should decode from a runt"


@cocotb.test()
async def mixed_stream_with_backpressure(dut):
    """Data / heartbeat / end-of-session interleaved, with stutter + backpressure."""
    source, sink = await bringup(dut)
    rng = random.Random(6)

    def pause():
        while True:
            yield rng.random() < 0.3
    source.set_pause_generator(pause())
    sink.set_pause_generator(pause())

    seq = 1
    for _ in range(30):
        kind = rng.choice(["data", "data", "data", "hb", "eos"])
        if kind == "data":
            n  = rng.randint(1, 5)
            dg = F.mold_packet(msgs(n, rng), seq=seq)
            seq += n
        elif kind == "hb":
            dg = F.mold_heartbeat(seq=seq)
        else:
            dg = F.mold_end_of_session(seq=seq)
        got = await run_dgram(dut, source, sink, dg)
        assert got == expected_payload(dg), f"kind={kind}"


@cocotb.test()
async def real_capture_datagrams(dut):
    """Real Nasdaq messages in Mold datagrams: the concatenated payloads must
    rebuild exactly the length-prefixed stream itch_parser.v expects."""
    from test_itch_parser import REAL_FILE
    if REAL_FILE is None:
        dut._log.info("no capture file; skipping")
        return

    source, sink = await bringup(dut)
    _hdrs, gaps = start_monitor(dut)
    rng = random.Random(7)

    ms = F.split_messages(REAL_FILE.read_bytes()[: 1 << 20])[:300]
    out, i, seq = [], 0, 1
    while i < len(ms):
        n     = rng.randint(1, 6)
        group = ms[i:i + n]
        dg    = F.mold_packet(group, seq=seq)
        out.append(await run_dgram(dut, source, sink, dg))
        i   += n
        seq += n

    expect = b"".join(struct.pack(">H", len(m)) + m for m in ms)
    assert b"".join(out) == expect, "payloads != the BinaryFILE stream"
    assert gaps == [], f"unexpected gaps on in-order capture: {gaps}"
