# Phase 3 - FPGA UDP Packet Parser

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
|   |
│   ├──  3.0  frames.py — wire-frame builder + reference unwrap (stimulus)                  [done]
│   ├──  3.1  axis_skip_align.v — the gearbox: drop N leading bytes, realign to lane 0      [done]
│   ├──  3.2  eth_parser.v      — strip 14 B (+4 VLAN), filter ethertype 0x0800            [done]
│   ├──  3.3  ipv4_parser.v     — IHL skip, proto==17, trim to total_length, checksum      [done]
│   ├──  3.4  udp_parser.v      — strip 8 B, port filter, trim to UDP length
│   ├──  3.5  mold_parser.v     — strip 20 B, sequence-gap strobe, heartbeat / end-of-session
│   └──  3.6  feed_top.v        — eth → ipv4 → udp → mold → itch_parser (unmodified)
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
│   ├── axis_skip_align.v  the gearbox: skip N bytes, realign to lane 0 (3.1)
│   ├── eth_parser.v       Ethernet layer: sniff ethertype, strip 14/18 B (3.2)
│   ├── ipv4_parser.v      IPv4 layer: IHL skip, proto/checksum, trim to total_length (3.3)
│   └── itch_parser.v      streaming ITCH 5.0 message parser
├── tb/                    cocotb testbenches (Python)
│   ├── test_axis_reg.py
│   ├── test_axis_skip_align.py
│   ├── test_eth_parser.py
│   ├── test_ipv4_parser.py
│   ├── test_itch_parser.py
│   ├── frames.py          wire-frame builder + reference unwrap (step-3 stimulus)
│   └── test_frames.py     self-test for frames.py (pure Python, no simulator)
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

## Step 3 - the wire-format front-end

Each subpart below records **why it has to exist** (requirement) and **how it
works**, added as it is built.

### 3.0  frames.py - the wire-frame builder

**Requirement.** The capture files hold the offline BinaryFILE format —
`[2B BE len][body]…` with no network wrapper. Real wire frames do not exist
anywhere in this project, so before a single line of front-end RTL can be
tested, something has to *manufacture* them — and independently say what each
layer owes its downstream. That is stimulus and ground truth in one file, which
is why it is built first: a bug here would be "verified" straight into the
hardware.

**Working.** On a real feed the length-prefixed blocks arrive buried under four
layers of headers:

```
byte  0        14          34      42              62
      ├────────┼───────────┼───────┼───────────────┼──────────────────┐
      │Ethernet│   IPv4    │  UDP  │  MoldUDP64    │ [len][msg][len][msg]…
      │  14 B  │  20 B(*)  │  8 B  │     20 B      │  <-- what itch_parser.v parses
      └────────┴───────────┴───────┴───────────────┴──────────────────┘
       ethertype ver/IHL      dport  session(10)
       =0x0800   proto=17     length seq(8) count(2)
```

MoldUDP64's payload is *exactly* the length-prefixed framing the RTL parser
already consumes, so the front-end never touches `itch_parser.v` — it strips 62
bytes and hands over the rest. `frames.py` builds those frames (`wire_frame`,
`packetize`) and provides the reference unwrap (`unwrap_frame`) that tells each
RTL layer what it owes its downstream.

`test_frames.py` (17 tests, pure Python — no simulator) covers— **wrap → unwrap must return exactly the byte stream
`itch_parser.v` already parses**, on synthetic *and* real capture messages —
plus header field placement, a valid IPv4 checksum, VLAN tags, IP options,
Mold heartbeats / end-of-session, sequence-number advance (the basis for gap
detection), and the four frames that must be dropped (wrong ethertype, wrong
protocol, wrong port, bad checksum).

**A finding from writing it:** Ethernet padding, which the plan listed as a
hazard, *cannot occur on this feed*. The smallest possible datagram is a Mold
heartbeat — 20 (mold) + 8 (UDP) + 20 (IPv4) = **48 bytes**, already above
Ethernet's 46-byte minimum payload. The RTL still trims to IPv4 `total_length`
(that is the generally-correct bound, and the test proves the trim path works),
but the padding case is dead code for this protocol stack. A test asserts the
48-byte floor so a future framing change trips it loudly.

