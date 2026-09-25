# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""SMTP load: the clients that deliver (smtp-in, and the injection half of
smtp-out), the sink that takes what a server relays out, and the check that
what was accepted reached the mailboxes.

The clients are asyncio coroutines, spread over worker processes so that one
Python interpreter's lock is not the ceiling. Each client opens a connection,
says EHLO, optionally STARTTLS and AUTH PLAIN, then sends --per-connection
messages on it (pipelined when the server offers PIPELINING, as Postfix's own
client does) and QUITs, until the run's messages are gone. A message's latency
is from its MAIL FROM to the 250 after its final dot: what a sending MTA waits
for, and the moment the receiving server says it has taken responsibility.
"""

import asyncio
import base64
import imaplib
import os
import random
import re
import ssl
import time

SIZE_UNITS = {"": 1, "b": 1, "k": 1024, "kb": 1024, "m": 1048576, "mb": 1048576}


def parse_sizes(text):
    """'4k:50,32k:30,256k:15,1m:5' -> [(4096, 50), ...]: sizes and weights."""
    buckets = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        size_text, _, weight_text = part.partition(":")
        match = re.match(r"^\s*(\d+(?:\.\d+)?)\s*([a-zA-Z]*)\s*$", size_text)
        if not match or match.group(2).lower() not in SIZE_UNITS:
            raise ValueError("a size is a number with k or m after it: %r" % size_text)
        size = int(float(match.group(1)) * SIZE_UNITS[match.group(2).lower()])
        weight = float(weight_text) if weight_text.strip() else 1.0
        if size < 256 or weight <= 0:
            raise ValueError("each size must be at least 256 bytes and each weight above 0: %r" % part)
        buckets.append((size, weight))
    if not buckets:
        raise ValueError("no sizes in %r" % text)
    return buckets


class MessageFactory(object):
    """Messages of the size distribution, the same for the same seed: each
    bucket's body is made once (lines of 76 base64 characters, which neither
    server compresses away or treats as special) and each message gets its own
    headers, among them the run's token and the moment it was sent, which the
    sink reads."""

    def __init__(self, sizes, seed, run_id, mail_from, rcpt_template, mailboxes, rcpts_per_message=1):
        self.buckets = parse_sizes(sizes) if isinstance(sizes, str) else sizes
        self.seed = seed
        self.run_id = run_id
        self.mail_from = mail_from
        self.rcpt_template = rcpt_template
        self.mailboxes = max(1, mailboxes)
        self.rcpts_per_message = max(1, rcpts_per_message)
        self.total_weight = sum(w for _, w in self.buckets)
        rng = random.Random(seed)
        self.bodies = []
        for size, _ in self.buckets:
            line_count = max(1, (size - 600) // 78)
            lines = []
            for _ in range(line_count):
                lines.append(base64.b64encode(rng.getrandbits(456).to_bytes(57, "little")) + b"\r\n")
            self.bodies.append(b"".join(lines))

    def bucket_of(self, index):
        pick = random.Random(self.seed * 1000003 + index).random() * self.total_weight
        for position, (_, weight) in enumerate(self.buckets):
            pick -= weight
            if pick <= 0:
                return position
        return len(self.buckets) - 1

    def recipients(self, index):
        return [self.rcpt_template.format(n=((index * self.rcpts_per_message + k) % self.mailboxes) + 1)
                for k in range(self.rcpts_per_message)]

    def mailbox_numbers(self, count):
        numbers = set()
        for index in range(count):
            for k in range(self.rcpts_per_message):
                numbers.add(((index * self.rcpts_per_message + k) % self.mailboxes) + 1)
        return sorted(numbers)

    def build(self, index):
        bucket = self.bucket_of(index)
        rcpts = self.recipients(index)
        headers = ("From: <%s>\r\nTo: %s\r\nSubject: loadgen %s %d\r\nMessage-ID: <%s.%d@loadgen.invalid>\r\n"
                   "Date: %s\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=us-ascii\r\n"
                   "X-Loadgen-Run: %s\r\nX-Loadgen-Seq: %d\r\nX-Loadgen-Sent: %.6f\r\n\r\n") % (
            self.mail_from, ", ".join("<%s>" % r for r in rcpts), self.run_id, index, self.run_id, index,
            time.strftime("%a, %d %b %Y %H:%M:%S +0000", time.gmtime()), self.run_id, index, time.time())
        return headers.encode("ascii") + self.bodies[bucket] + b".\r\n", rcpts, self.buckets[bucket][0]


# ---------------------------------------------------------------- the client

async def read_reply(reader):
    lines = []
    while True:
        line = await reader.readline()
        if not line:
            raise ConnectionError("the server closed the connection")
        lines.append(line)
        if len(line) < 4 or line[3:4] != b"-":
            break
    head = lines[-1][:3]
    return (int(head) if head.isdigit() else 0), b"".join(lines)


def client_ssl_context(insecure):
    context = ssl.create_default_context()
    if insecure:
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
    return context


class SmtpWorkerStats(object):
    def __init__(self):
        self.latencies = []
        self.accepted = 0
        self.failed = 0
        self.bytes = 0
        self.errors = {}
        self.connections = 0
        self.first_send = None
        self.last_ack = None

    def error(self, what):
        self.errors[what] = self.errors.get(what, 0) + 1


async def _open(cfg):
    context = client_ssl_context(cfg["insecure"]) if cfg["tls"] != "none" else None
    if cfg["tls"] == "implicit":
        return await asyncio.open_connection(cfg["host"], cfg["port"], ssl=context, server_hostname=cfg["host"])
    return await asyncio.open_connection(cfg["host"], cfg["port"])


async def smtp_client(cfg, factory, take, stats):
    consecutive_failures = 0
    while True:
        index = take()
        if index is None:
            return
        writer = None
        try:
            reader, writer = await asyncio.wait_for(_open(cfg), cfg["timeout"])
            stats.connections += 1
            code, _ = await asyncio.wait_for(read_reply(reader), cfg["timeout"])
            if code != 220:
                raise RuntimeError("greeting %d" % code)
            writer.write(b"EHLO loadgen.invalid\r\n")
            code, text = await asyncio.wait_for(read_reply(reader), cfg["timeout"])
            if code != 250:
                raise RuntimeError("EHLO %d" % code)
            if cfg["tls"] == "starttls":
                writer.write(b"STARTTLS\r\n")
                code, _ = await asyncio.wait_for(read_reply(reader), cfg["timeout"])
                if code != 220:
                    raise RuntimeError("STARTTLS %d" % code)
                await writer.start_tls(client_ssl_context(cfg["insecure"]), server_hostname=cfg["host"])
                writer.write(b"EHLO loadgen.invalid\r\n")
                code, text = await asyncio.wait_for(read_reply(reader), cfg["timeout"])
            pipelining = b"PIPELINING" in text.upper() and not cfg["no_pipelining"]
            if cfg["user"]:
                token = base64.b64encode(("\0%s\0%s" % (cfg["user"], cfg["password"])).encode("utf-8"))
                writer.write(b"AUTH PLAIN " + token + b"\r\n")
                code, _ = await asyncio.wait_for(read_reply(reader), cfg["timeout"])
                if code != 235:
                    raise RuntimeError("AUTH %d" % code)
            consecutive_failures = 0
            sent = 0
            while index is not None:
                payload, rcpts, size = factory.build(index)
                commands = [b"MAIL FROM:<" + cfg["mail_from"].encode() + b"> SIZE=" + str(len(payload)).encode() + b"\r\n"]
                commands += [b"RCPT TO:<" + r.encode() + b">\r\n" for r in rcpts]
                commands.append(b"DATA\r\n")
                started = time.perf_counter()
                wall = time.time()
                codes = []
                if pipelining:
                    writer.write(b"".join(commands))
                    for _ in commands:
                        code, _ = await asyncio.wait_for(read_reply(reader), cfg["timeout"])
                        codes.append(code)
                else:
                    for command in commands:
                        writer.write(command)
                        code, _ = await asyncio.wait_for(read_reply(reader), cfg["timeout"])
                        codes.append(code)
                        if code >= 400:
                            break
                ok = codes and codes[0] == 250 and all(c in (250, 251) for c in codes[1:-1]) and codes[-1] == 354
                if ok:
                    writer.write(payload)
                    await writer.drain()
                    code, _ = await asyncio.wait_for(read_reply(reader), cfg["timeout"])
                    if code == 250:
                        now = time.time()
                        stats.latencies.append((time.perf_counter() - started) * 1000.0)
                        stats.accepted += 1
                        stats.bytes += len(payload)
                        if stats.first_send is None or wall < stats.first_send:
                            stats.first_send = wall
                        if stats.last_ack is None or now > stats.last_ack:
                            stats.last_ack = now
                    else:
                        stats.failed += 1
                        stats.error("end of data %d" % code)
                else:
                    stats.failed += 1
                    stats.error("envelope %s" % "/".join(str(c) for c in codes))
                    if codes and codes[-1] == 354:
                        writer.write(b".\r\n")
                        await asyncio.wait_for(read_reply(reader), cfg["timeout"])
                    writer.write(b"RSET\r\n")
                    await asyncio.wait_for(read_reply(reader), cfg["timeout"])
                sent += 1
                index = take() if sent < cfg["per_connection"] else None
            writer.write(b"QUIT\r\n")
            try:
                await asyncio.wait_for(read_reply(reader), 5)
            except Exception:
                pass
        except Exception as error:
            stats.failed += 1
            stats.error(type(error).__name__ + (": " + str(error) if str(error) and len(str(error)) < 60 else ""))
            consecutive_failures += 1
            if consecutive_failures >= 20:
                stats.error("client gave up after 20 failures in a row")
                return
            await asyncio.sleep(min(1.0, 0.05 * consecutive_failures))
        finally:
            if writer is not None:
                writer.close()
                try:
                    await writer.wait_closed()
                except Exception:
                    pass


def smtp_worker(cfg, worker, workers, clients, start_at, queue):
    """One worker process: its clients, its share of the messages (every
    workers-th, from its own number), started together at start_at."""
    factory = MessageFactory(cfg["sizes"], cfg["seed"], cfg["run_id"], cfg["mail_from"], cfg["rcpt_template"],
                             cfg["mailboxes"], cfg["rcpts_per_message"])
    indices = iter(range(worker, cfg["messages"], workers))

    def take():
        return next(indices, None)

    stats = SmtpWorkerStats()
    cpu = {}

    async def main():
        delay = start_at - time.time()
        if delay > 0:
            await asyncio.sleep(delay)
        # The generator's CPU is counted from the start of the load, not
        # through the making of the bodies before it.
        cpu["before"] = time.process_time()
        await asyncio.gather(*[smtp_client(cfg, factory, take, stats) for _ in range(clients)])
        cpu["after"] = time.process_time()

    asyncio.run(main())
    queue.put({
        "worker": worker, "latencies": stats.latencies, "accepted": stats.accepted, "failed": stats.failed,
        "bytes": stats.bytes, "errors": stats.errors, "connections": stats.connections,
        "first_send": stats.first_send, "last_ack": stats.last_ack, "cpu_seconds": cpu["after"] - cpu["before"],
    })


# ---------------------------------------------------------------- delivery check

class MailboxCounter(object):
    """How many messages each mailbox's INBOX holds, by IMAP STATUS: the
    count before the run and the count until the accepted messages are all
    there. Both servers queue before they deliver, so a 250 is not a message
    in a mailbox; this is."""

    def __init__(self, host, port, tls, insecure, users, password, timeout=30):
        self.connections = []
        for user in users:
            if tls == "implicit":
                connection = imaplib.IMAP4_SSL(host, port, ssl_context=client_ssl_context(insecure), timeout=timeout)
            else:
                connection = imaplib.IMAP4(host, port, timeout=timeout)
                if tls == "starttls":
                    connection.starttls(ssl_context=client_ssl_context(insecure))
            connection.login(user, password)
            self.connections.append((user, connection))

    def total(self):
        total = 0
        for _, connection in self.connections:
            status, data = connection.status("INBOX", "(MESSAGES)")
            if status != "OK":
                raise RuntimeError("STATUS INBOX answered %s" % status)
            match = re.search(rb"MESSAGES (\d+)", data[0] if data and data[0] else b"")
            total += int(match.group(1)) if match else 0
        return total

    def close(self):
        for _, connection in self.connections:
            try:
                connection.logout()
            except Exception:
                pass


# ---------------------------------------------------------------- the sink

class SinkStats(object):
    def __init__(self, run_id):
        self.run_id = run_id.encode("ascii") if run_id else None
        self.messages = 0
        self.run_messages = 0
        self.bytes = 0
        self.connections = 0
        self.first = None
        self.last = None
        self.end_to_end = []
        self.per_second = {}

    def record(self, data):
        now = time.time()
        self.messages += 1
        self.bytes += len(data)
        head = data[:32768]
        second = int(now)
        self.per_second[second] = self.per_second.get(second, 0) + 1
        if self.run_id is not None:
            match = re.search(rb"\r\nX-Loadgen-Run: ([^\r\n]+)", b"\r\n" + head)
            if not match or match.group(1).strip() != self.run_id:
                return
        self.run_messages += 1
        if self.first is None:
            self.first = now
        self.last = now
        match = re.search(rb"\r\nX-Loadgen-Sent: ([0-9.]+)", b"\r\n" + head)
        if match:
            try:
                self.end_to_end.append((now - float(match.group(1))) * 1000.0)
            except ValueError:
                pass

    def report(self):
        return {
            "messages": self.messages, "run_messages": self.run_messages, "bytes": self.bytes,
            "connections": self.connections, "first": self.first, "last": self.last,
            "end_to_end_ms": self.end_to_end,
            "busiest_second": max(self.per_second.values()) if self.per_second else 0,
        }


async def sink_connection(reader, writer, stats):
    """Accept and discard: the least an SMTP server can do. EHLO offers
    PIPELINING and 8BITMIME and no CHUNKING, so every relay sends DATA."""
    stats.connections += 1
    try:
        writer.write(b"220 sink.invalid loadgen sink\r\n")
        while True:
            line = await reader.readline()
            if not line:
                break
            verb = line[:4].upper()
            if verb == b"EHLO":
                writer.write(b"250-sink.invalid\r\n250-PIPELINING\r\n250-8BITMIME\r\n250-SIZE 0\r\n250 ENHANCEDSTATUSCODES\r\n")
            elif verb == b"HELO":
                writer.write(b"250 sink.invalid\r\n")
            elif verb in (b"MAIL", b"RCPT", b"RSET", b"NOOP"):
                writer.write(b"250 2.0.0 ok\r\n")
            elif verb == b"DATA":
                writer.write(b"354 go on\r\n")
                await writer.drain()
                data = await reader.readuntil(b"\r\n.\r\n")
                stats.record(data)
                writer.write(b"250 2.0.0 discarded\r\n")
            elif verb == b"QUIT":
                writer.write(b"221 2.0.0 bye\r\n")
                await writer.drain()
                break
            else:
                writer.write(b"502 5.5.2 not here\r\n")
            await writer.drain()
    except (ConnectionError, asyncio.IncompleteReadError, asyncio.LimitOverrunError):
        pass
    finally:
        writer.close()


async def start_sink(host, port, stats):
    return await asyncio.start_server(lambda r, w: sink_connection(r, w, stats), host, port,
                                      limit=128 * 1048576, reuse_address=True)


def sink_process(host, port, run_id, connection):
    """The sink in a process of its own, answering the parent over a pipe:
    'report' for its figures so far, 'stop' to end."""
    stats = SinkStats(run_id)

    async def main():
        server = await start_sink(host, port, stats)
        connection.send({"ready": True})
        loop = asyncio.get_running_loop()
        try:
            while True:
                request = await loop.run_in_executor(None, connection.recv)
                if request == "report":
                    connection.send(stats.report())
                elif request == "stop":
                    connection.send(stats.report())
                    break
        finally:
            server.close()
    try:
        asyncio.run(main())
    except OSError as error:
        connection.send({"ready": False, "error": str(error)})


def secret_from(value, variable):
    """A password from its option or, better, from the environment: a process's
    arguments are in every user's process list."""
    if value:
        return value
    return os.environ.get(variable, "")
