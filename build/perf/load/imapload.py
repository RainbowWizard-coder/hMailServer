# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""IMAP sessions: many clients logged in at once, each doing what a mail
client does between the moments a person looks at it.

A session connects, LOGINs, SELECTs INBOX, and then loops: an operation drawn
from --ops by weight, a think time, another. The operations:

  noop          NOOP                                   what a client polls with
  fetch-flags   FETCH of the last 50 messages' UID and FLAGS   a client's resync
  fetch-header  FETCH BODY.PEEK[HEADER] of one message a list row
  fetch-body    FETCH BODY.PEEK[] of one message      a message opened
  idle          IDLE for --idle-seconds, then DONE    a client waiting for mail;
                the latency recorded is from DONE to the tagged OK

A mailbox with no messages turns every fetch into a NOOP, so seed the
mailboxes first (build/perf/seed-folder.py, or smtp-in).

The sessions live in worker processes, which the parent steers over a pipe:
open sessions up to a count, report what was recorded between two times, stop.
"""

import asyncio
import random
import re
import ssl
import time

LITERAL = re.compile(rb"\{(\d+)\+?\}\r\n$")
EXISTS = re.compile(rb"^\* (\d+) EXISTS", re.I)

OPERATIONS = ("noop", "fetch-flags", "fetch-header", "fetch-body", "idle")


def parse_ops(text):
    weights = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        name, _, weight = part.partition(":")
        name = name.strip()
        if name not in OPERATIONS:
            raise ValueError("no operation %r (%s)" % (name, ", ".join(OPERATIONS)))
        value = float(weight) if weight.strip() else 1.0
        if value > 0:
            weights.append((name, value))
    if not weights:
        raise ValueError("--ops names no operation with a weight above 0")
    return weights


def quote(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Session(object):

    def __init__(self, cfg, number, records, errors):
        self.cfg = cfg
        self.number = number
        self.records = records
        self.errors = errors
        self.reader = None
        self.writer = None
        self.exists = 0
        self.tag_number = 0
        self.random = random.Random(cfg["seed"] * 7919 + number)
        self.weights = parse_ops(cfg["ops"])
        self.total_weight = sum(w for _, w in self.weights)
        self.alive = False

    def user(self):
        return self.cfg["user_template"].format(n=(self.number % max(1, self.cfg["users"])) + 1)

    def error(self, what):
        self.errors[what] = self.errors.get(what, 0) + 1

    async def response(self, tag):
        """Reads to the tagged reply, skipping literals, noting EXISTS. The
        reply's status word: OK, NO or BAD."""
        while True:
            line = await self.reader.readline()
            if not line:
                raise ConnectionError("the server closed the connection")
            match = LITERAL.search(line)
            while match:
                await self.reader.readexactly(int(match.group(1)))
                line = await self.reader.readline()
                if not line:
                    raise ConnectionError("the server closed the connection inside a literal")
                match = LITERAL.search(line)
            if line.startswith(tag + b" "):
                words = line[len(tag) + 1:].split(None, 1)
                return words[0].upper() if words else b""
            match = EXISTS.match(line)
            if match:
                self.exists = int(match.group(1))

    def next_tag(self):
        self.tag_number += 1
        return b"a%d" % self.tag_number

    async def command(self, text):
        tag = self.next_tag()
        self.writer.write(tag + b" " + text + b"\r\n")
        return await asyncio.wait_for(self.response(tag), self.cfg["timeout"])

    async def open(self):
        cfg = self.cfg
        started = time.perf_counter()
        context = None
        if cfg["tls"] != "none":
            context = ssl.create_default_context()
            if cfg["insecure"]:
                context.check_hostname = False
                context.verify_mode = ssl.CERT_NONE
        if cfg["tls"] == "implicit":
            self.reader, self.writer = await asyncio.wait_for(
                asyncio.open_connection(cfg["host"], cfg["port"], ssl=context, server_hostname=cfg["host"]), cfg["timeout"])
        else:
            self.reader, self.writer = await asyncio.wait_for(asyncio.open_connection(cfg["host"], cfg["port"]), cfg["timeout"])
        greeting = await asyncio.wait_for(self.reader.readline(), cfg["timeout"])
        if not greeting.startswith(b"* OK"):
            raise RuntimeError("greeting %r" % greeting[:40])
        if cfg["tls"] == "starttls":
            if await self.command(b"STARTTLS") != b"OK":
                raise RuntimeError("STARTTLS refused")
            await self.writer.start_tls(context, server_hostname=cfg["host"])
        status = await self.command(("LOGIN %s %s" % (quote(self.user()), quote(cfg["password"]))).encode("utf-8"))
        if status != b"OK":
            raise RuntimeError("LOGIN %s" % status.decode("ascii", "replace"))
        status = await self.command(b"SELECT INBOX")
        if status != b"OK":
            raise RuntimeError("SELECT %s" % status.decode("ascii", "replace"))
        self.alive = True
        return (time.perf_counter() - started) * 1000.0

    def choose(self):
        pick = self.random.random() * self.total_weight
        for name, weight in self.weights:
            pick -= weight
            if pick <= 0:
                return name
        return self.weights[-1][0]

    async def operate(self, name):
        if name.startswith("fetch") and self.exists == 0:
            name = "noop"
        started = time.perf_counter()
        if name == "noop":
            status = await self.command(b"NOOP")
        elif name == "fetch-flags":
            status = await self.command(b"FETCH %d:%d (UID FLAGS)" % (max(1, self.exists - 49), self.exists))
        elif name == "fetch-header":
            status = await self.command(b"FETCH %d (BODY.PEEK[HEADER])" % self.random.randint(1, self.exists))
        elif name == "fetch-body":
            status = await self.command(b"FETCH %d (BODY.PEEK[])" % self.random.randint(1, self.exists))
        else:
            tag = self.next_tag()
            self.writer.write(tag + b" IDLE\r\n")
            line = await asyncio.wait_for(self.reader.readline(), self.cfg["timeout"])
            if not line.startswith(b"+"):
                raise RuntimeError("IDLE not accepted")
            await asyncio.sleep(self.cfg["idle_seconds"])
            started = time.perf_counter()
            self.writer.write(b"DONE\r\n")
            status = await asyncio.wait_for(self.response(tag), self.cfg["timeout"])
        elapsed = (time.perf_counter() - started) * 1000.0
        self.records.append((time.time(), name, elapsed, status == b"OK"))
        if status != b"OK":
            self.error("%s answered %s" % (name, status.decode("ascii", "replace")))

    async def loop(self):
        think = self.cfg["think_ms"] / 1000.0
        try:
            # The first operation of each session starts at a random point of one
            # think time, so that a step's sessions do not all ask at once.
            await asyncio.sleep(self.random.random() * think)
            while True:
                await self.operate(self.choose())
                await asyncio.sleep(think * (0.5 + self.random.random()))
        except asyncio.CancelledError:
            raise
        except Exception as error:
            self.alive = False
            self.error("session lost: " + type(error).__name__)

    async def close(self):
        if self.writer is None:
            return
        try:
            if self.alive:
                self.writer.write(b"z LOGOUT\r\n")
                await asyncio.wait_for(self.writer.drain(), 5)
        except Exception:
            pass
        self.writer.close()