Note the payload begins at **byte 62 — lane 6 of beat 7** on a 64-bit datapath.
Nothing downstream is 8-byte aligned, which is exactly the problem the gearbox
(3.1) exists to solve.

### 3.1  axis_skip_align.v - we'll call it gearbox from now

**Requirement.** Every protocol layer needs the same operation: discard a
*runtime-variable* number of leading bytes, then hand the rest downstream —
starting at lane 0. The byte counts are not known at compile time (IPv4's IHL
makes its header 20–60 B; a VLAN tag adds 4 B to Ethernet), and after any skip
that is not a multiple of 8 the payload is misaligned inside the 64-bit beats.
With the real 62-byte header stack the payload starts at **lane 6 of beat 7**.
Written once, generically, `eth`/`ipv4`/`udp`/`mold` all become thin wrappers
around it — written four times, it would be the same bug four times.

**Working.** A byte-oriented staging buffer, two beats (16 B) wide.

```
  skip=3     in  |a a a b b b b b|   |b b c c . . . .|
  (drop 'a')     └skip─┘                              staging buffer
                 out |b b b b b b b c|  |c . . . . . . .| tlast
                      └── aligned to lane 0 ──┘
```

Each input beat appends its post-skip bytes to the buffer; whenever ≥ 8 bytes
are staged, one fully-aligned beat is emitted and the **residue carries into the
next beat — that carry *is* the realignment**. Backpressure works by only
accepting an input beat when the buffer has room for a whole beat, so a
downstream stall propagates upstream in one cycle.

Three control inputs, sampled on the frame's first beat and held by the layer
above (which has already seen the header bytes it needs):

| Input | Meaning |
|---|---|
| `skip` | leading bytes to discard |
| `keep_len` | payload bytes to emit after the skip; `0` = to end of frame. This is how IPv4 `total_length` bounds the payload. |
| `drop` | filter reject — emit nothing at all |

The whole contract is one line of Python, and that is literally the testbench's
reference model:

```python
out = b"" if drop else frame[skip : skip + keep_len]
```

**A design bug the tests caught.** The first version inferred end-of-frame from
the input's `tlast`. But when `keep_len` truncates the payload, the last output
beat is emitted *before* the input frame ends — so `tlast` never fired and the
sink waited forever (the test hung, rather than failing). The fix is to track
"no more payload bytes can arrive" explicitly (`done_q`, set when the `keep_len`
budget is spent **or** the input ends) and assert `tlast` on whichever output
beat empties the buffer once it holds. Truncation and frame-end are genuinely
different events; conflating them is what broke it.

### 3.2  eth_parser.v - the Ethernet layer

**Requirement.** This is the first block that *reads header bytes and makes a
decision*, instead of being told what to do. Everything before it was blind:
the gearbox obeys a `skip` it is handed. `eth_parser` has to produce that
number itself, and it has two decisions to make:

- **how many bytes to strip** — 14 for a plain Ethernet II frame, but **18** if
  an 802.1Q VLAN tag is present. A tag shows up as ethertype `0x8100` at bytes
  12–13, which pushes the *real* ethertype out to bytes 16–17 and grows the
  header by 4. The strip length is therefore data-dependent, not a constant.
- **whether to keep the frame at all** — the (post-VLAN) ethertype must be
  `0x0800` (IPv4). ARP, IPv6, anything else → drop, emit nothing.

**Working.** It is a thin lookahead FSM wrapped around one `axis_skip_align`
instance — the payoff of building the gearbox first. `eth_parser` decides
`skip` and `drop`; the gearbox does the actual byte-moving. Ethernet does not
trim a tail, so `keep_len = 0` (to end of frame); IPv4 `total_length` handles
trimming one layer up.

```
  wire frame ─► [ eth_parser ]──────────────────────► IPv4 packet
                   │ sniff bytes 12-13 (+16-17 if VLAN tag)
                   │ decide  skip = 14 or 18
                   │         drop = (ethertype != 0x0800)
                   └─► axis_skip_align does the stripping
```

