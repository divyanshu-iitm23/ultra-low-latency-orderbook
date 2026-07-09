# Phase 3 — FPGA UDP Packet Parser

Hardware study: a streaming Verilog parser for the Nasdaq feed, verified in
simulation against the Phase-1 C++ parser.

## Roadmap

```
PHASE 3: FPGA UDP Packet PARSER (Hardware)
│
├── 1: Toolchain + first verified block
|   |
│   ├──  Icarus Verilog + gtkwave + cocotb/cocotbext-axi venv
│   └──  AXI-Stream register slice (skid buffer) + random-traffic TB
│
├── 2: ITCH message parser in RTL
|   |
│   ├──  length-prefixed framing (BinaryFILE format, same as the Phase-1 capture)
│   ├──  cross-beat field extraction (message fields straddling 64-bit beats)
│   └──  golden-model scoreboard vs the Phase-1 C++ parser — random + real capture
│
├── 3: Wire-format front-end — MoldUDP64 / UDP / IP / Ethernet
│
└── 4: FPGA vs software comparison
    |
    ├──  deterministic parse latency in cycles vs software ns-with-variance
    └──  synthesis numbers (Yosys / Vivado: Fmax, LUT/FF usage)
```

Note the ordering: the ITCH-layer parser comes before the network front-end.
The capture file used throughout this project is the offline BinaryFILE format
(length-prefixed messages, no Ethernet/IP/UDP/Mold wrapper), so an ITCH-first
parser can be verified against real data immediately; the wire-format layers
are added in front of it afterwards, with the testbench synthesizing the frames.

## Layout

```
fpga/
├── Makefile               cocotb entry point (SIM=icarus, DUT-parametrized)
├── rtl/                   Verilog sources (the DUTs)
│   ├── axis_reg.v         AXI-Stream register slice (skid buffer) — first block
│   └── itch_parser.v      streaming ITCH 5.0 message parser
├── tb/                    cocotb testbenches (Python)
│   ├── test_axis_reg.py
│   └── test_itch_parser.py
└── tools/
    └── itch_dump.cpp      golden-model dump (wraps the Phase-1 include/itch_parser.hpp)
```

## What each block does

### axis_reg - register slice (skid buffer)

One pipeline stage: registers `tdata`/`tkeep`/`tlast` so timing paths break at
the registers. Because valid/ready is a same-cycle contract, the cycle downstream
deasserts `tready` one beat has already been accepted — the skid register catches
exactly that beat, so nothing is ever dropped. Testbench: random-length frames
under ~30% source stutter plus random sink backpressure; every byte must come
out unchanged and in order.

### itch_parser - streaming ITCH 5.0 decoder

Input is the continuous BinaryFILE byte stream (2-byte big-endian length before
each message) on 64-bit AXI-Stream. `tready` is tied high: the parser consumes
8 bytes every cycle and never stalls — line-rate by construction.

```
 s_axis (64-bit beats)      ┌─────────────────────────┐   msg_valid (1-cycle strobe)
 ──────────────────────>    │  LEN0 → LEN1 → BODY fsm │──> msg_type · msg_modeled
   tready tied high         │  walks 8 byte lanes/clk │    timestamp · order_ref
   8 bytes/cycle            │  accumulate → decode    │    new_order_ref · side
                            └─────────────────────────┘    shares · price · stock
```

- A small FSM walks all 8 byte lanes of a beat inside one clock, so message
  fields straddling beats fall out naturally: bytes accumulate into a 50-byte
  message buffer, and decode is one combinational mux off the completed buffer.
- Decodes the seven types the Phase-1 software parser models (`A F E C X D U`)
  at the same byte offsets, and mirrors its edge cases exactly (truncated
  modeled types, unknown types), so the output is bit-identical to software.
- At most one message can complete per beat: the smallest ITCH 5.0 message is
  12 bytes plus the 2-byte length, larger than one 8-byte beat.

### The golden-model scoreboard

The reference is the Phase-1 parser itself: `tools/itch_dump.cpp` includes
`include/itch_parser.hpp`, decodes the identical byte stream, and prints one
canonical line per message. The testbench compiles it on demand, formats the
DUT's outputs the same way, and diffs line-for-line.

| Test | Stimulus |
|---|---|
| `random_traffic` | 1200 random messages — all modeled types plus unmodeled (`P` `S` `R`) and unknown, with source stutter |
| `real_capture` | real Nasdaq capture: session-start prefix + 1500 messages from the first Add Order (3000 messages, 1499 modeled) |

The scoreboard caught a real beat-boundary bug on its first run: when a message
ends mid-beat, the remaining byte lanes of that beat already carry the next
message's first bytes, so the message buffer must be snapshotted at the
completion lane — not after the lane loop. The symptom (type byte showed the
NEXT message's type while timestamp/ref were correct) appeared on both random
and real data.

## Toolchain

| Tool | Role | Install |
|---|---|---|
| Icarus Verilog | simulator | `sudo apt install iverilog` |
| gtkwave | waveform viewer | `sudo apt install gtkwave` |
| cocotb + cocotbext-axi | Python testbench + AXI-Stream bus models | in the venv below |

```sh
python3 -m venv ~/.venvs/fpga
source ~/.venvs/fpga/bin/activate
pip install cocotb cocotbext-axi pytest
```

## Run

```sh
source ~/.venvs/fpga/bin/activate

make               # default DUT: itch_parser (random-traffic + real-capture tests)
make DUT=axis_reg  # the skid-buffer block instead (used for testing)
make regress       # every testbench
make waves         # open the DUT's VCD dump in gtkwave

# point the real-capture test at any BinaryFILE-format .ITCH50 file
ITCH_FILE=~/market-data/sample_50mb.ITCH50 make
```

The golden-model tool is compiled automatically by the testbench (plain `g++`,
no CMake). `real_capture` looks for `~/market-data/sample_50mb.ITCH50` (then
the 500 MB sample) and is skipped if no capture file is present.

### Note: We are not running a whole ITCH file through the simulator

The RTL can parse a real `.ITCH50` file — that is exactly what `real_capture`
does — but only in small slices. The limit is the *simulator*, not the parser.
Icarus is an event-driven interpreter evaluating the design in software, so it
runs at roughly a thousand messages of simulated traffic per second of wall
clock — five to six orders of magnitude slower than the hardware it models.

| Input | Messages | Approx. sim wall time |
|---|---|---|
| `real_capture` slice | 3,000 | ~3 s |
| `sample_50mb.ITCH50` | ~1.4 M | ~25–40 min |
| full trading-day file (multi-GB) | ~230 M | days — not practical |

This is normal for RTL verification: correctness is proven on representative
slices (random traffic plus real-capture windows), diffed bit-for-bit against
the Phase-1 parser — the same code that already crunched 13.8 M messages full-
file in software. Simulating the whole file would take days and prove nothing
new about correctness. Whole-file throughput is the software parser's job; the
hardware's payoff is elsewhere — deterministic per-message latency in fixed
clock cycles, and, on a real FPGA at 156 MHz, chewing through that same multi-GB
day in about six seconds. Those are the step-4 claims, measured from cycle
counts and synthesis reports, not from a long simulation run.


