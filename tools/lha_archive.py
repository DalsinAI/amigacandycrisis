#!/usr/bin/env python3
"""LHA archives for the Amiga: written and read, without outside tools.

For ACKitchen's Aminet packager (We, 3 October 2026: "aminet packager
should be first"). Aminet takes LhA archives and asks that they pass UNIX
LhA; these are written as Amiga LhA writes them:

  * level-1 headers with OS id 'A' (Amiga), paths with '/';
  * the Amiga protection bits (HSPARWED) in the attribute byte, and a file
    comment after the name, separated by a NUL (Amiga LhA's convention);
  * -lh5- compression (LZSS over an 8 KB window, static Huffman blocks, the
    method LHA has always used), or -lh0- when packing does not help;
  * a CRC-16 (the ARC polynomial) of each file's bytes.

The compressor follows the published lh5 format (Haruhiko Okumura's ar002
design); the code here is our own. read() exists so tests can check the
writer without UNIX LhA; tests also run lha/lhasa/7z when the host has them.

Copied unchanged (apart from this note) from DalsinAI/amicradle
scripts/lha_archive.py at 95235b62dc3c971165af03c1113b3773f5159c74, so the
package is made without a checkout of AmiCradle. MIT, Copyright (c) 2026
AmiCradle Team.
"""
from __future__ import annotations

import struct
import time
from dataclasses import dataclass
from pathlib import Path

# ---------------------------------------------------------------- CRC-16

def _crc_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
        table.append(c)
    return table


_CRC = _crc_table()


def crc16(data: bytes, crc: int = 0) -> int:
    for b in data:
        crc = (crc >> 8) ^ _CRC[(crc ^ b) & 0xFF]
    return crc


# ---------------------------------------------------------------- bits

class _BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, n: int, value: int):
        for i in range(n - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc = self.n = 0

    def flush(self) -> bytes:
        if self.n:
            self.out.append(self.acc << (8 - self.n))
            self.acc = self.n = 0
        return bytes(self.out)


class _BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0          # in bits

    def get(self, n: int) -> int:
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3] if (self.pos >> 3) < len(self.data) else 0
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def peek(self, n: int) -> int:
        saved = self.pos
        v = self.get(n)
        self.pos = saved
        return v


# ---------------------------------------------------------------- lh5

DICBIT = 13
DICSIZ = 1 << DICBIT
MAXMATCH = 256
THRESHOLD = 3
NC = 255 + MAXMATCH + 2 - THRESHOLD      # 510: literals and match lengths
NP = DICBIT + 1                           # 14 position classes
NT = 19                                   # code-length codes
CBIT, PBIT, TBIT = 9, 4, 5
BLOCK_TOKENS = 16384                      # tokens per Huffman block (fits the 16-bit count)


def _code_lengths(freq: list[int], limit: int = 16) -> list[int]:
    """Huffman code lengths for the symbols with nonzero frequency, no longer
    than `limit` (package-merge would be exact; this rescales, as LHA does)."""
    import heapq
    syms = [i for i, f in enumerate(freq) if f]
    lengths = [0] * len(freq)
    if not syms:
        return lengths
    if len(syms) == 1:
        lengths[syms[0]] = 1
        return lengths
    work = list(freq)
    while True:
        heap = [(work[s], s, None) for s in syms]
        heapq.heapify(heap)
        parent: dict = {}
        node_id = len(freq)
        while len(heap) > 1:
            f1, a, _ = heapq.heappop(heap)
            f2, b, _ = heapq.heappop(heap)
            parent[a] = node_id
            parent[b] = node_id
            heapq.heappush(heap, (f1 + f2, node_id, None))
            node_id += 1
        depth_of: dict = {}

        def depth(x):
            if x not in parent:
                return 0
            if x not in depth_of:
                depth_of[x] = depth(parent[x]) + 1
            return depth_of[x]

        ok = True
        for s in syms:
            lengths[s] = depth(s)
            if lengths[s] > limit:
                ok = False
        if ok:
            return lengths
        work = [max(1, (f + 1) >> 1) if f else 0 for f in work]


