# test_frames.py - self-test for the wire-frame builder (pure Python, no simulator).
#

import random
import struct

import pytest

import frames as F


def msgs(n, rng):
    """n raw ITCH bodies of assorted plausible lengths."""
    return [bytes([rng.randrange(0x41, 0x5B)]) + rng.randbytes(ln - 1)
            for ln in (rng.choice([19, 23, 31, 35, 36, 40]) for _ in range(n))]


# the headline property

def test_roundtrip_matches_binaryfile_stream():
    """wrap -> unwrap == the length-prefixed stream the ITCH parser expects."""
    rng = random.Random(1)
    ms  = msgs(200, rng)
    fr, expect = F.packetize(ms, per_packet=(1, 6), rng=rng)
    assert b"".join(F.unwrap_frame(f) for f in fr) == expect


def test_expected_stream_is_exactly_the_capture_framing():
    ms = msgs(20, random.Random(2))
    _, expect = F.packetize(ms, rng=random.Random(2))
    assert expect == b"".join(struct.pack(">H", len(m)) + m for m in ms)


def test_split_messages_inverts_the_framing():
    ms = msgs(50, random.Random(3))
    framed = b"".join(struct.pack(">H", len(m)) + m for m in ms)
    assert F.split_messages(framed) == ms


# header field placement

def test_layer_offsets_are_where_the_rtl_will_look():
    f = F.wire_frame([b"A" * 34], seq=7)
    assert struct.unpack_from(">H", f, 12)[0] == F.ETHERTYPE_IPV4
    assert f[14] >> 4 == 4                       # IPv4 version
    assert f[14] & 0x0F == 5                     # IHL = 5 words = 20 bytes
    assert f[14 + 9] == F.IP_PROTO_UDP           # protocol == 17
    assert struct.unpack_from(">H", f, 34 + 2)[0] == F.FEED_PORT
    assert f[42:52] == F.MOLD_SESSION            # Mold session id
    seq, count = struct.unpack_from(">QH", f, 52)
    assert (seq, count) == (7, 1)
    # payload starts at byte 62 -- lane 6 of beat 7, the misalignment the gearbox fixes
    assert F.unwrap_frame(f) == struct.pack(">H", 34) + b"A" * 34


def test_ipv4_checksum_is_valid():
    f = F.wire_frame([b"D" * 17], seq=1)
    assert F.ones_complement_sum(f[14:34]) == 0  # a correct header sums to zero


# the cases the RTL must survive

def test_mold_frames_never_need_ethernet_padding():
    """On THIS feed padding can't happen: the smallest possible datagram is a
    Mold heartbeat -- 20 (mold) + 8 (udp) + 20 (ip) = 48 B -- already above
    Ethernet's 46 B minimum payload. Good news for the RTL; assert it so a
    future framing change (smaller header, no Mold) trips this test loudly."""
    smallest = F.ipv4(F.udp(F.mold_heartbeat(seq=1)))
    assert len(smallest) == 48 >= F.ETH_MIN_PAYLOAD

    f = F.wire_frame([b"D" * 17], seq=1)         # smallest real ITCH message
    assert len(f) > 14 + F.ETH_MIN_PAYLOAD       # no padding was added
    assert F.unwrap_frame(f) == struct.pack(">H", 17) + b"D" * 17


def test_padding_is_trimmed_when_it_does_occur():
    """The trim path itself still has to be right -- the RTL uses IPv4
    total_length to bound the payload, not the frame size. Force the case with
    a deliberately tiny (non-Mold) UDP payload."""
    tiny = b"12345"
    f = F.eth(F.ipv4(F.udp(tiny)))
    assert len(f) == 14 + F.ETH_MIN_PAYLOAD      # padding actually happened here
    assert F.unwrap_udp(F.unwrap_ipv4(F.unwrap_eth(f))) == tiny   # padding not leaked


def test_vlan_tag_shifts_everything_by_four():
    f = F.wire_frame([b"A" * 34], seq=1, vlan=100)
    assert struct.unpack_from(">H", f, 12)[0] == F.ETHERTYPE_VLAN
    assert struct.unpack_from(">H", f, 16)[0] == F.ETHERTYPE_IPV4
    assert F.unwrap_frame(f) == struct.pack(">H", 34) + b"A" * 34


def test_ip_options_shift_the_payload():
    f = F.wire_frame([b"A" * 34], seq=1, ihl_words=7)     # 28-byte IP header
    assert f[14] & 0x0F == 7
    assert F.ones_complement_sum(f[14:14 + 28]) == 0
    assert F.unwrap_frame(f) == struct.pack(">H", 34) + b"A" * 34


@pytest.mark.parametrize("kw", [
    {"ethertype": 0x0806},        # ARP, not IPv4
    {"proto": 6},                 # TCP, not UDP
    {"dport": 9999},              # wrong port
    {"bad_checksum": True},       # corrupt IPv4 header checksum
])
def test_frames_that_must_be_dropped(kw):
    assert F.unwrap_frame(F.wire_frame([b"A" * 34], seq=1, **kw)) == b""


@pytest.mark.parametrize("dgram", [
    F.mold_heartbeat(seq=5),
    F.mold_end_of_session(seq=5),
])
def test_mold_packets_carrying_no_messages(dgram):
    """Heartbeat (count=0) and end-of-session (count=0xFFFF) emit nothing."""
    f = F.eth(F.ipv4(F.udp(dgram)))
    assert F.unwrap_frame(f) == b""


def test_sequence_numbers_advance_by_message_count():
    """seq of a packet == seq of previous + its message count (gap detection basis)."""
    ms = msgs(30, random.Random(4))
    fr, _ = F.packetize(ms, per_packet=(2, 5), seq0=100, rng=random.Random(5))
    expect = 100
    for f in fr:
        seq, count, _ = F.unwrap_mold(F.unwrap_udp(F.unwrap_ipv4(F.unwrap_eth(f))))
        assert seq == expect
        expect += count
    assert expect == 100 + len(ms)


# real capture

def test_real_capture_roundtrips():
    """Wrap real Nasdaq messages into wire frames and unwrap them back."""
    from test_itch_parser import REAL_FILE
    if REAL_FILE is None:
        pytest.skip("no ITCH capture file present")

    raw = REAL_FILE.read_bytes()[: 1 << 20]
    ms  = F.split_messages(raw)[:500]
    assert ms, "no messages parsed from capture"

    rng = random.Random(6)
    fr, expect = F.packetize(ms, per_packet=(1, 8), rng=rng)
    assert b"".join(F.unwrap_frame(f) for f in fr) == expect