**The wrinkle, and how it is handled.** The gearbox samples `skip`/`drop` on the
*first beat* of a frame — but on a 64-bit datapath the ethertype at byte 12 is
in **beat 1**, and the VLAN's real ethertype at byte 16 is in **beat 2**. The
decision simply is not available when the gearbox would want it. So `eth_parser`
*holds* the opening beats (`S_SNIFF`) while it reads the ethertype, resolves
`skip`/`drop`, then (`S_EMIT`) replays the held beats into the gearbox with the
control word already correct and streams the rest through live. It holds two
beats for a plain frame, three for a VLAN frame — and a runt frame that ends
inside the header is simply dropped. The input stalls for those few cycles per
frame while the held beats drain; the layer is not zero-bubble, but it is
honestly streaming (not store-and-forward), and the small per-frame bubble is a
fair thing to quantify in step 4.

Five tests, all green, checked against `frames.unwrap_eth()`: plain IPv4;
VLAN-tagged (strip 18); ARP/IPv6 dropped, tagged and untagged; a mixed
back-to-back stream with stutter and backpressure; and real capture frames from
`frames.py`. Because the DUT has no control inputs, the reference is entirely
"what should the header say" — the test never tells it the skip, it only checks
the payload.

### 3.3  ipv4_parser.v - the IPv4 layer    [done]

**Requirement.** The richest of the protocol layers: four decisions, all read
out of the IP header, and the first layer to use *every* gearbox control.

- **Variable-length skip.** The low nibble of byte 0 is IHL, the header length
  in 32-bit words; `skip = IHL*4` — 20 bytes normally, up to 60 with IP options.
  This is the reason the gearbox takes a runtime `skip` rather than a constant.
- **Protocol filter.** Byte 9 must be 17 (UDP); TCP/ICMP/anything else → drop.
- **Trim to total_length.** Bytes 2–3 are header+payload length. The payload is
  `total_length - IHL*4` bytes, and that becomes the gearbox `keep_len` — the
  first layer that actually uses it. This is what would strip trailing Ethernet
  padding (harmless on this feed, but the correct bound).
- **Header checksum.** The 16-bit one's-complement sum over the header words must
  fold to `0xFFFF`; a corrupt header → drop.

**Working.** Same hold-and-replay FSM around one `axis_skip_align`, but with more
to resolve during the hold. The catch specific to this layer: the checksum is
not known until the **whole header** has been summed, and how many beats that
spans depends on IHL itself (3 beats for a 20-byte header, up to 8 for a 60-byte
one). So the checksum is accumulated **incrementally, beat by beat**, during
`S_SNIFF` — each header beat contributes its 16-bit words to a running sum — and
only when the last header byte has been folded in is `drop` decided. Because
that all happens before `S_EMIT`, the drop decision is settled before the first
payload byte ever reaches the gearbox.

```
  IP packet ─► [ ipv4_parser ]─────────────────────────► UDP segment
                  │ beat 0 : version, IHL (→ skip), total_length
                  │ beat 1 : protocol (byte 9)
                  │ beats 0..N : accumulate header checksum
                  │ resolve: skip=IHL*4  keep_len=total-IHL*4
                  │          drop = !v4 | IHL<5 | proto!=17 | !csum | ...
                  └─► axis_skip_align strips + trims
```

Six tests, checked against `frames.unwrap_ipv4()`: plain UDP packets; variable
IHL with options (5–15 words); `total_length` trimming of trailing bytes;
rejects for wrong protocol, bad checksum, and wrong version; a mixed stream with
stutter and backpressure; and real capture packets. The checksum accumulation is
the one genuinely new sub-capability in step 3 — everything after this (UDP,
Mold) is a simplification of this block.


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

make                     # default DUT: itch_parser (random-traffic + real-capture tests)
make DUT=axis_reg        # the skid-buffer block instead (used for testing)
make DUT=axis_skip_align # the gearbox (3.1)
make DUT=eth_parser      # the Ethernet layer (3.2)
make DUT=ipv4_parser     # the IPv4 layer (3.3)
make pytests             # pure-Python testbench helpers (frames.py) - no simulator
make regress             # every testbench: pytests + each RTL block
make waves               # open the DUT's VCD dump in gtkwave

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