def _canonical(lengths: list[int]) -> list[int]:
    """LHA's codes: assigned in order of length, then symbol."""
    codes = [0] * len(lengths)
    count = [0] * 18
    for ln in lengths:
        count[ln] += 1
    start = [0] * 18
    for i in range(1, 17):
        start[i + 1] = (start[i] + count[i]) << 1
    for i, ln in enumerate(lengths):
        if ln:
            codes[i] = start[ln]
            start[ln] += 1
    return codes


def _pos_class(pos: int) -> int:
    return pos.bit_length()                      # 0 for 0; k for 2^(k-1) <= pos < 2^k


def _tokens(data: bytes, max_chain: int = 96):
    """LZSS tokens: (literal byte, None) or (length, distance - 1), greedy,
    over hash chains of 3-byte keys within the 8 KB window."""
    out = []
    n = len(data)
    head: dict = {}
    prev = [-1] * n
    inserted = 0

    def insert_upto(j):
        nonlocal inserted
        while inserted < j and inserted + 2 < n:
            k = data[inserted:inserted + 3]
            prev[inserted] = head.get(k, -1)
            head[k] = inserted
            inserted += 1

    i = 0
    while i < n:
        insert_upto(i)
        best_len, best_pos = 0, 0
        if i + THRESHOLD <= n:
            limit = min(MAXMATCH, n - i)
            cand = head.get(data[i:i + 3], -1)
            chain = 0
            while cand >= 0 and i - cand <= DICSIZ and chain < max_chain:
                if data[cand + best_len] == data[i + best_len] if best_len < limit else False:
                    ln = 0
                    while ln < limit and data[cand + ln] == data[i + ln]:
                        ln += 1
                    if ln > best_len:
                        best_len, best_pos = ln, i - cand - 1
                        if ln == limit:
                            break
                cand = prev[cand]
                chain += 1
        if best_len >= THRESHOLD:
            out.append((best_len, best_pos))
            i += best_len
        else:
            out.append((data[i], None))
            i += 1
    return out


def _write_pt(bw: _BitWriter, lengths: list[int], nbit: int, special: int):
    n = len(lengths)
    while n > 0 and lengths[n - 1] == 0:
        n -= 1
    bw.put(nbit, n)
    i = 0
    while i < n:
        k = lengths[i]
        i += 1
        if k <= 6:
            bw.put(3, k)
        else:
            bw.put(k - 3, (1 << (k - 3)) - 2)
        if i == special:
            while i < 6 and lengths[i] == 0:
                i += 1
            bw.put(2, (i - 3) & 3)


def _c_len_tokens(c_len: list[int]):
    """The C lengths as code-length symbols: 0-2 for runs of zeros, length + 2 otherwise."""
    n = len(c_len)
    while n > 0 and c_len[n - 1] == 0:
        n -= 1
    out = []
    i = 0
    while i < n:
        k = c_len[i]
        i += 1
        if k == 0:
            count = 1
            while i < n and c_len[i] == 0:
                i += 1
                count += 1
            while count > 0:
                if count <= 2:
                    out.extend([(0, None)] * count)
                    count = 0
                elif count <= 18:
                    out.append((1, count - 3))
                    count = 0
                elif count == 19:
                    out.append((0, None))
                    out.append((1, 15))
                    count = 0
                else:
                    take = min(count, 20 + 511)
                    out.append((2, take - 20))
                    count -= take
        else:
            out.append((k + 2, None))
    return n, out


