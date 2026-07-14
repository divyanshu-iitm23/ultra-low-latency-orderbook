# test_axis_skip_align.py - the gearbox (3.1)
#

import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles
from cocotbext.axi import AxiStreamBus, AxiStreamSource, AxiStreamSink


def expected(frame: bytes, skip: int, keep_len: int, drop: bool) -> bytes:
    """The whole contract of this block."""
    if drop:
        return b""
    body = frame[skip:]
    return body[:keep_len] if keep_len else body


async def bringup(dut):
    source = AxiStreamSource(AxiStreamBus.from_prefix(dut, "s_axis"), dut.clk, dut.rst)
    sink   = AxiStreamSink(AxiStreamBus.from_prefix(dut, "m_axis"), dut.clk, dut.rst)
    cocotb.start_soon(Clock(dut.clk, 8, "ns").start())          # 125 MHz
    dut.skip.value = 0
    dut.keep_len.value = 0
    dut.drop.value = 0
    dut.rst.value = 1
    await ClockCycles(dut.clk, 5)
    dut.rst.value = 0
    await ClockCycles(dut.clk, 2)
    return source, sink


async def run_frame(dut, source, sink, frame, skip, keep_len=0, drop=False):
    """Drive one frame with its control word; return the bytes the DUT emitted."""
    dut.skip.value     = skip
    dut.keep_len.value = keep_len
    dut.drop.value     = 1 if drop else 0

    await source.send(frame)
    await source.wait()
    await ClockCycles(dut.clk, 30)               # let the tail drain

    exp = expected(frame, skip, keep_len, drop)
    if not exp:
        assert sink.empty(), "dropped/empty frame should emit nothing"
        return b""
    rx = await sink.recv()
    return bytes(rx.tdata)


@cocotb.test()
async def every_skip_alignment(dut):
    """skip 0..15 x a range of frame lengths: the core realignment property."""
    source, sink = await bringup(dut)
    rng = random.Random(1)

    for skip in range(16):
        for length in (skip + 1, skip + 8, skip + 9, skip + 17, 64, 100):
            if length <= skip:
                continue
            frame = rng.randbytes(length)
            got = await run_frame(dut, source, sink, frame, skip)
            exp = expected(frame, skip, 0, False)
            assert got == exp, (f"skip={skip} len={length}\n"
                                f"  got {got.hex()}\n  exp {exp.hex()}")


@cocotb.test()
async def real_header_skip_62(dut):
    """The case this block exists for: Eth(14)+IPv4(20)+UDP(8)+Mold(20) = 62,
    i.e. the payload starts at lane 6 of beat 7. Nothing is 8-byte aligned."""
    source, sink = await bringup(dut)
    rng = random.Random(2)

    for payload_len in (2, 18, 36, 37, 64, 130, 255):
        payload = rng.randbytes(payload_len)
        frame   = rng.randbytes(62) + payload
        got = await run_frame(dut, source, sink, frame, 62)
        assert got == payload, f"payload_len={payload_len}"


@cocotb.test()
async def keep_len_trims_the_tail(dut):
    """keep_len bounds the payload -- this is how IPv4 total_length would trim
    trailing Ethernet padding (even though this feed never pads; see frames.py)."""
    source, sink = await bringup(dut)
    rng = random.Random(3)

    for keep in (1, 7, 8, 9, 23, 40):
        payload = rng.randbytes(48)
        frame   = rng.randbytes(14) + payload + b"\x00" * 20     # trailing padding
        got = await run_frame(dut, source, sink, frame, 14, keep_len=keep)
        assert got == payload[:keep], f"keep_len={keep}"


@cocotb.test()
async def drop_emits_nothing(dut):
    """A filter reject must produce no output beats at all."""
    source, sink = await bringup(dut)
    rng = random.Random(4)

    for _ in range(8):
        frame = rng.randbytes(rng.randint(20, 120))
        await run_frame(dut, source, sink, frame, 14, drop=True)
    assert sink.empty()


@cocotb.test()
async def back_to_back_frames_keep_their_own_control(dut):
    """Consecutive frames with different skip/keep must not bleed into each other."""
    source, sink = await bringup(dut)
    rng = random.Random(5)

    cases = [(rng.randbytes(rng.randint(30, 90)),
              rng.randint(0, 15),
              rng.choice([0, 0, 5, 12, 20])) for _ in range(20)]

    for frame, skip, keep in cases:
        if len(frame) <= skip:
            continue
        got = await run_frame(dut, source, sink, frame, skip, keep_len=keep)
        assert got == expected(frame, skip, keep, False), f"skip={skip} keep={keep}"


@cocotb.test()
async def stress_with_stutter_and_backpressure(dut):
    """Random everything, with both sides randomly pausing."""
    source, sink = await bringup(dut)
    rng = random.Random(6)

    def pause():
        while True:
            yield rng.random() < 0.3
    source.set_pause_generator(pause())
    sink.set_pause_generator(pause())

    for _ in range(60):
        skip   = rng.randint(0, 15)
        length = rng.randint(skip + 1, skip + 200)
        keep   = rng.choice([0, 0, 0, rng.randint(1, 60)])
        frame  = rng.randbytes(length)
        got = await run_frame(dut, source, sink, frame, skip, keep_len=keep)
        assert got == expected(frame, skip, keep, False), (
            f"skip={skip} len={length} keep={keep}\n  got {got.hex()}\n"
            f"  exp {expected(frame, skip, keep, False).hex()}")


@cocotb.test()
async def wire_frames_from_frames_py(dut):
    """End-to-end sanity with the real builder: strip the 62-byte header stack
    off actual wire frames and get back exactly the length-prefixed blocks."""
    import frames as F

    source, sink = await bringup(dut)
    rng = random.Random(7)

    msgs = [bytes([rng.randrange(0x41, 0x5B)]) + rng.randbytes(n - 1)
            for n in (rng.choice([19, 23, 31, 36]) for _ in range(40))]
    wire, _ = F.packetize(msgs, per_packet=(1, 4), rng=rng)

    for frame in wire:
        got = await run_frame(dut, source, sink, frame, skip=62)
        assert got == F.unwrap_frame(frame), "gearbox output != reference unwrap"
