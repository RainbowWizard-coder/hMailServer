# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A toy mail server for the load generator's self-test, and for nothing else:
SMTP that keeps mail for one domain in memory and relays the rest to one host,
and IMAP that serves those in-memory mailboxes. It exists so that every mode
can be run end to end on a laptop, at low load, without a real server - which
proves the generator, and says nothing about any server.
"""

import asyncio
import re


class Store(object):
    def __init__(self):
        self.mailboxes = {}

    def deliver(self, user, data):
        self.mailboxes.setdefault(user.lower(), []).append(data)


async def relay(host, port, sender, rcpts, data):
    reader, writer = await asyncio.open_connection(host, port)
    try:
        async def reply():
            while True:
                line = await reader.readline()
                if not line or line[3:4] != b"-":
                    return line
        await reply()
        for command in [b"EHLO toy.invalid", b"MAIL FROM:<" + sender + b">"] + [b"RCPT TO:<" + r + b">" for r in rcpts] + [b"DATA"]:
            writer.write(command + b"\r\n")
            await reply()
        writer.write(data)
        await reply()
        writer.write(b"QUIT\r\n")
        await reply()
    finally:
        writer.close()


async def smtp(reader, writer, store, domain, relay_to, queue):
    writer.write(b"220 toy.invalid ESMTP toy\r\n")
    sender = b""
    rcpts = []
    try:
        while True:
            line = await reader.readline()
            if not line:
                break
            verb = line[:4].upper()
            if verb == b"EHLO":
                writer.write(b"250-toy.invalid\r\n250-PIPELINING\r\n250-AUTH PLAIN\r\n250 8BITMIME\r\n")
            elif verb == b"AUTH":
                writer.write(b"235 2.7.0 ok\r\n")
            elif verb == b"MAIL":
                match = re.search(rb"<([^>]*)>", line)
                sender = match.group(1) if match else b""
                rcpts = []
                writer.write(b"250 ok\r\n")
            elif verb == b"RCPT":
                match = re.search(rb"<([^>]*)>", line)
                rcpts.append(match.group(1) if match else b"")
                writer.write(b"250 ok\r\n")
            elif verb == b"DATA":
                writer.write(b"354 go on\r\n")
                await writer.drain()
                data = await reader.readuntil(b"\r\n.\r\n")
                remote = []
                for rcpt in rcpts:
                    if rcpt.lower().endswith(b"@" + domain):
                        store.deliver(rcpt.decode("ascii", "replace"), data)
                    else:
                        remote.append(rcpt)
                if remote and relay_to:
                    queue.put_nowait((sender, remote, b"Received: by toy.invalid\r\n" + data))
                writer.write(b"250 queued\r\n")
            elif verb == b"QUIT":
                writer.write(b"221 bye\r\n")
                await writer.drain()
                break
            else:
                writer.write(b"250 ok\r\n")
            await writer.drain()
    except (ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        writer.close()


async def imap(reader, writer, store, password):
    writer.write(b"* OK toy IMAP ready\r\n")
    user = None
    try:
        while True:
            line = await reader.readline()
            if not line:
                break
            parts = line.rstrip(b"\r\n").split(b" ", 2)
            tag = parts[0]
            verb = parts[1].upper() if len(parts) > 1 else b""
            rest = parts[2] if len(parts) > 2 else b""
            if verb == b"CAPABILITY":
                writer.write(b"* CAPABILITY IMAP4rev1 IDLE\r\n" + tag + b" OK capability\r\n")
            elif verb == b"LOGIN":
                words = [q if q else a for q, a in re.findall(rb'"((?:[^"\\]|\\.)*)"|(\S+)', rest)]
                words = [w.replace(b'\\"', b'"').replace(b"\\\\", b"\\").decode() for w in words]
                if len(words) == 2 and words[1] == password:
                    user = words[0].lower()
                    writer.write(tag + b" OK logged in\r\n")
                else:
                    writer.write(tag + b" NO wrong password\r\n")
            elif verb == b"SELECT":
                count = len(store.mailboxes.get(user, []))
                writer.write(b"* %d EXISTS\r\n* 0 RECENT\r\n%s OK [READ-WRITE] selected\r\n" % (count, tag))
            elif verb == b"STATUS":
                count = len(store.mailboxes.get(user, []))
                writer.write(b"* STATUS INBOX (MESSAGES %d)\r\n%s OK status\r\n" % (count, tag))
            elif verb == b"FETCH":
                messages = store.mailboxes.get(user, [])
                spec = rest.split(b" ", 1)
                first = spec[0].split(b":")[0]
                number = int(first) if first.isdigit() else 1
                if 1 <= number <= len(messages):
                    data = messages[number - 1]
                    if b"BODY.PEEK[HEADER]" in rest.upper():
                        data = data.split(b"\r\n\r\n", 1)[0] + b"\r\n\r\n"
                    if b"FLAGS" in rest.upper() and b"BODY" not in rest.upper():
                        writer.write(b"* %d FETCH (UID %d FLAGS ())\r\n" % (number, number))
                    else:
                        writer.write(b"* %d FETCH (BODY[] {%d}\r\n%s)\r\n" % (number, len(data), data))
                writer.write(tag + b" OK fetch\r\n")
            elif verb == b"IDLE":
                writer.write(b"+ idling\r\n")
                await writer.drain()
                await reader.readline()
                writer.write(tag + b" OK idle done\r\n")
            elif verb == b"LOGOUT":
                writer.write(b"* BYE\r\n" + tag + b" OK bye\r\n")
                await writer.drain()
                break
            else:
                writer.write(tag + b" OK\r\n")
            await writer.drain()
    except (ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        writer.close()


def main(host, smtp_port, imap_port, domain, relay_host, relay_port, password):
    store = Store()

    async def run():
        queue = asyncio.Queue()

        async def relayer():
            while True:
                sender, rcpts, data = await queue.get()
                for attempt in range(3):
                    try:
                        await relay(relay_host, relay_port, sender, rcpts, data)
                        break
                    except OSError:
                        await asyncio.sleep(0.2)
        workers = [asyncio.ensure_future(relayer()) for _ in range(4)] if relay_host else []
        smtp_server = await asyncio.start_server(lambda r, w: smtp(r, w, store, domain.encode(), relay_host, queue),
                                                 host, smtp_port, limit=64 * 1048576)
        imap_server = await asyncio.start_server(lambda r, w: imap(r, w, store, password), host, imap_port)
        print("toy server ready: SMTP %d, IMAP %d" % (smtp_port, imap_port), flush=True)
        async with smtp_server, imap_server:
            await asyncio.gather(smtp_server.serve_forever(), imap_server.serve_forever(), *workers)
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass
    return 0

