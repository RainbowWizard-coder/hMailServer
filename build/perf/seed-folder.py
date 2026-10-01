#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Fill one folder of one account with N messages, the same N every time.

The Roadmap2 row "Performance at a hundred thousand messages" is measured on a
folder of that size (hmailserver/docs/Performance.md); this makes the folder.
It runs wherever Python 3.9 does - the Windows bench and a Linux server alike -
and needs nothing that is not in the standard library.

HOW IT GETS THE MESSAGES IN: IMAP APPEND, through the server's own front door.
An APPEND is how any client files a message, so what this makes is exactly
what a real mailbox is: each file placed through the message store (on disk,
encrypted at rest, or in an object store - whichever the server is set to),
each row given the folder's next UID and a mod-sequence, the quota counted, the
folder's IMAP sessions told, the full-text indexer shown the message. Writing
rows into hm_messages directly would be faster and would measure a mailbox no
server ever makes: no files, or files the store does not know, UIDs and
mod-sequences made up, one SQL dialect per backend, and the indexer never told.
The Maildir import is the Import Tool - a Windows program with a window - and
cannot be run from a script on either system. So: APPEND, several messages to
one command (RFC 3502 MULTIAPPEND, which the server offers), and the small ones
as non-synchronising literals (RFC 7888; the server advertises LITERAL-, which
allows {n+} up to 4096 bytes - larger literals wait for the server's "+", one
round trip on the loopback). Several connections at once (--connections) are
faster still; the UIDs then no longer follow the dates exactly.

WHAT IS MADE, deterministic from --seed: message i is the same message on every
run, whatever the batching, the connections or where a resumed run starts
(--start). Senders from a fixed pool shaped like an inbox - people, newsletters
with List-Unsubscribe, the social networks' notification addresses, machine
senders with Auto-Submitted, mailing lists with List-Post - so every inbox tab
has mail. Dates spread evenly over --years up to --end-date, oldest first, so
that UID order is date order. About a fifth are replies, with In-Reply-To and
References naming an earlier message, so the conversations view has threads.
Plain text, and for newsletters and some people multipart/alternative with an
HTML part; --attachments of them carry one attachment of 20 KB to 512 KB.
Older messages are mostly read, the newest mostly not, and a few are flagged.

TWO WORDS TO SEARCH FOR: the word NEEDLE ("quokka") is in the body - never the
subject - of every 1000th message (i % 1000 == 7), so a search for it has N/1000
hits and has to read bodies or ask the index; and "invoice" is in the subject
of every 100th (i % 100 == 3). The summary this prints says how many of each.

THE ACCOUNT needs room: at the defaults a hundred thousand messages are 1.3 GB
(1,322,787,827 bytes at seed 49 for self@uicheck.test, the account
Performance.md fills; the address is in every message's To:, so another
changes the count, and --dry-run --user says for any). That is what is sent;
what the server stores is read from it afterwards. Give it no quota (maximum
size 0) or enough, and a maximum message size of at least 1 MB, or the server
refuses the APPENDs it is right to refuse.

THE PASSWORD comes from the environment, HMPERF_PASSWORD, as the load
generator's do (build/perf/load/loadgen.py): a process's arguments are in every
user's process list. --password still takes one, for a throwaway account.

Usage:
  HMPERF_PASSWORD=SECRET python build/perf/seed-folder.py --user perf@example.test \\
      [--host 127.0.0.1] [--port 143] [--tls | --starttls] [--insecure] \\
      [--folder INBOX] [--count 100000] [--seed 49] [--start 0] \\
      [--batch 50] [--connections 1] [--attachments 0.05] \\
      [--end-date 2026-09-01] [--years 5] [--dry-run]

