# frames.py - wire-frame builder for the step-3 front-end testbenches.
#

import struct

ETHERTYPE_IPV4 = 0x0800
ETHERTYPE_VLAN = 0x8100
IP_PROTO_UDP   = 17

MOLD_SESSION   = b"ORDERBOOK1"          # exactly 10 bytes
FEED_PORT      = 26477                  # arbitrary; the RTL filters on it

ETH_MIN_PAYLOAD = 46                    # short frames are zero-padded on the wire


# checksums

def ones_complement_sum(data: bytes) -> int:
    """16-bit one's-complement sum (RFC 1071), used by the IPv4 header checksum."""
    if len(data) % 2:
        data += b"\x00"
    total = 0
    for i in range(0, len(data), 2):
        total += (data[i] << 8) | data[i + 1]
        total = (total & 0xFFFF) + (total >> 16)      # fold carries
    return (~total) & 0xFFFF


# layer builders (innermost first)

def mold_packet(messages: list[bytes], seq: int, session: bytes = MOLD_SESSION) -> bytes:
    """MoldUDP64: 10B session + 8B seq + 2B count, then [2B BE len][body] blocks.

    `messages` are raw ITCH bodies (NO length prefix) -- the length blocks are
    added here. seq is the sequence number of the FIRST message in the packet.
    """
    assert len(session) == 10, "MoldUDP64 session is 10 bytes"
    assert len(messages) <= 0xFFFF
    hdr  = session + struct.pack(">QH", seq, len(messages))
    body = b"".join(struct.pack(">H", len(m)) + m for m in messages)
    return hdr + body


def mold_heartbeat(seq: int, session: bytes = MOLD_SESSION) -> bytes:
    """Count == 0: a heartbeat. Carries no messages; the front-end must emit nothing."""
    return session + struct.pack(">QH", seq, 0)


def mold_end_of_session(seq: int, session: bytes = MOLD_SESSION) -> bytes:
    """Count == 0xFFFF: end of session marker. Also carries no message bodies."""
    return session + struct.pack(">QH", seq, 0xFFFF)


def udp(payload: bytes, sport: int = 4000, dport: int = FEED_PORT) -> bytes:
    """8-byte UDP header. Checksum 0 = 'not computed', which is legal over IPv4."""
    return struct.pack(">HHHH", sport, dport, 8 + len(payload), 0) + payload


def ipv4(payload: bytes, src="10.0.0.1", dst="239.1.1.1",
         ihl_words: int = 5, proto: int = IP_PROTO_UDP,
         bad_checksum: bool = False) -> bytes:
    """20-byte IPv4 header (or longer via ihl_words -> NOP options).

    total_length covers header + payload; the RTL uses it to trim Ethernet
    padding, so it must be exact.
    """
    assert 5 <= ihl_words <= 15
    opts     = b"\x01" * ((ihl_words - 5) * 4)        # NOP option bytes
    hdr_len  = ihl_words * 4
    total    = hdr_len + len(payload)
    ver_ihl  = (4 << 4) | ihl_words
    saddr    = bytes(int(x) for x in src.split("."))
    daddr    = bytes(int(x) for x in dst.split("."))

    fixed = struct.pack(">BBHHHBBH", ver_ihl, 0, total, 0, 0, 64, proto, 0) \
            + saddr + daddr
    csum  = ones_complement_sum(fixed + opts)
    if bad_checksum:
        csum ^= 0xFFFF                                 # corrupt it deliberately
    hdr = fixed[:10] + struct.pack(">H", csum) + fixed[12:] + opts
    return hdr + payload


def eth(payload: bytes, ethertype: int = ETHERTYPE_IPV4,
        vlan: int | None = None,
        dst=b"\x01\x00\x5e\x01\x01\x01", src=b"\x02\x00\x00\x00\x00\x01") -> bytes:
    """14-byte Ethernet II header (+4 if VLAN-tagged), zero-padded to 46B payload."""
    hdr = dst + src
    if vlan is not None:
        hdr += struct.pack(">HH", ETHERTYPE_VLAN, vlan)
    hdr += struct.pack(">H", ethertype)
    if len(payload) < ETH_MIN_PAYLOAD:                # the wire pads; the RTL must trim
        payload = payload + b"\x00" * (ETH_MIN_PAYLOAD - len(payload))
    return hdr + payload