def _encode_block(bw: _BitWriter, toks: list):
    c_freq = [0] * NC
    p_freq = [0] * NP
    for sym, pos in toks:
        if pos is None:
            c_freq[sym] += 1
        else:
            c_freq[sym + 256 - THRESHOLD] += 1
            p_freq[_pos_class(pos)] += 1
    c_len = _code_lengths(c_freq)
    c_code = _canonical(c_len)
    bw.put(16, len(toks))
    used = [i for i, f in enumerate(c_freq) if f]
    if len(used) == 1:                      # one symbol: the tables say so directly
        bw.put(TBIT, 0); bw.put(TBIT, 0)
        bw.put(CBIT, 0); bw.put(CBIT, used[0])
        c_len = [0] * NC
    else:
        n, tl = _c_len_tokens(c_len)
        t_freq = [0] * NT
        for s, _ in tl:
            t_freq[s] += 1
        t_len = _code_lengths(t_freq)
        t_used = [i for i, f in enumerate(t_freq) if f]
        if len(t_used) == 1:
            bw.put(TBIT, 0); bw.put(TBIT, t_used[0])
            t_code = [0] * NT
            t_len = [0] * NT
        else:
            t_code = _canonical(t_len)
            _write_pt(bw, t_len, TBIT, 3)
        bw.put(CBIT, n)
        for s, extra in tl:
            if t_len[s]:
                bw.put(t_len[s], t_code[s])
            if s == 1:
                bw.put(4, extra)
            elif s == 2:
                bw.put(CBIT, extra)
    p_len = _code_lengths(p_freq)
    p_used = [i for i, f in enumerate(p_freq) if f]
    if len(p_used) <= 1:
        bw.put(PBIT, 0); bw.put(PBIT, p_used[0] if p_used else 0)
        p_code = [0] * NP
        p_len = [0] * NP
    else:
        p_code = _canonical(p_len)
        _write_pt(bw, p_len, PBIT, -1)
    for sym, pos in toks:
        if pos is None:
            if c_len[sym]:
                bw.put(c_len[sym], c_code[sym])
        else:
            c = sym + 256 - THRESHOLD
            if c_len[c]:
                bw.put(c_len[c], c_code[c])
            k = _pos_class(pos)
            if p_len[k]:
                bw.put(p_len[k], p_code[k])
            if k > 1:
                bw.put(k - 1, pos & ((1 << (k - 1)) - 1))


def lh5_compress(data: bytes) -> bytes:
    if not data:
        return b""
    toks = _tokens(data)
    bw = _BitWriter()
    for start in range(0, len(toks), BLOCK_TOKENS):
        _encode_block(bw, toks[start:start + BLOCK_TOKENS])
    return bw.flush()


def _read_pt(br: _BitReader, nn: int, nbit: int, special: int) -> list[int]:
    lengths = [0] * nn
    n = br.get(nbit)
    if n == 0:
        c = br.get(nbit)
        return lengths, c
    i = 0
    while i < n:
        c = br.peek(3)
        br.get(3)
        if c == 7:
            while br.get(1):
                c += 1
        lengths[i] = c
        i += 1
        if i == special:
            z = br.get(2)
            for _ in range(z):
                lengths[i] = 0
                i += 1
    return lengths, None


def _decoder_table(lengths: list[int]):
    codes = _canonical(lengths)
    return {(lengths[s], codes[s]): s for s in range(len(lengths)) if lengths[s]}


def _read_sym(br: _BitReader, table, single):
    if single is not None:
        return single
    code, ln = 0, 0
    while ln < 17:
        code = (code << 1) | br.get(1)
        ln += 1
        s = table.get((ln, code))
        if s is not None:
            return s
    raise ValueError("lh5: bad Huffman code")


def lh5_decompress(packed: bytes, size: int) -> bytes:
    br = _BitReader(packed)
    out = bytearray()
    while len(out) < size:
        count = br.get(16)
        t_len, t_single = _read_pt(br, NT, TBIT, 3)
        n = br.get(CBIT)
        c_single = None
        c_len = [0] * NC
        if n == 0:
            c_single = br.get(CBIT)
        else:
            t_table = _decoder_table(t_len)
            i = 0
            while i < n:
                s = _read_sym(br, t_table, t_single)
                if s <= 2:
                    run = 1 if s == 0 else (br.get(4) + 3 if s == 1 else br.get(CBIT) + 20)
                    i += run
                else:
                    c_len[i] = s - 2
                    i += 1
        p_len, p_single = _read_pt(br, NP, PBIT, -1)
        c_table = _decoder_table(c_len) if c_single is None else None
        p_table = _decoder_table(p_len) if p_single is None else None
        for _ in range(count):
            c = _read_sym(br, c_table, c_single)
            if c < 256:
                out.append(c)
            else:
                length = c - 256 + THRESHOLD
                k = _read_sym(br, p_table, p_single)
                pos = k if k <= 1 else (1 << (k - 1)) + br.get(k - 1)
                start = len(out) - pos - 1
                for j in range(length):
                    out.append(out[start + j])
            if len(out) >= size:
                break
    return bytes(out[:size])


