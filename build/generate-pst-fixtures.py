#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Writes the Outlook data files (PST) the PST import is tested against.

    python build/generate-pst-fixtures.py            write build/fixtures/pst/*.pst
    python build/generate-pst-fixtures.py --check    compare what it would write with
                                                     what is there, byte for byte

A PST is a file Microsoft documents in full ([MS-PST], "Outlook Personal Folders
(.pst) File Format"), and the files here are written from that document and
nothing else - no Outlook, no third-party PST code - by the standard library
alone. They are deterministic: the same script writes the same bytes on every
run and every machine (fixed times, identifiers and record key), which is what
lets them be committed and checked.

What they hold is what the server's reader (hmailserver/source/Server/Common/Pst)
must get right, and what test/RegressionTests/API/PstImport.cs asserts:

  standard.pst       NDB_CRYPT_PERMUTE. The message store, the name-to-id map
                     (the Keywords named property), the root folder with the
                     Drafts and Junk EntryIDs, the IPM subtree three levels deep
                     and a chain of it 27 deep, a search folder and a folder
                     outside the subtree, a contacts and a calendar folder, the
                     four special folders under names IMAP's name table does not
                     hold (Gesendete Elemente, Gel\u00f6schte Elemente, Entw\u00fcrfe,
                     Junk-E-Mail) beside ordinary folders called Sent Items and
                     Junk E-mail that no EntryID names, and the messages listed
                     below: plain, HTML in cp1252, both with attachments and an
                     inline image under a Content-ID, LZFu-compressed RTF that
                     carries HTML, transport headers with a stale MIME boundary,
                     PT_STRING8 in cp1251, the subject marker, categories, an
                     embedded message with its own attachment, bodies and
                     attachments past one block (XBLOCK), a table whose rows and
                     heap span several blocks (180 recipients), attachments by
                     reference, an S/MIME signed message, an appointment in a
                     mail folder, an associated (FAI) item and a 1970 delivery time;
                     and in Extras a body in RTF alone, a message embedded twelve
                     deep, categories longer than a message's keywords, HTML in
                     a code page the reader does not name (37), opaque S/MIME, a
                     message sent on behalf of another, the identifiers of a
                     reply, and a Message-ID longer than a header line. The
                     folders added last carry NIDs of their own, far above the
                     others, so that every NID the tests name stays what it was.
  inbox-roles.pst    The mini file's shape, with the Drafts and Junk EntryIDs on
                     the Inbox rather than the root folder, as older files keep
                     them, and those folders named in French.
  many.pst           An Inbox of 90 messages, so that a job can be interrupted
                     half way through one folder.
  mini-none.pst,     The same three messages under each of the three encodings
  mini-permute.pst,  ([MS-PST] 5.1, 5.2): what the reader rebuilds from them must
  mini-cyclic.pst    be the same bytes.
  protected.pst      The mini file with PidTagPstPassword set to the CRC-32 of the
                     password "fixture-password" ([MS-PST] 5.3's CRC over its
                     bytes), which the reader asks for.
  corrupt-loop.pst   The mini file with its node B-tree's intermediate page
                     pointing at itself for the second half of its keys.
  corrupt-block.pst  The mini file with one byte of its second message's
                     property block changed, so that block's CRC fails.
  ansi.pst           An ANSI file (wVer 14, [MS-PST]'s 32-bit NDB layer, AnsiNdb
                     below): every string PT_STRING8, messages in Windows-1252 and
                     Windows-1251 by PidTagMessageCodepage, a drafts folder named
                     in Windows-1252, and a 12,000-byte attachment in two blocks
                     under an ANSI XBLOCK.
  offline.ost        The mini file with the OST magic ("SO"): read as a PST.
  offline-exchange.ost  The same, its store naming the IPM subtree by an Exchange
                     folder EntryID (46 bytes), as an OST caching an Exchange
                     mailbox does: refused at the store.
  items.pst          What is not mail: contacts (a full one with three addresses,
                     telephones, addresses, a birthday, notes and a photo; one with
                     no address; a distribution list) in two folders; appointments
                     (a single one, an all-day one, a weekly series across the
                     clock change with one occurrence deleted and one moved, a
                     monthly nth-weekday one, a yearly all-day one, a meeting with
                     attendees and a global object id, a damaged recurrence, one
                     with no time) with W. Europe's TZSTRUCT and TZDEFINITION;
                     tasks; a note; a journal entry - each named property's id
                     given through the name-to-id map ([MS-OXOCAL] 2.2.1.44 for the
                     recurrence blob, recurrence() below). Beside them: a hidden
                     contacts folder, the recipient cache, a contacts folder under
                     Deleted Items, notes of 270,000 bytes, a contact whose address
                     the test has the account collect first, a TZSTRUCT-only
                     series, and a calendar folder of a 300-character name holding
                     an appointment whose description is kept only as RTF.
  clash.pst          A calendar of four appointments, the first three with UIDs
                     that differ only in characters a resource name makes "_"
                     (clash+1@, clash=1@, clash!1@): the store takes the first and
                     refuses the next two every time; the fourth is stored, which
                     shows both refusals are their own, so both are counted
                     not_saved at once, with no pause. Beside it a calendar
                     "Tail" whose last item is a twin of its first: nothing after
                     it is stored, so its count decides, after three pauses.
  refuse-*.pst       A header and nothing else, for each header the reader
                     refuses: an OST in the 4K-page layout ("SO", wVer 36), an ANSI
                     header with an unknown encoding (7), the 4K-page layout (wVer
                     36), an unknown version (40), Windows Information Protection
                     (bCryptMethod 0x10), a file whose header says it is longer
                     than it is, and a header whose CRC is wrong.

The encode tables are [MS-PST] 5.1's mpbbCrypt: mpbbR, mpbbS and mpbbI (the
inverse of mpbbR, computed here). The CRC is [MS-PST] 5.3's: the CRC-32 table,
initial value 0 and no final inversion, which is zlib's crc32 with the register
inverted on the way in and out.
"""

import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "fixtures", "pst")

MPBB_R = bytes([
    65, 54, 19, 98, 168, 33, 110, 187, 244, 22, 204, 4, 127, 100, 232, 93,
    30, 242, 203, 42, 116, 197, 94, 53, 210, 149, 71, 158, 150, 45, 154, 136,
    76, 125, 132, 63, 219, 172, 49, 182, 72, 95, 246, 196, 216, 57, 139, 231,
    35, 59, 56, 142, 200, 193, 223, 37, 177, 32, 165, 70, 96, 78, 156, 251,
    170, 211, 86, 81, 69, 124, 85, 0, 7, 201, 43, 157, 133, 155, 9, 160,
    143, 173, 179, 15, 99, 171, 137, 75, 215, 167, 21, 90, 113, 102, 66, 191,
    38, 74, 107, 152, 250, 234, 119, 83, 178, 112, 5, 44, 253, 89, 58, 134,
    126, 206, 6, 235, 130, 120, 87, 199, 141, 67, 175, 180, 28, 212, 91, 205,
    226, 233, 39, 79, 195, 8, 114, 128, 207, 176, 239, 245, 40, 109, 190, 48,
    77, 52, 146, 213, 14, 60, 34, 50, 229, 228, 249, 159, 194, 209, 10, 129,
    18, 225, 238, 145, 131, 118, 227, 151, 230, 97, 138, 23, 121, 164, 183, 220,
    144, 122, 92, 140, 2, 166, 202, 105, 222, 80, 26, 17, 147, 185, 82, 135,
    88, 252, 237, 29, 55, 73, 27, 106, 224, 41, 51, 153, 189, 108, 217, 148,
    243, 64, 84, 111, 240, 198, 115, 184, 214, 62, 101, 24, 68, 31, 221, 103,
    16, 241, 12, 25, 236, 174, 3, 161, 20, 123, 169, 11, 255, 248, 163, 192,
    162, 1, 247, 46, 188, 36, 104, 117, 13, 254, 186, 47, 181, 208, 218, 61,
])

MPBB_S = bytes([
    20, 83, 15, 86, 179, 200, 122, 156, 235, 101, 72, 23, 22, 21, 159, 2,
    204, 84, 124, 131, 0, 13, 12, 11, 162, 98, 168, 118, 219, 217, 237, 199,
    197, 164, 220, 172, 133, 116, 214, 208, 167, 155, 174, 154, 150, 113, 102, 195,
    99, 153, 184, 221, 115, 146, 142, 132, 125, 165, 94, 209, 93, 147, 177, 87,
    81, 80, 128, 137, 82, 148, 79, 78, 10, 107, 188, 141, 127, 110, 71, 70,
    65, 64, 68, 1, 17, 203, 3, 63, 247, 244, 225, 169, 143, 60, 58, 249,
    251, 240, 25, 48, 130, 9, 46, 201, 157, 160, 134, 73, 238, 111, 77, 109,
    196, 45, 129, 52, 37, 135, 27, 136, 170, 252, 6, 161, 18, 56, 253, 76,
    66, 114, 100, 19, 55, 36, 106, 117, 119, 67, 255, 230, 180, 75, 54, 92,
    228, 216, 53, 61, 69, 185, 44, 236, 183, 49, 43, 41, 7, 104, 163, 14,
    105, 123, 24, 158, 33, 57, 190, 40, 26, 91, 120, 245, 35, 202, 42, 176,
    175, 62, 254, 4, 140, 231, 229, 152, 50, 149, 211, 246, 74, 232, 166, 234,
    233, 243, 213, 47, 112, 32, 242, 31, 5, 103, 173, 85, 16, 206, 205, 227,
    39, 59, 218, 186, 215, 194, 38, 212, 145, 29, 210, 28, 34, 51, 248, 250,
    241, 90, 239, 207, 144, 182, 139, 181, 189, 192, 191, 8, 151, 30, 108, 226,
    97, 224, 198, 193, 89, 171, 187, 88, 222, 95, 223, 96, 121, 126, 178, 138,
])

MPBB_I = bytes(MPBB_R.index(i) for i in range(256))

LZFU_PRELOAD = (b"{\\rtf1\\ansi\\mac\\deff0\\deftab720{\\fonttbl;}{\\f0\\fnil \\froman \\fswiss \\fmodern \\fscript \\fdecor MS Sans SerifSymbolArialTimes New RomanCourier"
                b"{\\colortbl\\red0\\green0\\blue0\r\n\\par \\pard\\plain\\f0\\fs20\\b\\i\\u\\tab\\tx")

# [MS-PST] 2.2.2.1: the node types.
NID_TYPE_NORMAL_FOLDER = 0x02
NID_TYPE_SEARCH_FOLDER = 0x03
NID_TYPE_NORMAL_MESSAGE = 0x04
NID_TYPE_ATTACHMENT = 0x05
NID_TYPE_ASSOC_MESSAGE = 0x08
NID_TYPE_HIERARCHY_TABLE = 0x0D
NID_TYPE_CONTENTS_TABLE = 0x0E
NID_TYPE_ASSOC_CONTENTS_TABLE = 0x0F
NID_TYPE_LTP = 0x1F

NID_MESSAGE_STORE = 0x21
NID_NAME_TO_ID_MAP = 0x61
NID_ROOT_FOLDER = 0x122
NID_ATTACHMENT_TABLE = 0x671
NID_RECIPIENT_TABLE = 0x692

# Property types.
PT_I2, PT_LONG, PT_BOOLEAN, PT_I8, PT_STRING8, PT_UNICODE, PT_SYSTIME, PT_BINARY, PT_OBJECT = (
    0x0002, 0x0003, 0x000B, 0x0014, 0x001E, 0x001F, 0x0040, 0x0102, 0x000D)
PT_MV_UNICODE, PT_MV_BINARY = 0x101F, 0x1102

MAX_BLOCK_DATA = 8192 - 16
MAX_HEAP_ALLOC = 3580

RECORD_KEY = bytes.fromhex("5053545f666978747572655f72656331")  # "PST_fixture_rec1"


def crc(data, init=0):
    return (~zlib.crc32(data, (~init) & 0xFFFFFFFF)) & 0xFFFFFFFF


def encode_permute(data):
    return bytes(MPBB_R[b] for b in data)


def crypt_cyclic(data, key):
    # [MS-PST] 5.2: its own inverse.
    w = (key ^ (key >> 16)) & 0xFFFF
    out = bytearray(len(data))
    for i, b in enumerate(data):
        b = (b + (w & 0xFF)) & 0xFF
        b = MPBB_R[b]
        b = (b + (w >> 8)) & 0xFF
        b = MPBB_S[b]
        b = (b - (w >> 8)) & 0xFF
        b = MPBB_I[b]
        b = (b - (w & 0xFF)) & 0xFF
        out[i] = b
        w = (w + 1) & 0xFFFF
    return bytes(out)


def signature(ib, bid):
    x = (ib ^ bid) & 0xFFFFFFFFFFFFFFFF
    return ((x >> 16) ^ x) & 0xFFFF


def align(value, unit):
    return (value + unit - 1) // unit * unit


def filetime(year, month, day, hour=0, minute=0, second=0):
    import calendar
    seconds = calendar.timegm((year, month, day, hour, minute, second, 0, 0, 0))
    return (seconds + 11644473600) * 10000000


def utf16(text):
    return text.encode("utf-16-le")


def lzfu_compress(raw):
    """A greedy LZFu compressor ([MS-OXRTFCP] 2.1.3): the longest match of 2 to
    17 bytes in the 4096-byte dictionary, which it keeps as the decompressor
    does; the stream ends with a reference to the write position."""
    dic = bytearray(4096)
    dic[:len(LZFU_PRELOAD)] = LZFU_PRELOAD
    w = len(LZFU_PRELOAD)
    filled = len(LZFU_PRELOAD)
    out = bytearray()
    i = 0
    n = len(raw)
    done = False
    while not done:
        control_at = len(out)
        out.append(0)
        control = 0
        for bit in range(8):
            if i >= n:
                control |= 1 << bit
                out.append((w >> 4) & 0xFF)
                out.append((w << 4) & 0xF0)
                done = True
                break
            best_length, best_offset = 0, 0
            longest = min(17, n - i)
            if longest >= 2:
                for offset in range(4096):
                    if offset == w or (filled < 4096 and offset >= filled):
                        continue
                    length = 0
                    copied = []
                    while length < longest:
                        position = (offset + length) % 4096
                        distance = (position - w) % 4096
                        c = copied[distance] if distance < length else dic[position]
                        if c != raw[i + length]:
                            break
                        copied.append(c)
                        length += 1
                    if length > best_length:
                        best_length, best_offset = length, offset
                        if length == longest:
                            break
            if best_length >= 2:
                control |= 1 << bit
                reference = (best_offset << 4) | (best_length - 2)
                out.append(reference >> 8)
                out.append(reference & 0xFF)
                for k in range(best_length):
                    dic[w] = raw[i + k]
                    w = (w + 1) % 4096
                    filled = min(4096, filled + 1)
                i += best_length
            else:
                out.append(raw[i])
                dic[w] = raw[i]
                w = (w + 1) % 4096
                filled = min(4096, filled + 1)
                i += 1
        out[control_at] = control
    body = bytes(out)
    return struct.pack("<IIII", len(body) + 12, len(raw), 0x75465A4C, crc(body)) + body


# ---------------------------------------------------------------- the NDB layer

class Ndb:
    """Blocks and nodes, laid out into a file at the end."""

    def __init__(self):
        self.blocks = []          # (bid, plain bytes, internal)
        self.nodes = {}           # nid -> (bidData, bidSub, nidParent)
        self.next_block = 1
        self.nid_index = {}       # type -> highest index used
        self.next_index = 0x400

    def block(self, data, internal=False):
        if len(data) > MAX_BLOCK_DATA:
            raise ValueError("a block holds at most 8176 bytes")
        bid = (self.next_block << 2) | (2 if internal else 0)
        self.next_block += 1
        self.blocks.append((bid, bytes(data), internal))
        return bid

    def data_tree(self, data):
        """A node's data: one block, an XBLOCK of blocks, or an XXBLOCK of XBLOCKs."""
        if len(data) <= MAX_BLOCK_DATA:
            return self.block(data)
        chunks = [data[i:i + MAX_BLOCK_DATA] for i in range(0, len(data), MAX_BLOCK_DATA)]
        return self.blocks_tree([self.block(c) for c in chunks], len(data))

    def blocks_tree(self, bids, total):
        if len(bids) == 1:
            return bids[0]
        per_x = (MAX_BLOCK_DATA - 8) // 8
        if len(bids) <= per_x:
            return self.block(struct.pack("<BBHI", 1, 1, len(bids), total) + b"".join(struct.pack("<Q", b) for b in bids), True)
        raise ValueError("an XXBLOCK is not needed by any fixture")

    def subnodes(self, entries):
        """entries: [(nid, bidData, bidSub)] -> the SLBLOCK's bid, or 0."""
        if not entries:
            return 0
        entries = sorted(entries)
        if len(entries) > (MAX_BLOCK_DATA - 8) // 24:
            raise ValueError("an SIBLOCK is not needed by any fixture")
        body = struct.pack("<BBHI", 2, 0, len(entries), 0)
        for nid, data, sub in entries:
            body += struct.pack("<QQQ", nid, data, sub)
        return self.block(body, True)

    def nid(self, nid_type):
        # One index counter for every type: a folder's tables take their NIDs
        # from the folder's index, so a search folder and a normal folder may
        # not share one.
        self.next_index += 1
        self.nid_index[nid_type] = self.next_index
        return (self.next_index << 5) | nid_type

    def node(self, nid, bid_data, bid_sub=0, parent=0):
        self.nodes[nid] = (bid_data, bid_sub, parent)

    def write(self, crypt, *, password_patch=None, loop=False, break_block=None, version=23, client=b"SM"):
        """The file's bytes. crypt: 0 none, 1 permute, 2 cyclic."""
        out = bytearray(0x400)    # the header; no allocation map is kept (fAMapValid 0)
        placed = {}
        for bid, data, internal in self.blocks:
            ib = len(out)
            if internal or crypt == 0:
                stored = data
            elif crypt == 1:
                stored = encode_permute(data)
            else:
                stored = crypt_cyclic(data, bid & 0xFFFFFFFF)
            size = align(len(stored) + 16, 64)
            trailer_crc = crc(stored)
            if break_block is not None and bid == break_block:
                stored = bytes([stored[0] ^ 0xFF]) + stored[1:]
            out += stored + bytes(size - 16 - len(stored))
            out += struct.pack("<HHIQ", len(stored), signature(ib, bid), trailer_crc, bid)
            placed[bid] = (ib, len(stored))
        while len(out) % 512:
            out.append(0)

        next_page = [0x100000]

        def page(entries, entry_size, level, ptype):
            ib = len(out)
            bid = next_page[0] << 2
            next_page[0] += 1
            body = bytearray(488)
            for i, e in enumerate(entries):
                body[i * entry_size:(i + 1) * entry_size] = e
            p = bytes(body) + struct.pack("<BBBBI", len(entries), 488 // entry_size, entry_size, level, 0)
            p += struct.pack("<BBHIQ", ptype, ptype, signature(ib, bid), crc(p), bid)
            out.extend(p)
            return bid, ib

        def tree(keyed_entries, leaf_size, ptype, looping):
            """[(key, entry bytes)] -> the root's (bid, ib). looping makes the
            root's last entry lead back to the root itself."""
            per_leaf = 488 // leaf_size
            level = []
            for i in range(0, len(keyed_entries), per_leaf):
                chunk = keyed_entries[i:i + per_leaf]
                bid, ib = page([e for _, e in chunk], leaf_size, 0, ptype)
                level.append((chunk[0][0], bid, ib))
            if looping and len(level) < 2:
                raise ValueError("a looping tree needs two leaves")
            depth = 1
            while len(level) > 1:
                per_node = 488 // 24
                upper = []
                for i in range(0, len(level), per_node):
                    chunk = level[i:i + per_node]
                    entries = [struct.pack("<QQQ", key, bid, ib) for key, bid, ib in chunk]
                    if looping and len(level) <= per_node:
                        # The root: its last key leads back to this same page.
                        entries[-1] = struct.pack("<QQQ", chunk[-1][0], next_page[0] << 2, len(out))
                    upper.append((chunk[0][0],) + page(entries, 24, depth, ptype))
                level = upper
                depth += 1
            return level[0][1], level[0][2]

        bbt_entries = []
        for bid in sorted(placed):
            ib, cb = placed[bid]
            bbt_entries.append((bid, struct.pack("<QQHHI", bid, ib, cb, 2, 0)))
        bbt_root = tree(bbt_entries, 24, 0x80, False)

        nbt_entries = []
        for nid in sorted(self.nodes):
            data, sub, parent = self.nodes[nid]
            nbt_entries.append((nid, struct.pack("<QQQII", nid, data, sub, parent, 0)))
        nbt_root = tree(nbt_entries, 32, 0x81, loop)

        eof = len(out)
        header = bytearray(564)
        header[0:4] = b"!BDN"
        header[8:10] = client
        struct.pack_into("<HHBB", header, 10, version, 19, 1, 1)
        struct.pack_into("<Q", header, 32, next_page[0] << 2)            # bidNextP
        struct.pack_into("<I", header, 40, 0x10)                          # dwUnique
        for t in range(32):
            struct.pack_into("<I", header, 44 + 4 * t, self.nid_index.get(t, 0x400) + 1)
        struct.pack_into("<QQQQ", header, 184, eof, 0x4400, 0, 0)         # ibFileEof, ibAMapLast, cbAMapFree, cbPMapFree
        struct.pack_into("<QQ", header, 216, *nbt_root)
        struct.pack_into("<QQ", header, 232, *bbt_root)
        header[248] = 0                                                   # fAMapValid: no allocation map is kept
        header[512] = 0x80                                                # bSentinel
        header[513] = crypt
        struct.pack_into("<Q", header, 516, self.next_block << 2)        # bidNextB
        struct.pack_into("<I", header, 4, crc(bytes(header[8:8 + 471])))
        struct.pack_into("<I", header, 524, crc(bytes(header[8:8 + 516])))
        out[0:564] = header
        return bytes(out)


class AnsiNdb(Ndb):
    """[MS-PST]'s ANSI file (Outlook 97 to 2002, wVer 14 or 15), the same blocks
    and nodes laid out in its 32-bit NDB layer: BIDs, IBs and NIDs of four bytes;
    a block trailer of 12 bytes (cb, wSig, bid, dwCRC - the BID before the CRC);
    a B-tree page of 496 bytes of entries, cEnt, cEntMax, cbEnt and cLevel, and a
    12-byte trailer (ptype, ptypeRepeat, wSig, bid, dwCRC); NBTENTRY 16 bytes,
    BBTENTRY 12 and BTENTRY 12; an XBLOCK's BIDs of four bytes; an SLBLOCK with
    no padding before its 12-byte entries; and the 512-byte ANSI header, with one
    CRC (dwCRCPartial) and its root at 164. The LTP layer above is the same."""

    def blocks_tree(self, bids, total):
        if len(bids) == 1:
            return bids[0]
        if len(bids) > (MAX_BLOCK_DATA - 8) // 4:
            raise ValueError("an XXBLOCK is not needed by any fixture")
        return self.block(struct.pack("<BBHI", 1, 1, len(bids), total) + b"".join(struct.pack("<I", b) for b in bids), True)

    def subnodes(self, entries):
        if not entries:
            return 0
        entries = sorted(entries)
        if len(entries) > (MAX_BLOCK_DATA - 4) // 12:
            raise ValueError("an SIBLOCK is not needed by any fixture")
        body = struct.pack("<BBH", 2, 0, len(entries))
        for nid, data, sub in entries:
            body += struct.pack("<III", nid, data, sub)
        return self.block(body, True)

    def write(self, crypt, *, version=14, client=b"SM", break_crc=False, **ignored):
        out = bytearray(0x400)
        placed = {}
        for bid, data, internal in self.blocks:
            ib = len(out)
            if internal or crypt == 0:
                stored = data
            elif crypt == 1:
                stored = encode_permute(data)
            else:
                stored = crypt_cyclic(data, bid & 0xFFFFFFFF)
            size = align(len(stored) + 12, 64)
            out += stored + bytes(size - 12 - len(stored))
            out += struct.pack("<HHII", len(stored), signature(ib, bid), bid, crc(stored))
            placed[bid] = (ib, len(stored))
        while len(out) % 512:
            out.append(0)

        next_page = [0x10000]

        def page(entries, entry_size, level, ptype):
            ib = len(out)
            bid = next_page[0] << 2
            next_page[0] += 1
            body = bytearray(496)
            for i, e in enumerate(entries):
                body[i * entry_size:(i + 1) * entry_size] = e
            p = bytes(body) + struct.pack("<BBBB", len(entries), 496 // entry_size, entry_size, level)
            p += struct.pack("<BBHII", ptype, ptype, signature(ib, bid), bid, crc(p))
            out.extend(p)
            return bid, ib

        def tree(keyed_entries, leaf_size, ptype):
            per_leaf = 496 // leaf_size
            level = []
            for i in range(0, len(keyed_entries), per_leaf):
                chunk = keyed_entries[i:i + per_leaf]
                bid, ib = page([e for _, e in chunk], leaf_size, 0, ptype)
                level.append((chunk[0][0], bid, ib))
            depth = 1
            while len(level) > 1:
                per_node = 496 // 12
                upper = []
                for i in range(0, len(level), per_node):
                    chunk = level[i:i + per_node]
                    entries = [struct.pack("<III", key, bid, ib) for key, bid, ib in chunk]
                    upper.append((chunk[0][0],) + page(entries, 12, depth, ptype))
                level = upper
                depth += 1
            return level[0][1], level[0][2]

        bbt_entries = []
        for bid in sorted(placed):
            ib, cb = placed[bid]
            bbt_entries.append((bid, struct.pack("<IIHH", bid, ib, cb, 2)))
        bbt_root = tree(bbt_entries, 12, 0x80)

        nbt_entries = []
        for nid in sorted(self.nodes):
            data, sub, parent = self.nodes[nid]
            nbt_entries.append((nid, struct.pack("<IIII", nid, data, sub, parent)))
        nbt_root = tree(nbt_entries, 16, 0x81)

        eof = len(out)
        header = bytearray(512)
        header[0:4] = b"!BDN"
        header[8:10] = client
        struct.pack_into("<HHBB", header, 10, version, 19, 1, 1)
        struct.pack_into("<III", header, 24, self.next_block << 2, next_page[0] << 2, 0x10)   # bidNextB, bidNextP, dwUnique
        for t in range(32):
            struct.pack_into("<I", header, 36 + 4 * t, self.nid_index.get(t, 0x400) + 1)
        struct.pack_into("<IIIII", header, 164, 0, eof, 0x4400, 0, 0)     # dwReserved, ibFileEof, ibAMapLast, cbAMapFree, cbPMapFree
        struct.pack_into("<II", header, 184, *nbt_root)
        struct.pack_into("<II", header, 192, *bbt_root)
        header[200] = 0                                                   # fAMapValid: no allocation map is kept
        header[204:460] = b"\xFF" * 256                                   # rgbFM and rgbFP, deprecated, all 0xFF
        header[460] = 0x80                                                # bSentinel
        header[461] = crypt
        struct.pack_into("<I", header, 4, crc(bytes(header[8:8 + 471])))
        if break_crc:
            header[4] ^= 0xFF
        out[0:512] = header
        return bytes(out)


# ---------------------------------------------------------------- the LTP layer

class Heap:
    """A heap-on-node ([MS-PST] 2.3.1) over as many blocks as it needs."""

    def __init__(self, client):
        self.client = client
        self.pages = [[]]         # allocations per block
        self.root = 0

    def header_size(self, index):
        if index == 0:
            return 12
        return 66 if (index - 8) % 128 == 0 and index >= 8 else 2

    def used(self, index):
        allocations = self.pages[index]
        return align(self.header_size(index) + sum(len(a) for a in allocations), 2) + 4 + 2 * (len(allocations) + 1)

    def alloc(self, data):
        if len(data) > MAX_HEAP_ALLOC:
            raise ValueError("a heap allocation holds at most 3580 bytes")
        index = len(self.pages) - 1
        if self.used(index) + len(data) + 1 + 2 > MAX_BLOCK_DATA:
            self.pages.append([])
            index += 1
        self.pages[index].append(bytes(data))
        return (index << 16) | (len(self.pages[index]) << 5)

    def blocks(self):
        out = []
        for index, allocations in enumerate(self.pages):
            offsets = []
            body = bytearray(self.header_size(index))
            # The allocations end to end - each ends where the next begins -
            # and the page map after them, at an even offset.
            for a in allocations:
                offsets.append(len(body))
                body += a
            offsets.append(len(body))
            if len(body) % 2:
                body.append(0)
            ib_map = len(body)
            body += struct.pack("<HH", len(allocations), 0)
            for o in offsets:
                body += struct.pack("<H", o)
            if index == 0:
                struct.pack_into("<HBBI", body, 0, ib_map, 0xEC, self.client, self.root)
            else:
                struct.pack_into("<H", body, 0, ib_map)
            out.append(bytes(body))
        return out


class Node:
    """What a node is built of: its heap, and the subnodes its values and tables need."""

    def __init__(self, ndb):
        self.ndb = ndb
        self.subs = []            # (nid, bidData, bidSub)

    def big_value(self, data):
        nid = self.ndb.nid(NID_TYPE_LTP)
        self.subs.append((nid, self.ndb.data_tree(data), 0))
        return nid

    def value_ref(self, heap, data):
        if len(data) == 0:
            return 0
        if len(data) <= MAX_HEAP_ALLOC:
            return heap.alloc(data)
        return self.big_value(data)

    def heap_data(self, heap):
        blocks = heap.blocks()
        bids = [self.ndb.block(b) for b in blocks]
        return self.ndb.blocks_tree(bids, sum(len(b) for b in blocks))


def encode_value(ptype, value):
    if ptype in (PT_UNICODE,):
        return utf16(value)
    if ptype == PT_STRING8:
        return value if isinstance(value, bytes) else value.encode("ascii")
    if ptype in (PT_BINARY, PT_OBJECT):
        return value
    if ptype == PT_LONG:
        return struct.pack("<I", value & 0xFFFFFFFF)
    if ptype == PT_I2:
        return struct.pack("<h", value)
    if ptype == PT_BOOLEAN:
        return struct.pack("<B", 1 if value else 0)
    if ptype in (PT_I8, PT_SYSTIME):
        return struct.pack("<Q", value)
    if ptype == 0x0005:
        return struct.pack("<d", value)
    if ptype in (PT_MV_UNICODE, PT_MV_BINARY):
        items = [utf16(v) if ptype == PT_MV_UNICODE else v for v in value]
        head = struct.pack("<I", len(items))
        offset = 4 + 4 * len(items)
        body = b""
        for item in items:
            head += struct.pack("<I", offset + len(body))
            body += item
        return head + body
    raise ValueError("type %04X" % ptype)


def property_context(ndb, props, extra_subs=()):
    """props: [(id, type, value)] -> (bidData, bidSub)."""
    node = Node(ndb)
    node.subs.extend(extra_subs)
    heap = Heap(0xBC)
    records = []
    for pid, ptype, value in sorted(props, key=lambda p: p[0]):
        if ptype == PT_OBJECT:
            # value: (nid, size) of the subnode holding the object
            ref = heap.alloc(struct.pack("<II", value[0], value[1]))
        else:
            data = encode_value(ptype, value)
            if ptype in (PT_LONG, PT_I2, PT_BOOLEAN):
                ref = struct.unpack("<I", data.ljust(4, b"\0"))[0]
            else:
                ref = node.value_ref(heap, data)
        records.append(struct.pack("<HHI", pid, ptype, ref))
    body = b"".join(records)
    if len(body) > MAX_HEAP_ALLOC:
        raise ValueError("a two-level property context is not needed by any fixture")
    leaf = heap.alloc(body) if body else 0
    heap.root = heap.alloc(struct.pack("<BBBBI", 0xB5, 2, 6, 0, leaf))
    return node.heap_data(heap), ndb.subnodes(node.subs)


def table_context(ndb, columns, rows):
    """columns: [(id, type)]; rows: [(row id, {id: value})] -> (bidData, bidSub).
    PidTagLtpRowId and PidTagLtpRowVer are added."""
    node = Node(ndb)
    heap = Heap(0x7C)
    all_columns = [(0x67F2, PT_LONG), (0x67F3, PT_LONG)] + list(columns)

    def width(ptype):
        return 1 if ptype == PT_BOOLEAN else (2 if ptype == PT_I2 else (8 if ptype in (PT_I8, PT_SYSTIME) else 4))

    # [MS-PST] 2.3.4.1: the eight- and four-byte values first, then the
    # two-byte, then the one-byte, then the cell existence bitmap.
    layout = {}
    offset = 0
    for size in (8, 4, 2, 1):
        for pid, ptype in all_columns:
            if width(ptype) == size:
                layout[pid] = offset
                offset += size
        if size == 4:
            end_4b = offset
        elif size == 2:
            end_2b = offset
        elif size == 1:
            end_1b = offset
    ceb = (len(all_columns) + 7) // 8
    row_size = end_1b + ceb
    bits = {pid: i for i, (pid, ptype) in enumerate(all_columns)}

    matrix = bytearray()
    for row_id, values in rows:
        row = bytearray(row_size)
        values = dict(values)
        values[0x67F2] = row_id
        values[0x67F3] = 1
        for pid, ptype in all_columns:
            if pid not in values:
                continue
            value = values[pid]
            if ptype in (PT_LONG, PT_I2, PT_BOOLEAN, PT_I8, PT_SYSTIME):
                data = encode_value(ptype, value)
            else:
                data = struct.pack("<I", node.value_ref(heap, encode_value(ptype, value)))
            row[layout[pid]:layout[pid] + len(data)] = data
            bit = bits[pid]
            row[end_1b + bit // 8] |= 0x80 >> (bit % 8)
        matrix += row

    if not rows:
        rows_ref = 0
    elif len(matrix) <= MAX_HEAP_ALLOC:
        rows_ref = heap.alloc(bytes(matrix))
    else:
        per_block = MAX_BLOCK_DATA // row_size
        chunks = []
        for i in range(0, len(rows), per_block):
            chunk = matrix[i * row_size:(i + per_block) * row_size]
            chunks.append(bytes(chunk))
        bids = [ndb.block(c) for c in chunks]
        nid = ndb.nid(NID_TYPE_LTP)
        node.subs.append((nid, ndb.blocks_tree(bids, sum(len(c) for c in chunks)), 0))
        rows_ref = nid

    index = sorted((row_id, i) for i, (row_id, _) in enumerate(rows))
    index_body = b"".join(struct.pack("<II", r, i) for r, i in index)
    if len(index_body) > MAX_HEAP_ALLOC:
        raise ValueError("a two-level row index is not needed by any fixture")
    index_leaf = heap.alloc(index_body) if index_body else 0
    row_index = heap.alloc(struct.pack("<BBBBI", 0xB5, 4, 4, 0, index_leaf))

    info = struct.pack("<BB4HIII", 0x7C, len(all_columns), end_4b, end_2b, end_1b, row_size, row_index, rows_ref, 0)
    for pid, ptype in sorted(all_columns, key=lambda c: (c[0] << 16) | c[1]):
        info += struct.pack("<IHBB", (pid << 16) | ptype, layout[pid], width(ptype), bits[pid])
    heap.root = heap.alloc(info)
    return node.heap_data(heap), ndb.subnodes(node.subs)


# ---------------------------------------------------------------- the messaging layer

def entry_id(nid):
    return struct.pack("<I", 0) + RECORD_KEY + struct.pack("<I", nid)


class Message:
    def __init__(self, name, props, recipients=(), attachments=(), flags=0x01, fai=False):
        self.name = name
        self.props = list(props)
        self.recipients = list(recipients)
        self.attachments = list(attachments)
        self.flags = flags
        self.fai = fai


class Attachment:
    def __init__(self, props, data=None, method=1, embedded=None):
        self.props = list(props)
        self.data = data
        self.method = method
        self.embedded = embedded


# The recipient table's columns: the five the reader takes, and as many again of
# those Outlook's own template has, so that a row is wide enough for the table of
# 180 recipients to need two blocks of rows.
RECIPIENT_COLUMNS = [(0x0C15, PT_LONG), (0x3001, PT_UNICODE), (0x3002, PT_UNICODE), (0x3003, PT_UNICODE), (0x39FE, PT_UNICODE),
                     (0x0E0F, PT_BOOLEAN), (0x0FF9, PT_BINARY), (0x0FFE, PT_LONG), (0x0FFF, PT_BINARY), (0x300B, PT_BINARY),
                     (0x3900, PT_LONG), (0x3A20, PT_UNICODE), (0x3A40, PT_BOOLEAN), (0x3A71, PT_LONG), (0x5FDF, PT_LONG),
                     (0x5FF6, PT_UNICODE)]


def message_node(ndb, message):
    """(bidData, bidSub) of a message and everything under it."""
    subs = []
    recipient_rows = []
    for i, (kind, name, address) in enumerate(message.recipients):
        recipient_rows.append((i + 1, {0x0C15: kind, 0x3001: name, 0x3002: "SMTP", 0x3003: address, 0x39FE: address}))
    # A message may carry recipient rows of its own shape (an ANSI file's
    # PT_STRING8 columns, a meeting's flags and answers): recipient_table.
    recipient_rows = getattr(message, "recipient_rows", None) or recipient_rows
    data, sub = table_context(ndb, getattr(message, "recipient_columns", None) or RECIPIENT_COLUMNS, recipient_rows)
    subs.append((NID_RECIPIENT_TABLE, data, sub))

    if message.attachments:
        attachment_rows = []
        for attachment in message.attachments:
            nid = ndb.nid(NID_TYPE_ATTACHMENT)
            own = []
            props = list(attachment.props) + [(0x3705, PT_LONG, attachment.method)]
            if attachment.embedded is not None:
                inner_data, inner_sub = message_node(ndb, attachment.embedded)
                inner_nid = ndb.nid(NID_TYPE_NORMAL_MESSAGE)
                own.append((inner_nid, inner_data, inner_sub))
                props.append((0x3701, PT_OBJECT, (inner_nid, 0)))
            elif attachment.data is not None:
                props.append((0x3701, PT_BINARY, attachment.data))
                props.append((0x0E20, PT_LONG, len(attachment.data)))
            a_data, a_sub = property_context(ndb, props, own)
            subs.append((nid, a_data, a_sub))
            attachment_rows.append((nid, {0x3705: attachment.method}))
        data, sub = table_context(ndb, [(0x3705, PT_LONG)], attachment_rows)
        subs.append((NID_ATTACHMENT_TABLE, data, sub))

    props = list(message.props) + [(0x0E07, PT_LONG, message.flags | (0x10 if message.attachments else 0))]
    return property_context(ndb, props, subs)


class Folder:
    def __init__(self, name, container_class="IPF.Note", messages=(), children=(), search=False, nid=None):
        self.name = name
        self.container_class = container_class
        self.messages = list(messages)
        self.children = list(children)
        self.search = search
        self.nid = nid
        self.extra = []


def build(ndb, folders_by_role, root_children, store_extra=(), keywords_id=0x8000, roles_on=None, string_type=PT_UNICODE,
          named=(), exchange_ids=False):
    """Writes the store, the name-to-id map and the folder tree. roles_on: the
    folder that carries the Drafts and Junk EntryIDs, when it is not the root.

    string_type is the type of the folders' and the store's names (PT_STRING8
    in an ANSI file, in Windows-1252); named is [(GUID, LID, property id)],
    numeric named properties the map carries beside "Keywords"; exchange_ids
    writes the store's IPM subtree EntryID as an Exchange folder EntryID rather
    than a PST one, as an OST caching an Exchange mailbox has it."""

    def text(value):
        return value.encode("cp1252") if string_type == PT_STRING8 else value

    def assign(folder):
        if folder.nid is None:
            folder.nid = ndb.nid(NID_TYPE_SEARCH_FOLDER if folder.search else NID_TYPE_NORMAL_FOLDER)
        for child in folder.children:
            assign(child)

    root = Folder("", None, children=root_children, nid=NID_ROOT_FOLDER)
    for child in root_children:
        assign(child)

    def write_folder(folder, parent):
        props = [(0x3001, string_type, text(folder.name)), (0x3602, PT_LONG, len([m for m in folder.messages if not m.fai])),
                 (0x3603, PT_LONG, len([m for m in folder.messages if not m.fai and not (m.flags & 1)])),
                 (0x360A, PT_BOOLEAN, bool(folder.children))] + folder.extra
        if folder.container_class:
            props.append((0x3613, string_type, text(folder.container_class)))
        data, sub = property_context(ndb, props)
        ndb.node(folder.nid, data, sub, parent)

        base = folder.nid & ~0x1F
        rows = [(child.nid, {0x3001: text(child.name)}) for child in folder.children]
        data, sub = table_context(ndb, [(0x3001, string_type)], rows)
        ndb.node(base | NID_TYPE_HIERARCHY_TABLE, data, sub)

        contents, associated = [], []
        for message in folder.messages:
            nid = ndb.nid(NID_TYPE_ASSOC_MESSAGE if message.fai else NID_TYPE_NORMAL_MESSAGE)
            data, sub = message_node(ndb, message)
            ndb.node(nid, data, sub, folder.nid)
            (associated if message.fai else contents).append((nid, {0x0E07: message.flags}))
        data, sub = table_context(ndb, [(0x0E07, PT_LONG)], contents)
        ndb.node(base | NID_TYPE_CONTENTS_TABLE, data, sub)
        data, sub = table_context(ndb, [(0x0E07, PT_LONG)], associated)
        ndb.node(base | NID_TYPE_ASSOC_CONTENTS_TABLE, data, sub)

        for child in folder.children:
            write_folder(child, folder.nid)

    root.extra = []
    carrier = roles_on if roles_on is not None else root
    if "drafts" in folders_by_role:
        carrier.extra.append((0x36D7, PT_BINARY, entry_id(folders_by_role["drafts"].nid)))
    if "junk" in folders_by_role:
        ren = [b"", b"", b"", b"", entry_id(folders_by_role["junk"].nid)]
        carrier.extra.append((0x36D8, PT_MV_BINARY, ren))
    # [MS-OXOSFLD] 2.2.3: the root folder names the calendar (PidTagIpmAppointmentEntryId),
    # contacts, journal, notes and tasks folders.
    for role, tag in (("calendar", 0x36D0), ("contacts", 0x36D1), ("journal", 0x36D2), ("notes", 0x36D3), ("tasks", 0x36D4)):
        if role in folders_by_role:
            root.extra.append((tag, PT_BINARY, entry_id(folders_by_role[role].nid)))
    write_folder(root, NID_ROOT_FOLDER)

    # An Exchange folder EntryID ([MS-OXCDATA] 2.2.4.1): flags, the mailbox's
    # provider UID, the folder type, a database GUID and a global counter -
    # 46 bytes, which name no node of the file.
    ipm_entry_id = entry_id(folders_by_role["ipm"].nid)
    if exchange_ids:
        ipm_entry_id = (struct.pack("<I", 0) + RECORD_KEY + struct.pack("<H", 1) + bytes.fromhex("0a0b0c0d0e0f10111213141516171819") +
                        bytes.fromhex("000000000001") + bytes(2))
    store = [(0x0FF9, PT_BINARY, RECORD_KEY), (0x3001, string_type, text("Fixture Personal Folders")),
             (0x35E0, PT_BINARY, ipm_entry_id),
             (0x35DF, PT_LONG, 0x89)] + list(store_extra)
    if "wastebasket" in folders_by_role:
        store.append((0x35E3, PT_BINARY, entry_id(folders_by_role["wastebasket"].nid)))
    if "sent" in folders_by_role:
        store.append((0x35E4, PT_BINARY, entry_id(folders_by_role["sent"].nid)))
    if "finder" in folders_by_role:
        store.append((0x35E7, PT_BINARY, entry_id(folders_by_role["finder"].nid)))
    data, sub = property_context(ndb, store)
    ndb.node(NID_MESSAGE_STORE, data, sub)

    # The name-to-id map: one name, "Keywords" in PS_PUBLIC_STRINGS (wGuid 2),
    # the categories.
    name = utf16("Keywords")
    strings = struct.pack("<I", len(name)) + name
    entries = struct.pack("<IHH", 0, (2 << 1) | 1, keywords_id - 0x8000)
    # The GUID stream holds PSETID_Common, which no entry names: a reader
    # finds PS_PUBLIC_STRINGS by its fixed index, 2, not in the stream.
    common = bytes.fromhex("0820060000000000c000000000000046")
    # A numeric named property ([MS-PST] 2.4.7.1): its LID, a word whose low
    # bit is 0 (named by number) and whose other fifteen are 3 + the GUID's
    # place in the stream, and its index (the property id less 0x8000).
    guids = [common]
    for guid, lid, property_id in named:
        if guid not in guids:
            guids.append(guid)
        entries += struct.pack("<IHH", lid, (3 + guids.index(guid)) << 1, property_id - 0x8000)
    common = b"".join(guids)
    data, sub = property_context(ndb, [(0x0001, PT_LONG, 251), (0x0002, PT_BINARY, common), (0x0003, PT_BINARY, entries),
                                       (0x0004, PT_BINARY, strings)])
    ndb.node(NID_NAME_TO_ID_MAP, data, sub)


# ---------------------------------------------------------------- the content

T0 = filetime(2024, 3, 1, 9, 0, 0)


def mail(name, subject, body=None, *, sender=("Alice Example", "alice@example.org"), to=(("Bob Example", "bob@example.org"),),
         cc=(), bcc=(), flags=0x01, delivery=None, submit=None, created=None, message_id=None, extra=(), attachments=(),
         message_class="IPM.Note", fai=False, recipients=None):
    props = [(0x001A, PT_UNICODE, message_class)]
    if subject is not None:
        props.append((0x0037, PT_UNICODE, subject))
    if body is not None:
        props.append((0x1000, PT_UNICODE, body))
    if sender is not None:
        props += [(0x0042, PT_UNICODE, sender[0]), (0x0064, PT_UNICODE, "SMTP"), (0x0065, PT_UNICODE, sender[1]),
                  (0x0C1A, PT_UNICODE, sender[0]), (0x0C1E, PT_UNICODE, "SMTP"), (0x0C1F, PT_UNICODE, sender[1])]
    props.append((0x0E06, PT_SYSTIME, delivery if delivery is not None else T0))
    props.append((0x0039, PT_SYSTIME, submit if submit is not None else T0 - 600 * 10000000))
    props.append((0x3007, PT_SYSTIME, created if created is not None else T0 - 1200 * 10000000))
    if message_id:
        props.append((0x1035, PT_UNICODE, message_id))
    props += list(extra)
    if recipients is None:
        recipients = [(1, n, a) for n, a in to] + [(2, n, a) for n, a in cc] + [(3, n, a) for n, a in bcc]
    return Message(name, props, recipients, attachments, flags, fai)


def token(name):
    return "PSTTOKEN-" + name


def standard_messages():
    rtf_html = (b"{\\rtf1\\ansi\\ansicpg1252\\fromhtml1 \\deff0{\\fonttbl{\\f0\\fswiss Arial;}}"
                b"{\\*\\htmltag19 <html>}{\\*\\htmltag34 <head>}{\\*\\htmltag41 </head>}{\\*\\htmltag50 <body>}\\htmlrtf {\\htmlrtf0 "
                b"{\\*\\htmltag84 <b>}\\htmlrtf {\\b\\htmlrtf0 " + token("RTFHTML").encode() + b"\\htmlrtf }\\htmlrtf0 "
                b"{\\*\\htmltag92 </b>}\\par {\\*\\htmltag84 <p>}Caf\\'e9 \\u8364?{\\*\\htmltag92 </p>}"
                b"{\\*\\htmltag58 </body>}{\\*\\htmltag27 </html>}\\htmlrtf }\\htmlrtf0 }")
    big_body = "".join("Line %04d of a body longer than one block. %s\r\n" % (i, "x" * 20) for i in range(160)) + token("BIGBODY") + "\r\n"
    blob = bytes((i * 7 + 3) & 0xFF for i in range(20000))
    signed_entity = (b"Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256; boundary=\"sig-boundary\"\r\n\r\n"
                     b"--sig-boundary\r\nContent-Type: text/plain\r\n\r\n" + token("SIGNED").encode() + b"\r\n"
                     b"--sig-boundary\r\nContent-Type: application/pkcs7-signature; name=smime.p7s\r\nContent-Transfer-Encoding: base64\r\n\r\n"
                     b"MIAGCSqGSIb3DQEHAqCAMIACAQExDzANBglghkgBZQMEAgEFADCABgkqhkiG9w0BBwEAAA==\r\n--sig-boundary--\r\n")
    transport = ("Received: from relay.example.org by mx.example.net; Fri, 1 Mar 2024 09:00:00 +0000\r\n"
                 "From: \"Carol Example\" <carol@example.org>\r\nTo: bob@example.org\r\nSubject: With transport headers\r\n"
                 "Date: Fri, 1 Mar 2024 08:59:00 +0000\r\nMessage-ID: <transport@fixture.test>\r\nMIME-Version: 1.0\r\n"
                 "Content-Type: multipart/alternative;\r\n\tboundary=\"stale-boundary-XYZ\"\r\nContent-Transfer-Encoding: 7bit\r\n"
                 "X-Fixture: kept\r\n\r\n")
    inner = mail("inner", "Inner message", token("INNER") + "\r\n",
                 attachments=[Attachment([(0x3707, PT_UNICODE, "inner.txt"), (0x370E, PT_UNICODE, "text/plain")],
                                         data=(token("INNERATTACH") + "\r\n").encode())])
    many = [(1, "Listed Person %03d" % i, "person%03d@example.org" % i) for i in range(180)]

    inbox = [
        mail("plain", "Plain text only", token("PLAIN") + "\r\nSecond line.\r\n", message_id="<plain@fixture.test>",
             delivery=filetime(2024, 3, 1, 9, 0, 0), submit=filetime(2024, 3, 1, 8, 59, 0)),
        mail("html1252", "HTML in windows-1252", None, flags=0x00,
             extra=[(0x1013, PT_BINARY, ("<html><body><p>" + token("HTML1252") + " Caf\u00e9</p></body></html>").encode("cp1252")),
                    (0x3FDE, PT_LONG, 1252)]),
        mail("mixed", "Plain, HTML and attachments", token("MIXED") + "\r\n",
             extra=[(0x1013, PT_BINARY, ("<html><body><p>" + token("MIXEDHTML") + "</p><img src=\"cid:logo@fixture\"></body></html>").encode("utf-8")),
                    (0x3FDE, PT_LONG, 65001), (0x1090, PT_LONG, 2)],
             attachments=[
                 Attachment([(0x3707, PT_UNICODE, "notes.txt"), (0x370E, PT_UNICODE, "text/plain")], data=(token("ATTACH") + "\r\n").encode()),
                 Attachment([(0x3707, PT_UNICODE, "data.bin")], data=blob),
                 Attachment([(0x3707, PT_UNICODE, "logo.png"), (0x370E, PT_UNICODE, "image/png"), (0x3712, PT_UNICODE, "logo@fixture"),
                             (0x7FFE, PT_BOOLEAN, True)], data=b"\x89PNG\r\n\x1a\n" + bytes(40)),
                 Attachment([(0x3707, PT_UNICODE, "C:\\Users\\x\\r\u00e9sum\u00e9\r\n.txt")], data=b"path and newline\r\n"),
                 Attachment([(0x3707, PT_UNICODE, "empty.txt")], data=b""),
             ]),
        mail("rtfhtml", "RTF carrying HTML", None, extra=[(0x1009, PT_BINARY, lzfu_compress(rtf_html))]),
        mail("transport", "With transport headers", token("TRANSPORT") + "\r\n", extra=[(0x007D, PT_UNICODE, transport)]),
        mail("cp1251", "\x01\x04RE: \u041f\u0440\u0438\u0432\u0435\u0442", None,
             extra=[(0x1000, PT_STRING8, ("\u041f\u0440\u0438\u0432\u0435\u0442 " + token("CP1251") + "\r\n").encode("cp1251")),
                    (0x3FFD, PT_LONG, 1251)]),
        mail("categories", "Answered, with categories", token("CATEGORIES") + "\r\n",
             extra=[(0x1081, PT_LONG, 102), (0x8000, PT_MV_UNICODE, ["Red category", "Work"])]),
        mail("embedded", "Carries a message", token("OUTER") + "\r\n",
             attachments=[Attachment([(0x3001, PT_UNICODE, "Inner message")], method=5, embedded=inner)]),
        mail("bigbody", "A long body", big_body),
        mail("unicode", "\u039a\u03b1\u03bb\u03b7\u03bc\u03ad\u03c1\u03b1 \u03ba\u03cc\u03c3\u03bc\u03b5 \U0001F600",
             token("UNICODE") + "\r\n", sender=("Evil\r\nBcc: victim@example.org", "evil@example.org")),
        mail("reference", "Attachments by reference", token("REFERENCE") + "\r\n",
             attachments=[Attachment([(0x3707, PT_UNICODE, "linked.docx")], method=2),
                          Attachment([(0x3707, PT_UNICODE, "cloud.docx")], method=7)]),
        mail("appointment", "An appointment in a mail folder", "Not mail.\r\n", message_class="IPM.Appointment"),
        mail("list", "To a long list", token("LIST") + "\r\n", recipients=many),
        mail("forwarded", "Forwarded, flag complete", token("FORWARDED") + "\r\n", flags=0x00,
             extra=[(0x1081, PT_LONG, 104), (0x1090, PT_LONG, 1)] +
                   # Forty properties nobody reads, of 400 bytes each: a property
                   # context whose heap spans three blocks.
                   [(0x6A00 + i, PT_UNICODE, ("Filler %02d " % i) * 20) for i in range(40)]),
        mail("smime", "Signed", None, message_class="IPM.Note.SMIME.MultipartSigned",
             attachments=[Attachment([(0x3707, PT_UNICODE, "smime.p7m"), (0x370E, PT_UNICODE, "multipart/signed")], data=signed_entity)]),
        mail("epoch", "Delivered in 1970", token("EPOCH") + "\r\n", delivery=filetime(1970, 1, 2, 0, 0, 0),
             submit=filetime(2023, 6, 15, 12, 30, 0)),
        mail("fai", "Associated", "FAI\r\n", message_class="IPM.Microsoft.FolderDesign.NamedView", fai=True),
    ]
    return inbox


def extra_messages():
    rtf_only = (b"{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fswiss Arial;}}\\f0\\fs20 " + token("RTFONLY").encode() +
                b" written in RTF\\par}")

    # Twelve messages, each embedded in the one before: the reader reads ten
    # deep (the outer message and nine inside it) and says what it left out.
    nested = mail("nest11", "Nested 11", token("NEST11") + "\r\n")
    for level in range(10, 0, -1):
        nested = mail("nest%02d" % level, "Nested %02d" % level, token("NEST%02d" % level) + "\r\n",
                      attachments=[Attachment([(0x3001, PT_UNICODE, "Nested %02d" % (level + 1))], method=5, embedded=nested)])
    twelve = mail("nested", "Twelve deep", token("NEST00") + "\r\n",
                  attachments=[Attachment([(0x3001, PT_UNICODE, "Nested 01")], method=5, embedded=nested)])

    # Three categories of 200 characters: two fit in the 500 a message's
    # keywords may take, and the third does not.
    categories = ["Category %d %s" % (i, "x" * 189) for i in range(3)]

    opaque = b"0\x80\x06\x09*\x86H\x86\xf7\r\x01\x07\x03\xa0\x80" + token("OPAQUE").encode()

    return [
        mail("rtfonly", "RTF only", None, extra=[(0x1009, PT_BINARY, lzfu_compress(rtf_only))]),
        twelve,
        mail("longcategories", "Categories past what a message carries", token("LONGCATEGORIES") + "\r\n",
             extra=[(0x8000, PT_MV_UNICODE, categories)]),
        mail("cp37", "HTML in a code page this reader does not name", None,
             extra=[(0x1013, PT_BINARY, ("<p>" + token("CP37") + "</p>").encode("ascii")), (0x3FDE, PT_LONG, 37)]),
        mail("opaque", "Opaque S/MIME", None, message_class="IPM.Note.SMIME",
             attachments=[Attachment([(0x3707, PT_UNICODE, "smime.p7m"), (0x370E, PT_UNICODE, "application/pkcs7-mime")], data=opaque)]),
        mail("behalf", "Sent on behalf of another", token("BEHALF") + "\r\n", sender=None,
             extra=[(0x0042, PT_UNICODE, "Boss Example"), (0x0064, PT_UNICODE, "SMTP"), (0x0065, PT_UNICODE, "boss@example.org"),
                    (0x0C1A, PT_UNICODE, "Assistant Example"), (0x0C1E, PT_UNICODE, "SMTP"), (0x0C1F, PT_UNICODE, "assistant@example.org")]),
        mail("ids", "A reply in a thread", token("IDS") + "\r\n", message_id="<ids@fixture.test>",
             extra=[(0x1042, PT_UNICODE, "<parent@fixture.test>"), (0x1039, PT_UNICODE, "<root@fixture.test> <parent@fixture.test>")]),
        mail("longid", "A Message-ID longer than a line", token("LONGID") + "\r\n", message_id="<" + "x" * 1000 + "@fixture.test>"),
    ]


def own_nid(index):
    """A folder NID of its own, far above what the counter gives, for a folder
    added after the tests named the others' NIDs."""
    return ((0x2000 + index) << 5) | NID_TYPE_NORMAL_FOLDER


def standard(ndb):
    sent = Folder("Gesendete Elemente", messages=[
        mail("sent", "Sent from an Exchange account", token("SENT") + "\r\n", sender=None,
             extra=[(0x0042, PT_UNICODE, "Alice Example"), (0x0064, PT_UNICODE, "EX"),
                    (0x0065, PT_UNICODE, "/O=EXAMPLE/OU=EXCHANGE ADMINISTRATIVE GROUP/CN=RECIPIENTS/CN=ALICE"),
                    (0x5D02, PT_UNICODE, "alice@example.org"),
                    (0x0C1A, PT_UNICODE, "Alice Example"), (0x0C1E, PT_UNICODE, "EX"),
                    (0x0C1F, PT_UNICODE, "/O=EXAMPLE/OU=EXCHANGE ADMINISTRATIVE GROUP/CN=RECIPIENTS/CN=ALICE"),
                    (0x5D01, PT_UNICODE, "alice@example.org")],
             to=(("Bob Example", "bob@example.org"),), cc=(("Carol Example", "carol@example.org"),),
             bcc=(("Dave Example", "dave@example.org"),))])
    ordinary_sent = Folder("Sent Items", messages=[mail("ordinarysent", "In an ordinary folder called Sent Items", token("ORDINARYSENT") + "\r\n")])
    # The special folders under names IMAP's name table does not hold: only
    # their EntryIDs say what they are.
    deleted = Folder("Gel\u00f6schte Elemente", messages=[mail("deleted", "Deleted", token("DELETED") + "\r\n")])
    drafts = Folder("Entw\u00fcrfe", messages=[mail("draft", "Unsent", token("DRAFT") + "\r\n", flags=0x08)])
    junk = Folder("Junk-E-Mail", messages=[mail("junk", "Junk", token("JUNK") + "\r\n")])
    q3 = Folder("Q3", messages=[mail("q3", "Three levels down", token("Q3") + "\r\n")])
    projects = Folder("Projects", children=[Folder("2025", children=[q3])])
    dotted = Folder("Notes.Archive/Old", messages=[mail("dotted", "In a folder with a dot and a slash", token("DOTTED") + "\r\n")])
    hashed = Folder("#hashtag", messages=[mail("hashed", "In a folder whose name starts with #", token("HASHED") + "\r\n")])
    umlaut = Folder("\u00dcberpr\u00fcfung \u2713", messages=[mail("umlaut", "In a folder with a non-ASCII name", token("UMLAUT") + "\r\n")])
    contacts = Folder("Contacts", "IPF.Contact", messages=[
        mail("contact1", "Ann Contact", None, message_class="IPM.Contact"),
        mail("contact2", "Bert Contact", None, message_class="IPM.Contact")])
    calendar = Folder("Calendar", "IPF.Appointment", messages=[mail("meeting", "A meeting", None, message_class="IPM.Appointment")])
    inbox = Folder("Inbox", messages=standard_messages())

    # Added after the tests named NIDs, with NIDs of their own: an ordinary
    # folder called Junk E-mail that no EntryID names, the Extras, and a chain
    # 27 folders deep whose last holds one message.
    ordinary_junk = Folder("Junk E-mail", messages=[mail("ordinaryjunk", "In an ordinary folder called Junk E-mail",
                                                          token("ORDINARYJUNK") + "\r\n")], nid=own_nid(0))
    extras = Folder("Extras", messages=extra_messages(), nid=own_nid(1))
    deepest = Folder("L27", messages=[mail("deep", "Twenty-seven folders down", token("DEEP") + "\r\n")], nid=own_nid(10 + 27))
    chain = deepest
    for level in range(26, 1, -1):
        chain = Folder("L%02d" % level, children=[chain], nid=own_nid(10 + level))
    deep = Folder("Deep", children=[chain], nid=own_nid(2))

    ipm = Folder("Top of Personal Folders", None,
                 children=[inbox, sent, ordinary_sent, deleted, drafts, junk, projects, dotted, hashed, umlaut, contacts, calendar,
                           ordinary_junk, extras, deep])
    finder = Folder("Search Root", None, children=[Folder("A search folder", None, search=True)])
    outside = Folder("Outside the subtree", messages=[mail("outside", "Never imported", token("OUTSIDE") + "\r\n")])
    roles = {"ipm": ipm, "sent": sent, "wastebasket": deleted, "drafts": drafts, "junk": junk, "finder": finder}
    build(ndb, roles, [ipm, finder, outside])


def mini(ndb, store_extra=(), **build_options):
    blob = bytes((i * 13 + 5) & 0xFF for i in range(9000))
    inbox = Folder("Inbox", messages=[
        mail("mini1", "Mini one", token("MINI1") + "\r\n", message_id="<mini1@fixture.test>"),
        mail("mini2", "Mini two", token("MINI2") + "\r\n", flags=0x00,
             extra=[(0x1013, PT_BINARY, ("<p>" + token("MINI2HTML") + "</p>").encode("utf-8")), (0x3FDE, PT_LONG, 65001)]),
        mail("mini3", "Mini three", token("MINI3") + "\r\n",
             attachments=[Attachment([(0x3707, PT_UNICODE, "blob.bin")], data=blob)]),
    ])
    archive = Folder("Archive", messages=[mail("mini4", "Mini four", token("MINI4") + "\r\n")])
    ipm = Folder("Top of Personal Folders", None, children=[inbox, archive])
    build(ndb, {"ipm": ipm}, [ipm], store_extra, **build_options)
    return inbox


def inbox_roles(ndb):
    inbox = Folder("Inbox", messages=[mail("inboxroles", "In the Inbox", token("ROLESINBOX") + "\r\n")])
    drafts = Folder("Brouillons", messages=[mail("brouillon", "Pas envoy\u00e9", token("BROUILLON") + "\r\n", flags=0x08)])
    junk = Folder("Courrier ind\u00e9sirable", messages=[mail("indesirable", "Ind\u00e9sirable", token("INDESIRABLE") + "\r\n")])
    ipm = Folder("Top of Personal Folders", None, children=[inbox, drafts, junk])
    build(ndb, {"ipm": ipm, "drafts": drafts, "junk": junk}, [ipm], roles_on=inbox)


def many(ndb):
    inbox = Folder("Inbox", messages=[mail("many%03d" % i, "Message %03d of many" % i, token("MANY%03d" % i) + "\r\n",
                                           message_id="<many%03d@fixture.test>" % i, to=())
                                      for i in range(1, 91)])
    archive = Folder("Archive", messages=[mail("manyarchive", "Archived", token("MANYARCHIVE") + "\r\n")])
    ipm = Folder("Top of Personal Folders", None, children=[inbox, archive])
    build(ndb, {"ipm": ipm}, [ipm])


# ---------------------------------------------------------------- an ANSI file

ANSI_RECIPIENT_COLUMNS = [(0x0C15, PT_LONG), (0x3001, PT_STRING8), (0x3002, PT_STRING8), (0x3003, PT_STRING8), (0x39FE, PT_STRING8)]


def ansi_mail(name, subject, body, code_page, **options):
    """A message as Outlook 2002 wrote one: every string PT_STRING8, in the code
    page PidTagMessageCodepage names."""
    message = mail(name, None, None, **options)
    encoding = "cp%d" % code_page
    props = []
    for pid, ptype, value in message.props:
        props.append((pid, PT_STRING8, value.encode(encoding)) if ptype == PT_UNICODE else (pid, ptype, value))
    props += [(0x0037, PT_STRING8, subject.encode(encoding)), (0x1000, PT_STRING8, body.encode(encoding)), (0x3FFD, PT_LONG, code_page)]
    message.props = props
    message.recipient_columns = ANSI_RECIPIENT_COLUMNS
    return message


def ansi(ndb):
    blob = bytes((i * 17 + 11) & 0xFF for i in range(12000))
    inbox = Folder("Inbox", messages=[
        ansi_mail("ansi1", "Café in Windows-1252", "Café crème " + token("ANSI1") + "\r\n", 1252, message_id="<ansi1@fixture.test>"),
        ansi_mail("ansi2", "Привет", "Привет " + token("ANSI2") + "\r\n", 1251, flags=0x00),
        ansi_mail("ansi3", "With an attachment", token("ANSI3") + "\r\n", 1252,
                  attachments=[Attachment([(0x3707, PT_STRING8, b"blob.bin")], data=blob)]),
    ])
    drafts = Folder("Entwürfe", messages=[ansi_mail("ansi4", "Unsent", token("ANSI4") + "\r\n", 1252, flags=0x08)])
    ipm = Folder("Top of Personal Folders", None, children=[inbox, drafts])
    build(ndb, {"ipm": ipm, "drafts": drafts}, [ipm], string_type=PT_STRING8)


def ansi_header_only(version=14, crypt=1):
    """An ANSI header, padded to a Unicode header's length, and nothing else."""
    header = bytearray(564)
    header[0:4] = b"!BDN"
    header[8:10] = b"SM"
    struct.pack_into("<HHBB", header, 10, version, 19, 1, 1)
    struct.pack_into("<I", header, 168, 564)
    header[460] = 0x80
    header[461] = crypt
    struct.pack_into("<I", header, 4, crc(bytes(header[8:8 + 471])))
    return bytes(header)


# ---------------------------------------------------------------- contacts, calendar, tasks, notes, journal

PT_DOUBLE = 0x0005

PSETID_ADDRESS = bytes.fromhex("0420060000000000c000000000000046")      # {00062004-0000-0000-C000-000000000046}
PSETID_APPOINTMENT = bytes.fromhex("0220060000000000c000000000000046")  # {00062002-...}
PSETID_TASK = bytes.fromhex("0320060000000000c000000000000046")         # {00062003-...}
PSETID_COMMON = bytes.fromhex("0820060000000000c000000000000046")       # {00062008-...}
PSETID_LOG = bytes.fromhex("0a20060000000000c000000000000046")          # {0006200A-...}
PSETID_MEETING = bytes.fromhex("90dad86e0b451b1098da00aa003f1305")      # {6ED8DA90-450B-101B-98DA-00AA003F1305}

# The named properties items.pst uses ([MS-OXPROPS]), each given the next
# property id from 0x8001 in this order - ids no reader can guess, so a reader
# that finds them finds them through the name-to-id map.
ITEM_NAMES = [
    (PSETID_ADDRESS, 0x8005), (PSETID_ADDRESS, 0x8015), (PSETID_ADDRESS, 0x8045), (PSETID_ADDRESS, 0x8046),
    (PSETID_ADDRESS, 0x8047), (PSETID_ADDRESS, 0x8048), (PSETID_ADDRESS, 0x8049), (PSETID_ADDRESS, 0x8080),
    (PSETID_ADDRESS, 0x8082), (PSETID_ADDRESS, 0x8083), (PSETID_ADDRESS, 0x8084), (PSETID_ADDRESS, 0x8092),
    (PSETID_ADDRESS, 0x8093), (PSETID_ADDRESS, 0x80A2), (PSETID_ADDRESS, 0x80A3), (PSETID_ADDRESS, 0x80A4),
    (PSETID_ADDRESS, 0x80DE),
    (PSETID_APPOINTMENT, 0x8205), (PSETID_APPOINTMENT, 0x8208), (PSETID_APPOINTMENT, 0x820D), (PSETID_APPOINTMENT, 0x820E),
    (PSETID_APPOINTMENT, 0x8215), (PSETID_APPOINTMENT, 0x8216), (PSETID_APPOINTMENT, 0x8217), (PSETID_APPOINTMENT, 0x8223),
    (PSETID_APPOINTMENT, 0x8233), (PSETID_APPOINTMENT, 0x8234), (PSETID_APPOINTMENT, 0x825E), (PSETID_APPOINTMENT, 0x8260),
    (PSETID_COMMON, 0x8501), (PSETID_COMMON, 0x8503),
    (PSETID_TASK, 0x8101), (PSETID_TASK, 0x8102), (PSETID_TASK, 0x8104), (PSETID_TASK, 0x8105), (PSETID_TASK, 0x810F),
    (PSETID_TASK, 0x811C),
    (PSETID_MEETING, 0x0003),
    (PSETID_LOG, 0x8700), (PSETID_LOG, 0x8706), (PSETID_LOG, 0x8707), (PSETID_LOG, 0x8708),
]
NAMED_ID = {key: 0x8001 + i for i, key in enumerate(ITEM_NAMES)}


def named(guid, lid):
    return NAMED_ID[(guid, lid)]


def minutes_1601(year, month, day, hour=0, minute=0):
    import calendar
    return (calendar.timegm((year, month, day, hour, minute, 0, 0, 0, 0)) + 11644473600) // 60


def system_time(year, month, day_of_week, day, hour, minute=0):
    """[MS-OXOCAL] 2.2.1.39's SYSTEMTIME: with year 0, day is the week (5 the last)."""
    return struct.pack("<8H", year, month, day_of_week, day, hour, minute, 0, 0)


# W. Europe Standard Time: UTC+1, and +2 from the last Sunday of March at 02:00
# to the last Sunday of October at 03:00.
W_EUROPE_STANDARD = system_time(0, 10, 0, 5, 3)
W_EUROPE_DAYLIGHT = system_time(0, 3, 0, 5, 2)


def time_zone_struct():
    """PidLidTimeZoneStruct ([MS-OXOCAL] 2.2.1.39): lBias, lStandardBias,
    lDaylightBias, wStandardYear, stStandardDate, wDaylightYear, stDaylightDate."""
    return struct.pack("<iiiH", -60, 0, -60, 0) + W_EUROPE_STANDARD + struct.pack("<H", 0) + W_EUROPE_DAYLIGHT


def time_zone_definition():
    """A TZDEFINITION ([MS-OXOCAL] 2.2.1.41.1) of one TZRULE, marked effective."""
    key = "W. Europe Standard Time"
    header = struct.pack("<HH", 0x0002, len(key)) + utf16(key) + struct.pack("<H", 1)
    rule = (struct.pack("<BBHHH", 2, 1, 0x003E, 0x0002, 2007) + bytes(14) + struct.pack("<iii", -60, 0, -60) +
            W_EUROPE_STANDARD + W_EUROPE_DAYLIGHT)
    return struct.pack("<BBH", 2, 1, len(header)) + header + rule


def global_object_id(uid):
    """PidLidGlobalObjectId ([MS-OXOCAL] 2.2.1.27) carrying an iCalendar UID in
    its Data, as [MS-OXCICAL] 2.1.3.1.1.20.26 reads it back."""
    data = b"vCal-Uid" + struct.pack("<I", 1) + uid.encode("ascii") + b"\0"
    return bytes.fromhex("040000008200e00074c5b7101a82e008") + bytes(4) + bytes(8) + bytes(8) + struct.pack("<I", len(data)) + data


ARO_SUBJECT = 0x0001
ARO_LOCATION = 0x0010


def recurrence(frequency, pattern_type, period, specific, end_type, count, start_date, end_date, start_offset, end_offset,
               deleted=(), modified=(), exceptions=(), first_dow=1, reader=0x3004):
    """PidLidAppointmentRecur ([MS-OXOCAL] 2.2.1.44.5): a RecurrencePattern and
    what an appointment's adds - the times of day, the exceptions and their
    Unicode subjects and locations. exceptions: [(start, end, original, subject,
    location)], minutes since 1601 in the series' zone."""
    out = struct.pack("<HHHHH", reader, 0x3004, frequency, pattern_type, 0)
    out += struct.pack("<III", start_date % max(period, 1), period, 0) + specific
    out += struct.pack("<IIII", end_type, count, first_dow, len(deleted)) + b"".join(struct.pack("<I", d) for d in deleted)
    out += struct.pack("<I", len(modified)) + b"".join(struct.pack("<I", d) for d in modified)
    out += struct.pack("<II", start_date, end_date)
    out += struct.pack("<IIIIH", 0x3006, 0x3009, start_offset, end_offset, len(exceptions))
    for start, end, original, subject, location in exceptions:
        flags = (ARO_SUBJECT if subject is not None else 0) | (ARO_LOCATION if location is not None else 0)
        out += struct.pack("<IIIH", start, end, original, flags)
        if subject is not None:
            eight = subject.encode("cp1252")
            out += struct.pack("<HH", len(eight) + 1, len(eight)) + eight
        if location is not None:
            eight = location.encode("cp1252")
            out += struct.pack("<HH", len(eight) + 1, len(eight)) + eight
    out += struct.pack("<I", 0)                                             # ReservedBlock1Size
    for start, end, original, subject, location in exceptions:
        out += struct.pack("<III", 4, 0, 0)                                 # ChangeHighlight (size, value), ReservedBlockEE1Size
        if subject is not None or location is not None:
            out += struct.pack("<III", start, end, original)
            if subject is not None:
                out += struct.pack("<H", len(utf16(subject)) // 2) + utf16(subject)
            if location is not None:
                out += struct.pack("<H", len(utf16(location)) // 2) + utf16(location)
            out += struct.pack("<I", 0)                                     # ReservedBlockEE2Size
    return out + struct.pack("<I", 0)                                       # ReservedBlock2Size


def item(name, message_class, subject, props, recipients=None, attachments=(), flags=0x01):
    message = Message(name, [(0x001A, PT_UNICODE, message_class), (0x0037, PT_UNICODE, subject),
                             (0x3007, PT_SYSTIME, T0 - 86400 * 10000000), (0x3008, PT_SYSTIME, T0)] + list(props),
                      [], attachments, flags)
    if recipients:
        message.recipient_columns = RECIPIENT_COLUMNS + [(0x5FFD, PT_LONG), (0x5FFF, PT_LONG)]
        message.recipient_rows = [(i + 1, {0x0C15: kind, 0x3001: rname, 0x3002: "SMTP", 0x3003: address, 0x39FE: address,
                                           0x5FFD: rflags, 0x5FFF: track})
                                  for i, (kind, rname, address, rflags, track) in enumerate(recipients)]
    return message


def items(ndb):
    A = PSETID_ADDRESS
    P = PSETID_APPOINTMENT
    T = PSETID_TASK
    jpeg = b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00" + bytes(range(64)) + b"\xff\xd9"
    photo = Attachment([(0x3707, PT_UNICODE, "ContactPicture.jpg"), (0x7FFF, PT_BOOLEAN, True)], data=jpeg)

    contacts = Folder("Contacts", "IPF.Contact", messages=[
        item("ann", "IPM.Contact", "Ann Example", [
            (0x3001, PT_UNICODE, "Ann Example"), (0x3A06, PT_UNICODE, "Ann"), (0x3A11, PT_UNICODE, "Example"),
            (0x3A44, PT_UNICODE, "Q"), (0x3A45, PT_UNICODE, "Dr."), (0x3A4F, PT_UNICODE, "Annie"),
            (0x3A16, PT_UNICODE, "Example Ltd"), (0x3A17, PT_UNICODE, "Director"), (0x3A18, PT_UNICODE, "Sales"),
            (0x3A08, PT_UNICODE, "+44 20 7946 0000"), (0x3A1C, PT_UNICODE, "+44 7700 900000"), (0x3A25, PT_UNICODE, "+44 20 7946 0001"),
            (0x3A5D, PT_UNICODE, "1 High Street"), (0x3A59, PT_UNICODE, "London"), (0x3A5B, PT_UNICODE, "SW1A 1AA"),
            (0x3A5A, PT_UNICODE, "United Kingdom"),
            (named(A, 0x8045), PT_UNICODE, "2 Low Road"), (named(A, 0x8046), PT_UNICODE, "Leeds"),
            (named(A, 0x8048), PT_UNICODE, "LS1 1AA"), (named(A, 0x8049), PT_UNICODE, "United Kingdom"),
            (0x3A42, PT_SYSTIME, filetime(1980, 4, 11, 23, 0, 0)), (named(A, 0x80DE), PT_SYSTIME, filetime(1980, 4, 12)),
            (0x1000, PT_UNICODE, "Met at the fair; bring the samples.\r\n" + token("CONTACTNOTE")),
            (0x3A51, PT_UNICODE, "https://example.org/ann"), (named(A, 0x8005), PT_UNICODE, "Example, Ann"),
            (named(A, 0x8080), PT_UNICODE, "Ann Example (ann@example.org)"), (named(A, 0x8082), PT_UNICODE, "SMTP"),
            (named(A, 0x8083), PT_UNICODE, "Ann@Example.org"), (named(A, 0x8084), PT_UNICODE, "ann@example.org"),
            (named(A, 0x8092), PT_UNICODE, "SMTP"), (named(A, 0x8093), PT_UNICODE, "ann.private@example.net"),
            (named(A, 0x80A2), PT_UNICODE, "EX"), (named(A, 0x80A3), PT_UNICODE, "/O=EXAMPLE/OU=EXCHANGE/CN=RECIPIENTS/CN=ANN"),
            (named(A, 0x80A4), PT_UNICODE, "ann@exchange.example.org"), (named(A, 0x8015), PT_BOOLEAN, True),
            (0x8000, PT_MV_UNICODE, ["Customers"])], attachments=[photo]),
        item("nomail", "IPM.Contact", "Nora Nomail", [(0x3001, PT_UNICODE, "Nora Nomail"), (0x3A09, PT_UNICODE, "+1 555 0100")]),
        item("list", "IPM.DistList", "Team list", [(0x3001, PT_UNICODE, "Team list")]),
        # Notes of 270,000 bytes, eight-bit: more than the 256 KB a contact keeps.
        item("long", "IPM.Contact", "Lena Long", [(0x3001, PT_UNICODE, "Lena Long"), (named(A, 0x8082), PT_UNICODE, "SMTP"),
                                                (named(A, 0x8083), PT_UNICODE, "lena@example.org"),
                                                (0x1000, PT_STRING8, (token("LONGNOTE") + "\r\n" + "".join(
                                                    "Long note, line %05d of many.\r\n" % i for i in range(9000)))[:270000].encode("ascii"))]),
        # An address the account will have collected from mail it sent.
        item("colin", "IPM.Contact", "Colin Collected", [(0x3001, PT_UNICODE, "Colin Collected"), (named(A, 0x8082), PT_UNICODE, "SMTP"),
                                                       (named(A, 0x8083), PT_UNICODE, "colin@example.test"),
                                                       (0x3A1C, PT_UNICODE, "+44 7700 900999")]),
    ])
    # What Exchange keeps for itself, and what Outlook does not show: never
    # imported, though the walk meets them before the Suppliers folder.
    recipient_cache = Folder("Recipient Cache", "IPF.Contact.RecipientCache", messages=[
        item("samcache", "IPM.Contact", "Sam Cached", [(0x3001, PT_UNICODE, "Sam Cached"), (named(A, 0x8082), PT_UNICODE, "SMTP"),
                                                     (named(A, 0x8083), PT_UNICODE, "sam@supplier.example")]),
    ])
    hidden = Folder("Hidden contacts", "IPF.Contact", messages=[
        item("harriet", "IPM.Contact", "Harriet Hidden", [(0x3001, PT_UNICODE, "Harriet Hidden"), (named(A, 0x8082), PT_UNICODE, "SMTP"),
                                                        (named(A, 0x8083), PT_UNICODE, "harriet@example.org")]),
    ])
    hidden.extra.append((0x10F4, PT_BOOLEAN, True))
    # A contacts folder under Deleted Items, which the walk meets first: its
    # copy of Ann's address loses to the live one, its own contact comes in.
    old_contacts = Folder("Old contacts", "IPF.Contact", messages=[
        item("annold", "IPM.Contact", "Ann Old", [(0x3001, PT_UNICODE, "Ann Old"), (named(A, 0x8082), PT_UNICODE, "SMTP"),
                                                (named(A, 0x8083), PT_UNICODE, "ann@example.org")]),
        item("olga", "IPM.Contact", "Olga Old", [(0x3001, PT_UNICODE, "Olga Old"), (named(A, 0x8082), PT_UNICODE, "SMTP"),
                                               (named(A, 0x8083), PT_UNICODE, "olga@example.org")]),
    ])
    # Named as the account's default book: under Deleted Items it has no book of
    # its own, and is left out.
    deleted_contacts = Folder("Contacts", "IPF.Contact", messages=[
        item("dora", "IPM.Contact", "Dora Deleted", [(0x3001, PT_UNICODE, "Dora Deleted"), (named(A, 0x8082), PT_UNICODE, "SMTP"),
                                                   (named(A, 0x8083), PT_UNICODE, "dora@example.org")]),
    ])
    deleted_items = Folder("Deleted Items", children=[old_contacts, deleted_contacts])
    suppliers = Folder("Suppliers", "IPF.Contact", messages=[
        item("sam", "IPM.Contact", "Sam Supplier", [(0x3001, PT_UNICODE, "Sam Supplier"), (named(A, 0x8082), PT_UNICODE, "SMTP"),
                                                  (named(A, 0x8083), PT_UNICODE, "sam@supplier.example")]),
    ])

    def appointment(name, subject, start, end, extra=(), recipients=None):
        props = [(named(P, 0x820D), PT_SYSTIME, start), (named(P, 0x820E), PT_SYSTIME, end)] if start else []
        return item(name, "IPM.Appointment", subject, props + list(extra), recipients)

    weekly = recurrence(0x200B, 0x0001, 1, struct.pack("<I", 0x02), 0x2022, 6,
                        minutes_1601(2024, 3, 4), minutes_1601(2024, 4, 8), 9 * 60, 9 * 60 + 30,
                        deleted=[minutes_1601(2024, 3, 18), minutes_1601(2024, 3, 25)], modified=[minutes_1601(2024, 3, 25)],
                        exceptions=[(minutes_1601(2024, 3, 25, 10), minutes_1601(2024, 3, 25, 10, 30), minutes_1601(2024, 3, 25, 9),
                                     "Weekly sync (moved)", "Room 2")])
    monthly = recurrence(0x200C, 0x0003, 1, struct.pack("<II", 0x04, 2), 0x2021, 4,
                         minutes_1601(2024, 3, 12), minutes_1601(2024, 6, 11), 14 * 60, 15 * 60)
    yearly = recurrence(0x200D, 0x0002, 12, struct.pack("<I", 20), 0x2023, 10,
                        minutes_1601(2024, 3, 20), 0x5AE980DF, 0, 24 * 60)
    zone = [(named(P, 0x8233), PT_BINARY, time_zone_struct()), (named(P, 0x8234), PT_UNICODE, "(UTC+01:00) Amsterdam, Berlin"),
            (named(P, 0x8260), PT_BINARY, time_zone_definition())]

    calendar = Folder("Calendar", "IPF.Appointment", messages=[
        appointment("dentist", "Dentist", filetime(2024, 3, 7, 15, 0), filetime(2024, 3, 7, 16, 0),
                    [(named(P, 0x8208), PT_UNICODE, "Main Street 1"), (0x1000, PT_UNICODE, "Bring the card.\r\n"),
                     (named(PSETID_COMMON, 0x8503), PT_BOOLEAN, True), (named(PSETID_COMMON, 0x8501), PT_LONG, 30),
                     (0x0036, PT_LONG, 2), (named(P, 0x8205), PT_LONG, 2)]),
        appointment("holiday", "Holiday", filetime(2024, 3, 28, 23, 0), filetime(2024, 3, 29, 23, 0),
                    [(named(P, 0x8215), PT_BOOLEAN, True), (named(P, 0x8205), PT_LONG, 0),
                     (named(P, 0x825E), PT_BINARY, time_zone_definition())]),
        appointment("weekly", "Weekly sync", filetime(2024, 3, 4, 8, 0), filetime(2024, 3, 4, 8, 30),
                    [(named(P, 0x8223), PT_BOOLEAN, True), (named(P, 0x8216), PT_BINARY, weekly), (named(P, 0x8208), PT_UNICODE, "Room 1")] + zone),
        appointment("monthly", "Board meeting", filetime(2024, 3, 12, 13, 0), filetime(2024, 3, 12, 14, 0),
                    [(named(P, 0x8223), PT_BOOLEAN, True), (named(P, 0x8216), PT_BINARY, monthly)] + zone),
        appointment("yearly", "Anniversary", filetime(2024, 3, 19, 23, 0), filetime(2024, 3, 20, 23, 0),
                    [(named(P, 0x8223), PT_BOOLEAN, True), (named(P, 0x8216), PT_BINARY, yearly), (named(P, 0x8215), PT_BOOLEAN, True)] + zone),
        appointment("kickoff", "Project kickoff", filetime(2024, 3, 14, 10, 0), filetime(2024, 3, 14, 11, 0),
                    [(named(P, 0x8217), PT_LONG, 1), (named(PSETID_MEETING, 0x0003), PT_BINARY, global_object_id("kickoff-uid@example.org")),
                     (0x0042, PT_UNICODE, "Alice Example"), (0x0064, PT_UNICODE, "SMTP"), (0x0065, PT_UNICODE, "alice@example.org")],
                    recipients=[(1, "Alice Example", "alice@example.org", 0x0003, 1), (1, "Bob Example", "bob@example.org", 0x0001, 3),
                                (2, "Carol Example", "carol@example.org", 0x0001, 2), (3, "Room A", "rooma@example.org", 0x0001, 0)]),
        appointment("broken", "Broken series", filetime(2024, 3, 5, 8, 0), filetime(2024, 3, 5, 9, 0),
                    [(named(P, 0x8223), PT_BOOLEAN, True),
                     (named(P, 0x8216), PT_BINARY, recurrence(0x200A, 0x0000, 1440, b"", 0x2023, 0, minutes_1601(2024, 3, 5), 0x5AE980DF,
                                                                 540, 600, reader=0x1234))] + zone),
        appointment("notime", "No time given", None, None),
        # A zone named by PidLidTimeZoneStruct and its description alone.
        appointment("standup", "Standup", filetime(2024, 3, 20, 8, 0), filetime(2024, 3, 20, 8, 15),
                    [(named(P, 0x8223), PT_BOOLEAN, True),
                     (named(P, 0x8216), PT_BINARY, recurrence(0x200B, 0x0001, 1, struct.pack("<I", 0x08), 0x2022, 3,
                                                               minutes_1601(2024, 3, 20), minutes_1601(2024, 4, 3), 9 * 60, 9 * 60 + 15)),
                     (named(P, 0x8233), PT_BINARY, time_zone_struct()),
                     (named(P, 0x8234), PT_UNICODE, "(UTC+01:00) Amsterdam, Berlin")]),
    ])
    # A calendar folder whose name is longer than a calendar's may be, with an
    # appointment whose description Outlook kept only as RTF.
    offsite_rtf = b"{\\rtf1\\ansi\\ansicpg1252 Bring boots. " + token("RTFBODY").encode() + b"\\par}"
    long_calendar = Folder("Team " + "x" * 295, "IPF.Appointment", messages=[
        appointment("offsite", "Offsite", filetime(2024, 3, 21, 9, 0), filetime(2024, 3, 21, 17, 0),
                    [(0x1009, PT_BINARY, lzfu_compress(offsite_rtf))]),
    ])

    tasks = Folder("Tasks", "IPF.Task", messages=[
        item("report", "IPM.Task", "File the report", [
            (named(T, 0x8104), PT_SYSTIME, filetime(2024, 3, 10)), (named(T, 0x8105), PT_SYSTIME, filetime(2024, 3, 15)),
            (named(T, 0x8101), PT_LONG, 1), (named(T, 0x8102), PT_DOUBLE, 0.5), (0x0017, PT_LONG, 2),
            (0x1000, PT_UNICODE, "The quarterly one.\r\n"), (0x8000, PT_MV_UNICODE, ["Work"])]),
        item("renew", "IPM.Task", "Renew the domain", [
            (named(T, 0x8105), PT_SYSTIME, filetime(2024, 3, 1)), (named(T, 0x8101), PT_LONG, 2), (named(T, 0x8102), PT_DOUBLE, 1.0),
            (named(T, 0x811C), PT_BOOLEAN, True), (named(T, 0x810F), PT_SYSTIME, filetime(2024, 3, 2))]),
        item("someday", "IPM.Task", "Someday", [(named(T, 0x8105), PT_SYSTIME, filetime(4501, 1, 1)), (named(T, 0x8101), PT_LONG, 0)]),
    ])

    notes = Folder("Notes", "IPF.StickyNote", messages=[
        item("note", "IPM.StickyNote", "Buy milk", [(0x1000, PT_UNICODE, "Buy milk\r\nand bread " + token("NOTE") + "\r\n")]),
    ])
    journal = Folder("Journal", "IPF.Journal", messages=[
        item("call", "IPM.Activity", "Call with Ann", [
            (named(PSETID_LOG, 0x8700), PT_UNICODE, "Phone call"), (named(PSETID_LOG, 0x8706), PT_SYSTIME, filetime(2024, 3, 5, 10, 0)),
            (named(PSETID_LOG, 0x8708), PT_SYSTIME, filetime(2024, 3, 5, 10, 15)), (named(PSETID_LOG, 0x8707), PT_LONG, 15),
            (0x1000, PT_UNICODE, "Talked about the samples. " + token("JOURNAL") + "\r\n")]),
    ])
    inbox = Folder("Inbox", messages=[mail("itemsmail", "Mail beside the items", token("ITEMSMAIL") + "\r\n")])
    # A deleted copy of the kickoff meeting, its UID the live one's: looked for
    # across the account's calendars, it is already there.
    old_calendar = Folder("Old calendar", "IPF.Appointment", messages=[
        appointment("kickoffcopy", "Project kickoff (deleted copy)", filetime(2024, 3, 14, 10, 0), filetime(2024, 3, 14, 11, 0),
                    [(named(PSETID_MEETING, 0x0003), PT_BINARY, global_object_id("kickoff-uid@example.org"))]),
    ])
    deleted_items.children.append(old_calendar)
    ipm = Folder("Top of Personal Folders", None, children=[deleted_items, inbox, recipient_cache, hidden, contacts, suppliers, calendar,
                                                             long_calendar, tasks, notes, journal])
    roles = {"ipm": ipm, "contacts": contacts, "calendar": calendar, "tasks": tasks, "notes": notes, "journal": journal,
             "wastebasket": deleted_items}
    build(ndb, roles, [ipm], named=[(guid, lid, NAMED_ID[(guid, lid)]) for guid, lid in ITEM_NAMES])


def clash(ndb):
    P = PSETID_APPOINTMENT

    def appointment(name, subject, day, uid):
        return item(name, "IPM.Appointment", subject, [
            (named(P, 0x820D), PT_SYSTIME, filetime(2024, 3, day, 10, 0)), (named(P, 0x820E), PT_SYSTIME, filetime(2024, 3, day, 11, 0)),
            (named(PSETID_MEETING, 0x0003), PT_BINARY, global_object_id(uid))])

    calendar = Folder("Calendar", "IPF.Appointment", messages=[
        appointment("first", "First", 11, "clash+1@example.org"),
        appointment("second", "Second", 12, "clash=1@example.org"),
        appointment("third", "Third", 13, "clash!1@example.org"),
        appointment("fourth", "Fourth", 14, "fourth@example.org"),
    ])
    # A calendar of its own whose last item is a twin: nothing after it can show
    # the store works, so its count decides, in its fourth run.
    tail = Folder("Tail", "IPF.Appointment", messages=[
        appointment("tailfirst", "Tail first", 15, "tail+1@example.org"),
        appointment("taillast", "Tail last", 16, "tail=1@example.org"),
    ])
    inbox = Folder("Inbox", messages=[mail("clashmail", "Mail beside the calendar", token("CLASHMAIL") + "\r\n")])
    ipm = Folder("Top of Personal Folders", None, children=[inbox, calendar, tail])
    build(ndb, {"ipm": ipm, "calendar": calendar}, [ipm], named=[(guid, lid, NAMED_ID[(guid, lid)]) for guid, lid in ITEM_NAMES])


def header_only(version=23, client=b"SM", crypt=1, eof_extra=0, break_crc=False):
    header = bytearray(564)
    header[0:4] = b"!BDN"
    header[8:10] = client
    struct.pack_into("<HHBB", header, 10, version, 19, 1, 1)
    struct.pack_into("<Q", header, 184, 564 + eof_extra)
    header[512] = 0x80
    header[513] = crypt
    struct.pack_into("<I", header, 4, crc(bytes(header[8:8 + 471])))
    struct.pack_into("<I", header, 524, crc(bytes(header[8:8 + 516])))
    if break_crc:
        header[524] ^= 0xFF
    return bytes(header)


def fixtures():
    files = {}

    ndb = Ndb()
    standard(ndb)
    files["standard.pst"] = ndb.write(1)

    for crypt, name in ((0, "mini-none.pst"), (1, "mini-permute.pst"), (2, "mini-cyclic.pst")):
        ndb = Ndb()
        mini(ndb)
        files[name] = ndb.write(crypt)

    ndb = Ndb()
    mini(ndb, [(0x67FF, PT_LONG, crc(b"fixture-password"))])
    files["protected.pst"] = ndb.write(1)

    ndb = Ndb()
    inbox_roles(ndb)
    files["inbox-roles.pst"] = ndb.write(1)

    ndb = Ndb()
    many(ndb)
    files["many.pst"] = ndb.write(1)

    ndb = Ndb()
    mini(ndb)
    files["corrupt-loop.pst"] = ndb.write(1, loop=True)

    # The second message's property block: the first block of its node.
    ndb = Ndb()
    inbox = mini(ndb)
    messages = sorted(nid for nid in ndb.nodes if nid & 0x1F == NID_TYPE_NORMAL_MESSAGE and ndb.nodes[nid][2] == inbox.nid)
    files["corrupt-block.pst"] = ndb.write(1, break_block=ndb.nodes[messages[1]][0])

    # An ANSI file, and OSTs: one that is a PST but for its magic, which is read,
    # and one whose store names its folders as an Exchange mailbox's, which is
    # refused. And every contact, calendar, task, note and journal shape.
    ndb = AnsiNdb()
    ansi(ndb)
    files["ansi.pst"] = ndb.write(1)

    ndb = Ndb()
    mini(ndb)
    files["offline.ost"] = ndb.write(1, client=b"SO")

    ndb = Ndb()
    mini(ndb, exchange_ids=True)
    files["offline-exchange.ost"] = ndb.write(1, client=b"SO")

    ndb = Ndb()
    items(ndb)
    files["items.pst"] = ndb.write(1)

    ndb = Ndb()
    clash(ndb)
    files["clash.pst"] = ndb.write(1)

    files["refuse-ost.pst"] = header_only(client=b"SO", version=36)
    files["refuse-ansi.pst"] = ansi_header_only(crypt=7)
    files["refuse-4k.pst"] = header_only(version=36)
    files["refuse-version.pst"] = header_only(version=40)
    files["refuse-wip.pst"] = header_only(crypt=0x10)
    files["refuse-wip37.pst"] = header_only(version=37, crypt=0x10)
    files["refuse-eof.pst"] = header_only(eof_extra=4096)
    files["refuse-crc.pst"] = header_only(break_crc=True)
    return files


def main():
    check = "--check" in sys.argv[1:]
    files = fixtures()
    if check:
        stale = []
        for name, data in sorted(files.items()):
            path = os.path.join(OUT, name)
            if not os.path.exists(path) or open(path, "rb").read() != data:
                stale.append(name)
        present = set(n for n in os.listdir(OUT) if n.endswith((".pst", ".ost"))) if os.path.isdir(OUT) else set()
        extra = sorted(present - set(files))
        if stale or extra:
            print("The PST fixtures are not what build/generate-pst-fixtures.py writes: %s%s" %
                  (", ".join(stale), (" (and not written by it: " + ", ".join(extra) + ")") if extra else ""))
            return 1
        print("The %d PST fixtures are what build/generate-pst-fixtures.py writes." % len(files))
        return 0
    os.makedirs(OUT, exist_ok=True)
    for name, data in sorted(files.items()):
        with open(os.path.join(OUT, name), "wb") as handle:
            handle.write(data)
        print("%-22s %7d bytes" % (name, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
