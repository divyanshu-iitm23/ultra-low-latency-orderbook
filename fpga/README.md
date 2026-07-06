# Phase 3 — FPGA ITCH parser (simulation-only)

Hardware study: a streaming Verilog parser for the Nasdaq feed, verified in
simulation against the Phase-1 C++ parser.

## Roadmap and status

```
PHASE 3: FPGA ITCH PARSER (Hardware, simulation-only)
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
│   └──  golden-model scoreboard vs the Phase-1 C++ parser
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
├── Makefile        cocotb entry point (SIM=icarus)
├── rtl/            
│   └── axis_reg.v  AXI-Stream register slice (skid buffer) — first block
└── tb/             cocotb testbenches (Python)
    └── test_axis_reg.py
```

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
make          # compile + simulate, prints the cocotb PASS/FAIL table
make waves    # open the VCD dump in gtkwave
```

## Conventions

- RTL is plain Verilog (no SystemVerilog), flat AXI-Stream ports
  (`tdata`/`tkeep`/`tvalid`/`tready`/`tlast`), 64-bit datapath, synchronous
  active-high reset.
- Testbenches are cocotb; cocotbext-axi drives/monitors the stream so tests
  are in byte frames.
- Bring-up order matters: construct the bus models (they drive DUT inputs
  to 0) before releasing reset, or the first clock edge samples Z.