# ---------------------------------------------------------------- archive

@dataclass
class Member:
    path: str                 # Amiga path inside the archive, '/' separated
    data: bytes
    protect: int = 0          # AmigaDOS protection bits (low byte kept: HSPARWED)
    comment: str = ""
    mtime: float | None = None


def _dos_time(t: float | None) -> int:
    tm = time.localtime(time.time() if t is None else t)     # MS-DOS stamps are local time, as LhA writes them
    year = max(1980, tm.tm_year)
    date = ((year - 1980) << 9) | (tm.tm_mon << 5) | tm.tm_mday
    clock = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec // 2)
    return (date << 16) | clock


def _header(m: Member, method: bytes, packed_len: int, crc: int) -> bytes:
    """A level-1 header: no extended headers, OS id 'A'."""
    name = m.path.encode("latin-1")
    if m.comment:
        name += b"\0" + m.comment.encode("latin-1")[:79]
    if len(name) > 230:
        raise ValueError(f"name too long for an LhA header: {m.path}")
    base = bytearray(method)                                  # offset 2: -lh5- / -lh0-
    base += struct.pack("<III", packed_len, len(m.data), _dos_time(m.mtime))
    base += bytes([m.protect & 0xFF, 1, len(name)]) + name    # attribute (Amiga HSPARWED), level 1
    base += struct.pack("<H", crc) + b"A" + struct.pack("<H", 0)
    return bytes([len(base), sum(base) & 0xFF]) + bytes(base)


def pack(members: list[Member], method: str = "auto") -> bytes:
    """An LhA archive of `members`. method: "auto" (lh5 when it helps), "lh5" or "lh0"."""
    out = bytearray()
    for m in members:
        crc = crc16(m.data)
        packed, meth = m.data, b"-lh0-"
        if method in ("auto", "lh5") and m.data:
            c = lh5_compress(m.data)
            if method == "lh5" or len(c) < len(m.data):
                packed, meth = c, b"-lh5-"
        out += _header(m, meth, len(packed), crc) + packed
    out.append(0)                                           # end of archive
    return bytes(out)


def read(archive: bytes) -> list[Member]:
    """The members of a level-1 archive (lh0/lh5), as this module writes them."""
    out = []
    pos = 0
    while pos < len(archive) and archive[pos]:
        hsize = archive[pos]
        base = archive[pos + 2:pos + 2 + hsize]
        if sum(base) & 0xFF != archive[pos + 1]:
            raise ValueError("LhA header checksum is wrong")
        method = base[0:5]
        packed_len, size, _stamp = struct.unpack_from("<III", base, 5)
        attr, level, nlen = base[17], base[18], base[19]
        if level != 1:
            raise ValueError(f"LhA header level {level} is not read here")
        name, _, comment = base[20:20 + nlen].partition(b"\0")
        crc = struct.unpack_from("<H", base, 20 + nlen)[0]
        ext = struct.unpack_from("<H", base, hsize - 2)[0]
        p = pos + 2 + hsize
        while ext:                                             # extended headers count in the packed size
            nxt = struct.unpack_from("<H", archive, p + ext - 2)[0]
            p += ext
            packed_len -= ext
            ext = nxt
        packed = archive[p:p + packed_len]
        data = packed if method == b"-lh0-" else lh5_decompress(packed, size)
        if crc16(data) != crc:
            raise ValueError(f"CRC mismatch in {name!r}")
        out.append(Member(name.decode("latin-1"), data, attr, comment.decode("latin-1")))
        pos = p + packed_len
    return out


def write(path: str | Path, members: list[Member], method: str = "auto") -> Path:
    path = Path(path)
    path.write_bytes(pack(members, method))
    return path