--dry-run makes the messages without connecting and reports their count and
size - a way to see what a run would upload. The summary is one JSON object on
standard output; progress goes to standard error.
"""

import argparse
import datetime
import json
import os
import random
import socket
import ssl
import sys
import threading
import time

NEEDLE = "quokka"
SUBJECT_NEEDLE = "invoice"
DOMAIN = "perf.hmailserver.invalid"
LITERAL_MINUS_LIMIT = 4096

WORDS = (
    "the of and to in is that for it as was with be by on not he this are or his from at which but have an they "
    "you were her she there been one all we their has would when if so what up out about more can said do into "
    "time only some could them than other then its now two may first very people also any new work because "
    "these after way well made over did many where must through back years before much good should our those "
    "each just most under three same such here day between both last never life being still world while "
    "house might under again place great every found another since against right under around small number "
    "project meeting budget schedule report review draft agenda quarter figures plan update team client "
    "contract delivery order account payment deadline proposal summary notes minutes forecast launch release "
    "garden weekend holiday train ticket hotel dinner lunch coffee photos birthday concert football weather "
    "school homework recipe kitchen bicycle museum library journey river mountain harbour village market"
).split()

FIRST_NAMES = (
    "Ada Alan Alice Amir Anna Ben Bea Carla Chen Dara David Elena Emeka Eva Farah Felix Grace Hana Hugo Ines "
    "Ivan Jana Jonas Kai Karin Lars Lea Liam Lina Luca Maya Mei Nadia Noor Olga Omar Paula Pedro Priya Rafael "
    "Rosa Sam Sara Selin Tariq Tom Uma Vera Wei Yara Yusuf Zoe"
).split()

LAST_NAMES = (
    "Abbott Baker Becker Costa Dubois Ekström Fischer Garcia Haddad Ivanova Jensen Kaur Kowalski Larsen Lopez "
    "Martin Meyer Moreau Nakamura Nguyen Novak Okafor Olsen Park Petrov Quinn Rossi Santos Schmidt Silva "
    "Tanaka Thomas Varga Wagner Wang Weber Yilmaz Young Zhang"
).split()

PEOPLE_DOMAINS = ["example.com", "example.org", "example.net", "mail.example", "post.example", "company.example"]
BRANDS = ["Northwind", "Contoso", "Fabrikam", "Tailspin", "Wingtip", "Adventure Works", "Litware", "Proseware"]
SOCIAL = [("LinkedIn", "linkedin.com"), ("Facebook", "facebookmail.com"), ("Reddit", "redditmail.com"),
          ("Strava", "strava.com"), ("Meetup", "meetup.com")]
MACHINES = ["noreply", "notifications", "alerts", "receipts", "billing", "no-reply"]
LISTS = ["dev", "users", "announce", "garden-club", "book-circle"]
MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
DAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]


def rng_for(seed, index, part):
    # A string seed is hashed with SHA-512 by random.Random (version 2), so the
    # stream is the same on every run and every platform.
    return random.Random("%s:%d:%s" % (seed, index, part))


def words(rng, count):
    return " ".join(rng.choice(WORDS) for _ in range(count))


def base_subject(seed, index):
    rng = rng_for(seed, index, "subject")
    return words(rng, rng.randint(3, 8)).capitalize()


def subject_of(seed, index):
    # The subject needle is on exactly every 100th message: those are never
    # replies, and a reply quotes its parent's words without the needle.
    if index % 100 == 3:
        return "Invoice " + str(10000 + index) + " - " + base_subject(seed, index)
    return base_subject(seed, index)


def sender_of(seed, index):
    """(display, address, kind): who message i is from."""
    rng = rng_for(seed, index, "sender")
    roll = rng.random()
    if roll < 0.60:
        first = rng.choice(FIRST_NAMES)
        last = rng.choice(LAST_NAMES)
        local = (first + "." + last).lower().replace("ö", "o").replace("ë", "e")
        return first + " " + last, local + "@" + rng.choice(PEOPLE_DOMAINS), "person"
    if roll < 0.75:
        brand = rng.choice(BRANDS)
        return brand + " News", "news@" + brand.lower().replace(" ", "") + ".example", "newsletter"
    if roll < 0.83:
        name, domain = rng.choice(SOCIAL)
        return name, "notifications@" + domain, "social"
    if roll < 0.93:
        brand = rng.choice(BRANDS)
        return brand, rng.choice(MACHINES) + "@" + brand.lower().replace(" ", "") + ".example", "machine"
    lst = rng.choice(LISTS)
    return lst + " list", lst + "@lists.example", "list"


def date_of(seed, index, count, end, years):
    """Evenly spread from end - years to end, oldest first, a little jitter."""
    span = years * 365.25 * 86400.0
    start = end - datetime.timedelta(seconds=span)
    step = span / max(count, 1)
    jitter = rng_for(seed, index, "date").random() * step * 0.8
    return start + datetime.timedelta(seconds=index * step + jitter)


def display_name(name):
    """A display name as a From header carries it: quoted when it is ASCII,
    an RFC 2047 encoded word when it is not."""
    import base64
    if all(ord(c) < 128 for c in name):
        return '"' + name.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return "=?utf-8?b?" + base64.b64encode(name.encode("utf-8")).decode("ascii") + "?="


def rfc5322_date(moment):
    return "%s, %02d %s %04d %02d:%02d:%02d +0000" % (
        DAYS[moment.weekday()], moment.day, MONTHS[moment.month - 1], moment.year, moment.hour, moment.minute, moment.second)


def imap_date(moment):
    return "%02d-%s-%04d %02d:%02d:%02d +0000" % (
        moment.day, MONTHS[moment.month - 1], moment.year, moment.hour, moment.minute, moment.second)


def message_id(seed, index):
    return "<perf-%s-%d@%s>" % (seed, index, DOMAIN)


def body_text(rng, index):
    paragraphs = []
    count = 1 + min(int(rng.expovariate(1 / 3.0)), 40)
    for p in range(count):
        line = words(rng, rng.randint(40, 120)).capitalize() + "."
        if p == 0 and index % 1000 == 7:
            line = line[:-1] + " and the " + NEEDLE + " came too."
        paragraphs.append(line)
    return paragraphs


def wrap(text, width=76):
    out = []
    for paragraph in text:
        line = ""
        for word in paragraph.split(" "):
            if line and len(line) + 1 + len(word) > width:
                out.append(line)
                line = word
            else:
                line = word if not line else line + " " + word
        if line:
            out.append(line)
        out.append("")
    return "\r\n".join(out)


def base64_lines(data):
    import base64
    encoded = base64.b64encode(data).decode("ascii")
    return "\r\n".join(encoded[i:i + 76] for i in range(0, len(encoded), 76))


def make_message(seed, index, count, end, years, attachment_rate, account):
    """(flags, internal date, bytes) for message i."""
    rng = rng_for(seed, index, "body")
    display, address, kind = sender_of(seed, index)
    moment = date_of(seed, index, count, end, years)
    subject = subject_of(seed, index)

    headers = [
        "Return-Path: <%s>" % address,
        "From: %s <%s>" % (display_name(display), address),
        "To: <%s>" % account,
        "Date: %s" % rfc5322_date(moment),
        "Message-ID: %s" % message_id(seed, index),
        "MIME-Version: 1.0",
    ]

    # A third of the people's mail - a fifth of all - answers an earlier
    # message, so the list has conversations.
    if index > 0 and kind == "person" and index % 100 != 3 and rng.random() < 0.33:
        parent = max(0, index - rng.randint(1, 50))
        subject = "Re: " + base_subject(seed, parent)
        headers.append("In-Reply-To: %s" % message_id(seed, parent))
        headers.append("References: %s" % message_id(seed, parent))

    headers.insert(3, "Subject: %s" % subject)

    if kind == "newsletter":
        headers.append("List-Unsubscribe: <mailto:unsubscribe@%s>" % address.split("@")[1])
        headers.append("Precedence: bulk")
    elif kind == "machine":
        headers.append("Auto-Submitted: auto-generated")
    elif kind == "list":
        headers.append("List-Post: <mailto:%s>" % address)
        headers.append("List-Id: <%s>" % address.replace("@", "."))
        headers.append("Precedence: list")

    text = wrap(body_text(rng, index))
    html = kind == "newsletter" or (kind == "person" and rng.random() < 0.3)
    attach = rng.random() < attachment_rate

    boundary_alt = "alt-%s-%d" % (seed, index)
    boundary_mix = "mix-%s-%d" % (seed, index)

    if html:
        paragraphs = ["<p>%s</p>" % p for p in text.split("\r\n\r\n") if p]
        html_part = "<html><body>%s</body></html>" % "\r\n".join(paragraphs)
        alternative = (
            "Content-Type: multipart/alternative; boundary=\"%s\"\r\n\r\n"
            "--%s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n%s\r\n"
            "--%s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n%s\r\n"
            "--%s--\r\n") % (boundary_alt, boundary_alt, text, boundary_alt, html_part, boundary_alt)
    else:
        alternative = "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n%s\r\n" % text

    if attach:
        # 20 KB to 512 KB, even in the logarithm, of bytes from the same stream.
        size = int(20 * 1024 * (512 / 20.0) ** rng.random())
        data = rng.getrandbits(size * 8).to_bytes(size, "little")
        name = "%s-%d.pdf" % (rng.choice(WORDS), index)
        content = (
            "Content-Type: multipart/mixed; boundary=\"%s\"\r\n\r\n"
            "--%s\r\n%s"
            "--%s\r\nContent-Type: application/pdf; name=\"%s\"\r\nContent-Disposition: attachment; filename=\"%s\"\r\n"
            "Content-Transfer-Encoding: base64\r\n\r\n%s\r\n"
            "--%s--\r\n") % (boundary_mix, boundary_mix, alternative, boundary_mix, name, name, base64_lines(data), boundary_mix)
    else:
        content = alternative

    raw = ("\r\n".join(headers) + "\r\n" + content).encode("utf-8")

    # Older mail is mostly read, the newest mostly not; a few are flagged.
    age = 1.0 - index / float(max(count, 1))
    flags = []
    if rng.random() < (0.97 if age > 0.02 else 0.3):
        flags.append("\\Seen")
    if rng.random() < 0.02:
        flags.append("\\Flagged")

    return " ".join(flags), imap_date(moment), raw


def quote(text):
    if any(ord(c) > 126 or ord(c) < 32 for c in text):
        raise SystemExit("the folder name must be ASCII here (a modified UTF-7 name is not built by this tool): %r" % text)
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


class ImapError(Exception):
    pass


class Imap(object):
    """Just enough IMAP4rev1 to log in and APPEND: a line reader over a socket."""

    def __init__(self, host, port, tls, starttls, insecure, timeout):
        self.tag_number = 0
        raw = socket.create_connection((host, port), timeout=timeout)
        self.context = ssl.create_default_context()
        if insecure:
            self.context.check_hostname = False
            self.context.verify_mode = ssl.CERT_NONE
        self.sock = self.context.wrap_socket(raw, server_hostname=host) if tls else raw
        self.buffer = b""
        greeting = self.readline()
        if not greeting.startswith(b"* OK"):
            raise ImapError("the server did not greet: %r" % greeting)
        self.capabilities = self.capability()
        if starttls:
            if "STARTTLS" not in self.capabilities:
                raise ImapError("the server does not offer STARTTLS")
            self.command("STARTTLS")
            self.sock = self.context.wrap_socket(self.sock, server_hostname=host)
            self.buffer = b""
            self.capabilities = self.capability()

    def readline(self):
        while b"\r\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ImapError("the server closed the connection")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        return line

    def next_tag(self):
        self.tag_number += 1
        return "s%05d" % self.tag_number

    def tagged(self, tag):
        """The untagged lines, then the tagged one - which must be OK."""
        lines = []
        while True:
            line = self.readline()
            if line.startswith(tag.encode("ascii") + b" "):
                status = line[len(tag) + 1:]
                if not status.startswith(b"OK"):
                    raise ImapError(status.decode("utf-8", "replace"))
                return lines, status
            lines.append(line)

    def command(self, text):
        tag = self.next_tag()
        self.sock.sendall((tag + " " + text + "\r\n").encode("utf-8"))
        return self.tagged(tag)

    def capability(self):
        lines, _ = self.command("CAPABILITY")
        caps = set()
        for line in lines:
            if line.upper().startswith(b"* CAPABILITY"):
                caps.update(line.decode("ascii", "replace").upper().split()[2:])
        return caps

    def login(self, user, password):
        self.command("LOGIN %s %s" % (quote(user), quote(password)))
        # The capabilities after authentication are the ones that count.
        self.capabilities = self.capability()

    def ensure_folder(self, folder):
        if folder.upper() == "INBOX":
            return
        try:
            self.command("CREATE %s" % quote(folder))
        except ImapError as why:
            if "ALREADYEXISTS" not in str(why).upper() and "EXIST" not in str(why).upper():
                raise

    def append_many(self, folder, messages):
        """One APPEND of several messages (MULTIAPPEND), or one at a time
        where the server does not offer it. Literals of up to 4096 bytes go
        without waiting (LITERAL- / LITERAL+); larger ones wait for "+"."""
        nonsync = "LITERAL+" in self.capabilities or "LITERAL-" in self.capabilities
        unlimited = "LITERAL+" in self.capabilities
        groups = [messages] if "MULTIAPPEND" in self.capabilities else [[m] for m in messages]
        for group in groups:
            tag = self.next_tag()
            out = bytearray((tag + " APPEND " + quote(folder)).encode("utf-8"))
            for flags, when, data in group:
                if flags:
                    out += (" (" + flags + ")").encode("ascii")
                out += (' "' + when + '" ').encode("ascii")
                size = len(data)
                if nonsync and (unlimited or size <= LITERAL_MINUS_LIMIT):
                    out += ("{%d+}\r\n" % size).encode("ascii")
                    out += data
                else:
                    out += ("{%d}\r\n" % size).encode("ascii")
                    self.sock.sendall(bytes(out))
                    out = bytearray()
                    self.wait_for_continuation(tag)
                    out += data
            out += b"\r\n"
            self.sock.sendall(bytes(out))
            self.tagged(tag)

    def wait_for_continuation(self, tag):
        while True:
            line = self.readline()
            if line.startswith(b"+"):
                return
            if line.startswith(tag.encode("ascii") + b" "):
                raise ImapError(line[len(tag) + 1:].decode("utf-8", "replace"))

    def status(self, folder):
        lines, _ = self.command("STATUS %s (MESSAGES UIDNEXT)" % quote(folder))
        return b" ".join(lines).decode("utf-8", "replace")

    def logout(self):
        try:
            self.command("LOGOUT")
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


def parse_args(argv):
    parser = argparse.ArgumentParser(description="Fill one folder with N deterministic messages over IMAP APPEND.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=None, help="143, or 993 with --tls")
    parser.add_argument("--tls", action="store_true", help="implicit TLS (IMAPS)")
    parser.add_argument("--starttls", action="store_true")
    parser.add_argument("--insecure", action="store_true", help="do not verify the server's certificate (a bench's own)")
    parser.add_argument("--user", help="the account's address")
    parser.add_argument("--password", help="the account's password; better HMPERF_PASSWORD in the environment")
    parser.add_argument("--folder", default="INBOX")
    parser.add_argument("--count", type=int, default=100000)
    parser.add_argument("--seed", default="49")
    parser.add_argument("--start", type=int, default=0, help="the first message to make, to resume a run")
    parser.add_argument("--batch", type=int, default=50, help="messages per APPEND command")
    parser.add_argument("--connections", type=int, default=1)
    parser.add_argument("--attachments", type=float, default=0.05, help="the share of messages with an attachment")
    parser.add_argument("--end-date", default="2026-09-01", help="the newest message's day (UTC)")
    parser.add_argument("--years", type=float, default=5.0, help="how far back the oldest goes")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--dry-run", action="store_true", help="make the messages, connect to nothing")
    args = parser.parse_args(argv)
    if args.port is None:
        args.port = 993 if args.tls else 143
    if args.password is None and "HMPERF_PASSWORD" in os.environ:
        args.password = os.environ["HMPERF_PASSWORD"]
    if not args.dry_run and (not args.user or args.password is None):
        parser.error("--user and a password (HMPERF_PASSWORD in the environment, or --password) are needed unless --dry-run")
    if args.count < 1 or args.start < 0 or args.start >= args.count or args.batch < 1 or args.connections < 1:
        parser.error("--count at least 1, --start below it, --batch and --connections at least 1")
    return args


def main(argv):
    args = parse_args(argv)
    end = datetime.datetime.strptime(args.end_date, "%Y-%m-%d").replace(hour=12)
    account = args.user or "perf@" + DOMAIN

    batches = [(first, min(first + args.batch, args.count)) for first in range(args.start, args.count, args.batch)]
    lock = threading.Lock()
    state = {"next": 0, "messages": 0, "bytes": 0, "failed": None}
    started = time.time()

    def take():
        with lock:
            if state["failed"] or state["next"] >= len(batches):
                return None
            batch = batches[state["next"]]
            state["next"] += 1
            return batch

    def report(made, size):
        with lock:
            state["messages"] += made
            state["bytes"] += size
            done = state["messages"]
        if done % (args.batch * 20) < made or done == args.count - args.start:
            elapsed = time.time() - started
            sys.stderr.write("%d of %d, %.1f MB, %.0f a second\n" % (
                done, args.count - args.start, state["bytes"] / 1048576.0, done / elapsed if elapsed > 0 else 0))

    def worker():
        connection = None
        batch = (args.start, args.start)
        try:
            if not args.dry_run:
                connection = Imap(args.host, args.port, args.tls, args.starttls, args.insecure, args.timeout)
                connection.login(args.user, args.password)
                connection.ensure_folder(args.folder)
            while True:
                batch = take()
                if batch is None:
                    return
                messages = [make_message(args.seed, i, args.count, end, args.years, args.attachments, account) for i in range(batch[0], batch[1])]
                if connection:
                    connection.append_many(args.folder, messages)
                report(len(messages), sum(len(m[2]) for m in messages))
        except Exception as why:
            with lock:
                if not state["failed"]:
                    # A MULTIAPPEND is all or nothing, so on one connection
                    # --start with this number resumes without a gap or a
                    # duplicate; with several, later batches may be in already.
                    state["failed"] = "%s (the batch from message %d was not stored; on one connection --start %d resumes)" % (why, batch[0], batch[0])
        finally:
            if connection:
                connection.logout()

    threads = [threading.Thread(target=worker) for _ in range(args.connections)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    elapsed = time.time() - started
    summary = {
        "folder": args.folder,
        "account": args.user or "",
        "seed": args.seed,
        "first": args.start,
        "count": args.count,
        "made": state["messages"],
        "bytes": state["bytes"],
        "seconds": round(elapsed, 1),
        "per_second": round(state["messages"] / elapsed, 1) if elapsed > 0 else None,
        "connections": args.connections,
        "dry_run": args.dry_run,
        "body_needle": NEEDLE,
        "body_needle_hits": len([i for i in range(args.count) if i % 1000 == 7]),
        "subject_needle": SUBJECT_NEEDLE,
        "subject_needle_hits": len([i for i in range(args.count) if i % 100 == 3]),
    }
    if state["failed"]:
        summary["failed"] = state["failed"]
    elif not args.dry_run:
        try:
            connection = Imap(args.host, args.port, args.tls, args.starttls, args.insecure, args.timeout)
            connection.login(args.user, args.password)
            summary["status"] = connection.status(args.folder)
            connection.logout()
        except Exception as why:
            summary["status"] = "not read: %s" % why

    print(json.dumps(summary, indent=2))
    return 1 if state["failed"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
