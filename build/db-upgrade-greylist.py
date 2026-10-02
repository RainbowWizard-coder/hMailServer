#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Greylisting triplets for build/db-upgrade-gate.sh: the rows a database from
before schema 6072 holds, and what schema 6107 must have made of them.

   db-upgrade-greylist.py seed          SQL to add the rows to a 6029 database
   db-upgrade-greylist.py check         reads the table back (one row a line,
                                        '|'-separated, as the gate's query
                                        prints it) and says what is wrong
   db-upgrade-greylist.py constraints   the statements the upgraded table must
                                        take ("accept") and refuse ("refuse")

The upgrade keys every row from before 6072 in the server (PreReqGreyListKeys),
because no database can compute the key: the MD5 of the address with ASCII
whitespace trimmed from both ends and the domain - after the last @ - folded A
to Z, as UTF-8, in lower-case hex. It is computed here, independently, from
docs/Greylisting.md, so that the gate holds the server's backfill to the
documented function and not to itself. A row with an address of 200 characters
- the width the columns had, and the cut every address was written with - is
deleted, on either side; of a triplet's rows the one that passed most, and of
those the oldest, is kept.
"""

import hashlib
import sys

CUT = "s" * (200 - len("@example.test")) + "@example.test"

# (glipaddress1, glipaddress2 or None, sender, recipient, passed, fate)
# The fate is what the upgrade must do: "keep", or why the row goes.
ROWS = [
    (3405803777, None, " Alice@Example.COM", "bob@example.test", 0, "keep"),
    (3405803777, None, CUT, "bob@example.test", 4, "cut"),
    (3405803777, None, "bob@example.test", CUT, 0, "cut"),
    # One triplet twice once the domain is folded: the confirmed row is kept
    # although the pending one is older.
    (3405803777, None, "carol@EXAMPLE.org", "dave@example.test", 0, "copy"),
    (3405803777, None, "carol@example.org", "dave@example.test", 3, "keep"),
    # The same addresses from an IPv6 address, and from the IPv4 address equal
    # to its first half: three triplets, glipaddress2 NULL being nothing else.
    (2306139568115548160, 5, "erin@example.org", "frank@example.test", 1, "keep"),
    (2306139568115548160, 6, "erin@example.org", "frank@example.test", 0, "keep"),
    (2306139568115548160, None, "erin@example.org", "frank@example.test", 0, "keep"),
]


def key(address):
    trimmed = address.strip(" \t\r\n")
    at = trimmed.rfind("@")
    if at >= 0:
        domain = "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in trimmed[at + 1:])
        trimmed = trimmed[:at + 1] + domain
    return hashlib.md5(trimmed.encode("utf-8")).hexdigest()


def literal(value):
    if value is None:
        return "null"
    if isinstance(value, int):
        return str(value)
    return "'" + value.replace("'", "''") + "'"


def insert(ip1, ip2, sender, recipient, passed, sender_key=None, recipient_key=None, keyed=False):
    columns = "glcreatetime, glblockendtime, gldeletetime, glipaddress1, glipaddress2, glsenderaddress, glrecipientaddress, glblockedcount, glpassedcount"
    values = "'2026-09-01 10:00:00', '2026-09-01 10:30:00', '2099-01-01 00:00:00', %s, %s, %s, %s, 0, %d" % (
        literal(ip1), literal(ip2), literal(sender), literal(recipient), passed)
    if keyed:
        columns += ", glsenderhash, glrecipienthash"
        values += ", %s, %s" % (literal(sender_key), literal(recipient_key))
    return "insert into hm_greylisting_triplets (%s) values (%s);" % (columns, values)


def seed():
    for ip1, ip2, sender, recipient, passed, _ in ROWS:
        print(insert(ip1, ip2, sender, recipient, passed))


def check():
    expected = [(ip1, ip2, s, r, p) for ip1, ip2, s, r, p, fate in ROWS if fate == "keep"]
    got = []
    for line in sys.stdin.read().splitlines():
        if not line.strip():
            continue
        fields = line.split("|")
        if len(fields) != 7:
            print("a row of hm_greylisting_triplets read back as %r" % line)
            return 1
        sender, recipient, ip1, ip2, passed, sender_key, recipient_key = fields
        got.append((int(ip1), None if ip2 == "-1" else int(ip2), sender, recipient, int(passed), sender_key, recipient_key))

    problems = []
    if len(got) != len(expected):
        problems.append("%d rows after the upgrade, not %d (the 200-character cuts and the second row of a triplet deleted, the rest kept)" % (len(got), len(expected)))
    for row in got:
        ip1, ip2, sender, recipient, passed, sender_key, recipient_key = row
        if (ip1, ip2, sender, recipient, passed) not in expected:
            problems.append("a row that should have gone: %r" % (row,))
        if sender_key != key(sender) or recipient_key != key(recipient):
            problems.append("keys %s/%s for %r/%r, not the documented %s/%s" % (sender_key, recipient_key, sender, recipient, key(sender), key(recipient)))
    for row in expected:
        if row not in [g[:5] for g in got]:
            problems.append("a row that should have been kept is gone: %r" % (row,))

    for problem in problems:
        print(problem)
    if problems:
        return 1
    print("   %d greylisting triplets from before 6072: keyed with the documented function, the 200-character cuts and the second row of a triplet deleted" % len(ROWS))
    return 0


def constraints():
    ip1 = 3405803777
    sender, recipient = " Alice@Example.COM", "bob@example.test"
    print("accept\t" + insert(ip1, None, sender, "other@example.test", 0, key(sender), key("other@example.test"), True))
    print("refuse\t" + insert(ip1, None, sender, recipient, 0, key(sender), key(recipient), True))
    print("refuse\t" + insert(2306139568115548160, 5, "erin@example.org", "frank@example.test", 0, key("erin@example.org"), key("frank@example.test"), True))
    print("refuse\t" + insert(ip1, None, "third@example.org", recipient, 0, None, key(recipient), True))
    return 0


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode == "seed":
        seed()
        sys.exit(0)
    if mode == "check":
        sys.exit(check())
    if mode == "constraints":
        sys.exit(constraints())
    print("usage: db-upgrade-greylist.py seed|check|constraints", file=sys.stderr)
    sys.exit(64)