def wire_frame(messages: list[bytes], seq: int, **kw) -> bytes:
    """Full stack: ITCH bodies -> Mold -> UDP -> IPv4 -> Ethernet."""
    eth_kw  = {k: kw.pop(k) for k in ("vlan", "ethertype") if k in kw}
    ip_kw   = {k: kw.pop(k) for k in ("ihl_words", "proto", "bad_checksum") if k in kw}
    udp_kw  = {k: kw.pop(k) for k in ("sport", "dport") if k in kw}
    assert not kw, f"unknown kwargs: {list(kw)}"
    return eth(ipv4(udp(mold_packet(messages, seq), **udp_kw), **ip_kw), **eth_kw)


# splitting a message stream into datagrams

def split_messages(framed: bytes) -> list[bytes]:
    """BinaryFILE stream -> list of raw ITCH bodies (length prefixes removed)."""
    out, off = [], 0
    while off + 2 <= len(framed):
        (n,) = struct.unpack_from(">H", framed, off)
        if n == 0 or off + 2 + n > len(framed):
            break
        out.append(framed[off + 2 : off + 2 + n])
        off += 2 + n
    return out


def packetize(messages: list[bytes], per_packet=(1, 6), seq0: int = 1,
              rng=None, **kw) -> tuple[list[bytes], bytes]:
    """Group messages into Mold datagrams -> wire frames.

    Returns (frames, expected_stream) where expected_stream is the
    length-prefixed byte stream the front-end must hand to itch_parser.v --
    i.e. exactly what the parser would have seen from the capture file.
    """
    import random as _random
    rng = rng or _random
    lo, hi = per_packet

    frames, expect, seq, i = [], [], seq0, 0
    while i < len(messages):
        n     = rng.randint(lo, hi)
        group = messages[i : i + n]
        frames.append(wire_frame(group, seq, **kw))
        expect += [struct.pack(">H", len(m)) + m for m in group]
        seq += len(group)
        i   += n
    return frames, b"".join(expect)


# reference unwrap

def unwrap_eth(frame: bytes) -> bytes | None:
    """Ethernet payload, or None if not IPv4. Handles a VLAN tag."""
    if len(frame) < 14:
        return None
    et, off = struct.unpack_from(">H", frame, 12)[0], 14
    if et == ETHERTYPE_VLAN:
        if len(frame) < 18:
            return None
        et, off = struct.unpack_from(">H", frame, 16)[0], 18
    return frame[off:] if et == ETHERTYPE_IPV4 else None


def unwrap_ipv4(pkt: bytes, check: bool = True) -> bytes | None:
    """IPv4 payload, trimmed to total_length (this is what removes Eth padding)."""
    if len(pkt) < 20 or (pkt[0] >> 4) != 4:
        return None
    ihl = (pkt[0] & 0x0F) * 4
    if ihl < 20 or len(pkt) < ihl:
        return None
    total = struct.unpack_from(">H", pkt, 2)[0]
    if pkt[9] != IP_PROTO_UDP or total < ihl or total > len(pkt):
        return None
    if check and ones_complement_sum(pkt[:ihl]) != 0:
        return None
    return pkt[ihl:total]


def unwrap_udp(seg: bytes, dport: int = FEED_PORT) -> bytes | None:
    """UDP payload, trimmed to the UDP length field."""
    if len(seg) < 8:
        return None
    dp, ln = struct.unpack_from(">HH", seg, 2)
    if dp != dport or ln < 8 or ln > len(seg):
        return None
    return seg[8:ln]


def unwrap_mold(dgram: bytes) -> tuple[int, int, bytes] | None:
    """(seq, count, payload) -- payload is the length-prefixed message blocks."""
    if len(dgram) < 20:
        return None
    seq, count = struct.unpack_from(">QH", dgram, 10)
    return seq, count, dgram[20:]


def unwrap_frame(frame: bytes) -> bytes:
    """Full reference unwrap: wire frame -> length-prefixed ITCH blocks (b'' if dropped)."""
    p = unwrap_eth(frame)
    if p is None:
        return b""
    p = unwrap_ipv4(p)
    if p is None:
        return b""
    p = unwrap_udp(p)
    if p is None:
        return b""
    m = unwrap_mold(p)
    if m is None:
        return b""
    seq, count, payload = m
    return b"" if count in (0, 0xFFFF) else payload