def imap_worker(cfg, worker, workers, connection):
    """One worker process's sessions, steered by the parent:
       ("target", n)         open sessions until this worker has n; answers
                             ("opened", alive, failures, login_ms[])
       ("collect", t0, t1)   answers ("records", records in [t0, t1], errors,
                             alive, cpu seconds so far)
       ("stop",)             LOGOUT every session; answers ("stopped",)"""
    asyncio.run(_worker_main(cfg, worker, workers, connection))


async def _worker_main(cfg, worker, workers, connection):
    loop = asyncio.get_running_loop()
    records = []
    errors = {}
    sessions = []
    tasks = []
    opened = 0
    cpu_started = time.process_time()
    rate = max(1.0, cfg["connect_rate"] / float(workers))
    while True:
        message = await loop.run_in_executor(None, connection.recv)
        if message[0] == "target":
            want = message[1]
            logins = []
            failures = 0
            pending = []

            async def one(number):
                session = Session(cfg, number, records, errors)
                try:
                    logins.append(await session.open())
                    sessions.append(session)
                    tasks.append(asyncio.ensure_future(session.loop()))
                    return True
                except Exception as error:
                    detail = str(error)
                    key = "open: " + type(error).__name__ + ((" " + detail) if detail and len(detail) < 40 else "")
                    errors[key] = errors.get(key, 0) + 1
                    await session.close()
                    return False
            while opened < want:
                pending.append(asyncio.ensure_future(one(worker + opened * workers)))
                opened += 1
                await asyncio.sleep(1.0 / rate)
            if pending:
                results = await asyncio.gather(*pending)
                failures = len([r for r in results if not r])
            connection.send(("opened", len([s for s in sessions if s.alive]), failures, logins))
        elif message[0] == "collect":
            start, end = message[1], message[2]
            inside = [r for r in records if start <= r[0] <= end]
            records[:] = [r for r in records if r[0] > end]
            snapshot = dict(errors)
            errors.clear()
            connection.send(("records", inside, snapshot, len([s for s in sessions if s.alive]),
                             time.process_time() - cpu_started))
        else:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await asyncio.gather(*[s.close() for s in sessions], return_exceptions=True)
            connection.send(("stopped",))
            return
