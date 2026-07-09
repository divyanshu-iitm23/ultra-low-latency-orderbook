# test_itch_parser.py - verify the RTL ITCH parser against the Phase-1 C++ parser.
#

import os
import random
import struct
import subprocess
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, FallingEdge
from cocotbext.axi import AxiStreamBus, AxiStreamSource

FPGA_DIR   = Path(__file__).resolve().parent.parent
GOLDEN_SRC = FPGA_DIR / "tools" / "itch_dump.cpp"
GOLDEN_HPP = FPGA_DIR.parent / "include" / "itch_parser.hpp"
GOLDEN_BIN = FPGA_DIR / "sim_build" / "itch_dump"

_REAL_CANDIDATES = [os.environ.get("ITCH_FILE"),
                    "~/market-data/sample_50mb.ITCH50",
                    "~/market-data/sample_500mb.ITCH50"]
REAL_FILE = next((Path(p).expanduser() for p in _REAL_CANDIDATES
                  if p and Path(p).expanduser().is_file()), None)


# golden model

def golden_lines(stream: bytes) -> list[str]:
    """Run the framed byte stream through the Phase-1 parser via itch_dump."""
    if (not GOLDEN_BIN.exists()
            or GOLDEN_BIN.stat().st_mtime < GOLDEN_SRC.stat().st_mtime
            or GOLDEN_BIN.stat().st_mtime < GOLDEN_HPP.stat().st_mtime):
        GOLDEN_BIN.parent.mkdir(exist_ok=True)
        subprocess.run(["g++", "-O2", "-std=c++17",
                        str(GOLDEN_SRC), "-o", str(GOLDEN_BIN)], check=True)
    tmp = GOLDEN_BIN.parent / "golden_input.bin"
    tmp.write_bytes(stream)
    out = subprocess.run([str(GOLDEN_BIN), str(tmp)],
                         check=True, capture_output=True, text=True)
    return out.stdout.splitlines()


# synthetic ITCH stimulus (well-formed messages per the 5.0 spec)

def _hdr(typ: bytes) -> bytes:            # type + locate + tracking + timestamp
    return (typ + struct.pack(">HH", random.getrandbits(16), random.getrandbits(16))
            + random.getrandbits(48).to_bytes(6, "big"))

def _stock() -> bytes:
    return "".join(random.choices("ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                                  k=random.randint(1, 8))).ljust(8).encode()

def gen_message() -> bytes:
    r64 = lambda: random.getrandbits(64).to_bytes(8, "big")
    r32 = lambda: random.getrandbits(32).to_bytes(4, "big")
    typ = random.choices("AFECXDUPSRZ",
                         weights=[25, 8, 10, 5, 10, 15, 10, 6, 3, 4, 4])[0]
    if typ in "AF":                        # add order (with MPID for 'F')
        body = (_hdr(typ.encode()) + r64() + random.choice(b"BS").to_bytes(1, "big")
                + r32() + _stock() + r32())
        if typ == "F":
            body += b"MPID"
    elif typ == "E":                       # executed
        body = _hdr(b"E") + r64() + r32() + r64()
    elif typ == "C":                       # executed with price
        body = _hdr(b"C") + r64() + r32() + r64() + b"Y" + r32()
    elif typ == "X":                       # cancel (partial)
        body = _hdr(b"X") + r64() + r32()
    elif typ == "D":                       # delete
        body = _hdr(b"D") + r64()
    elif typ == "U":                       # replace
        body = _hdr(b"U") + r64() + r64() + r32() + r32()
    elif typ == "P":                       # trade -- unmodeled by the golden parser
        body = (_hdr(b"P") + r64() + b"B" + r32() + _stock() + r32() + r64())
    elif typ == "S":                       # system event
        body = _hdr(b"S") + b"O"
    elif typ == "R":                       # stock directory
        body = _hdr(b"R") + _stock() + random.randbytes(20)
    else:                                  # unknown type, random length
        body = _hdr(b"Z") + random.randbytes(random.randint(1, 39))
    return struct.pack(">H", len(body)) + body


# real-capture slicing

def real_slice(path: Path, n_prefix: int, n_orders: int) -> bytes:
    """First n_prefix messages, plus n_orders starting at the first 'A'."""
    buf = path.read_bytes()[: 16 * 1024 * 1024]
    frames, first_add, off = [], None, 0
    while off + 2 <= len(buf):
        (mlen,) = struct.unpack_from(">H", buf, off)
        if mlen == 0 or off + 2 + mlen > len(buf):
            break
        if first_add is None and buf[off + 2] == ord("A"):
            first_add = len(frames)
        frames.append(buf[off : off + 2 + mlen])
        off += 2 + mlen
        if first_add is not None and len(frames) >= first_add + n_orders \
                and len(frames) >= n_prefix:
            break
    assert first_add is not None, "no Add Order found in capture prefix"
    return b"".join(frames[:n_prefix] + frames[first_add : first_add + n_orders])


# DUT side

def fmt(dut) -> str:                       # must match itch_dump.cpp's printf
    return ("typ={:02x} mod={} ts={} ref={} nref={} side={:02x} sh={} px={} "
            "stock={:016x}").format(
        int(dut.msg_type.value), int(dut.msg_modeled.value),
        int(dut.msg_timestamp.value), int(dut.msg_order_ref.value),
        int(dut.msg_new_order_ref.value), int(dut.msg_side.value),
        int(dut.msg_shares.value), int(dut.msg_price.value),
        int(dut.msg_stock.value))


async def bringup(dut):
    source = AxiStreamSource(AxiStreamBus.from_prefix(dut, "s_axis"),
                             dut.clk, dut.rst)
    cocotb.start_soon(Clock(dut.clk, 8, "ns").start())    # 125 MHz
    dut.rst.value = 1
    await ClockCycles(dut.clk, 5)
    dut.rst.value = 0
    await ClockCycles(dut.clk, 2)
    return source


async def run_stream(dut, source, stream: bytes) -> list[str]:
    got = []

    async def monitor():
        while True:
            await FallingEdge(dut.clk)
            if dut.msg_valid.value:
                got.append(fmt(dut))

    task = cocotb.start_soon(monitor())
    await source.send(stream)
    await source.wait()
    await ClockCycles(dut.clk, 20)         # drain the last message
    task.cancel()
    return got


def compare(got: list[str], exp: list[str]):
    assert len(got) == len(exp), f"message count: dut={len(got)} golden={len(exp)}"
    for i, (g, e) in enumerate(zip(got, exp)):
        assert g == e, f"message {i} mismatch:\n  dut:    {g}\n  golden: {e}"


# tests

@cocotb.test()
async def random_traffic(dut):
    """1200 random messages (all modeled types + unmodeled + unknown),
    source stutter on, diffed line-for-line against the Phase-1 parser."""
    source = await bringup(dut)

    def stutter():
        while True:
            yield random.random() < 0.2
    source.set_pause_generator(stutter())

    stream = b"".join(gen_message() for _ in range(1200))
    exp = golden_lines(stream)
    got = await run_stream(dut, source, stream)
    compare(got, exp)


@cocotb.test(skip=REAL_FILE is None)
async def real_capture(dut):
    """Real Nasdaq capture: session-start prefix (S/R/...) plus a window of
    real order flow starting at the first Add Order."""
    source = await bringup(dut)
    stream = real_slice(REAL_FILE, n_prefix=1500, n_orders=1500)
    exp = golden_lines(stream)
    got = await run_stream(dut, source, stream)
    compare(got, exp)
    modeled = sum(" mod=1 " in ln for ln in exp)
    dut._log.info(f"real capture: {len(exp)} messages, {modeled} modeled")
