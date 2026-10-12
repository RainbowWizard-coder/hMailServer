#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The active-active lab's remote mail server: takes every message and remembers it.

Runs as container hmlab-p-aa-sink (aa-lab.sh nodes), listening on port 2525, where
the nodes' route for the domain sink.test sends. For every message it accepts it
appends one JSON line to $AA_SINK_LOG (default /data/sink.jsonl): the time, the
peer's address, the envelope's recipients, and the message's Message-ID and
X-AA-Seq fields. aa-load.py check reads that file to count what came twice and what
never came.

Two behaviours a scenario can ask for, by environment:
  AA_SINK_DATA_DELAY_MS    wait this long before answering the end of DATA - a slow
                           remote server, so that a delivery outlasts a lease
  AA_SINK_TEMPFAIL_EVERY   answer every Nth message 451, so that messages are
                           retried and pass through the queue more than once

Python's standard library only; one coroutine per connection.
"""

import asyncio
import json
import os
import time

LOG = os.environ.get("AA_SINK_LOG", "/data/sink.jsonl")
DELAY = int(os.environ.get("AA_SINK_DATA_DELAY_MS", "0")) / 1000.0
TEMPFAIL_EVERY = int(os.environ.get("AA_SINK_TEMPFAIL_EVERY", "0"))
counter = {"messages": 0}


def header(lines, name):
    prefix = name.lower() + ":"
    for line in lines:
        if line == b"":
            break
        text = line.decode("utf-8", errors="replace")
        if text.lower().startswith(prefix):
            return text[len(prefix):].strip()
    return ""


async def session(reader, writer):
    peer = writer.get_extra_info("peername")
    peer = peer[0] if peer else ""

    async def say(text):
        writer.write((text + "\r\n").encode())
        await writer.drain()

    await say("220 sink.test active-active lab sink")
    rcpts = []
    sender = None
    try:
        while True:
            line = await reader.readline()
            if not line:
                break
            command = line.decode("utf-8", errors="replace").strip()
            verb = command[:4].upper()
            if verb in ("EHLO", "HELO"):
                await say("250-sink.test" if verb == "EHLO" else "250 sink.test")
                if verb == "EHLO":
                    await say("250-8BITMIME")
                    await say("250 SIZE 104857600")
            elif verb == "MAIL":
                sender = command[10:].strip()
                rcpts = []
                await say("250 OK")
            elif verb == "RCPT":
                rcpts.append(command[8:].strip().strip("<>").lower())
                await say("250 OK")
            elif verb == "DATA":
                await say("354 go ahead")
                lines = []
                while True:
                    data = await reader.readline()
                    if not data:
                        return
                    if data in (b".\r\n", b".\n"):
                        break
                    if data.startswith(b".."):
                        data = data[1:]
                    lines.append(data.rstrip(b"\r\n"))
                if DELAY:
                    await asyncio.sleep(DELAY)
                counter["messages"] += 1
                if TEMPFAIL_EVERY and counter["messages"] % TEMPFAIL_EVERY == 0:
                    await say("451 4.3.0 try again later (lab)")
                    continue
                entry = {"t": time.time(), "peer": peer, "from": sender, "rcpts": rcpts,
                         "message_id": header(lines, "Message-ID"), "seq": header(lines, "X-AA-Seq")}
                with open(LOG, "a", encoding="utf-8") as log:
                    log.write(json.dumps(entry) + "\n")
                await say("250 OK queued")
                rcpts = []
            elif verb == "RSET":
                rcpts = []
                sender = None
                await say("250 OK")
            elif verb == "NOOP":
                await say("250 OK")
            elif verb == "QUIT":
                await say("221 bye")
                break
            else:
                await say("502 not here")
    except (ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        writer.close()


async def main():
    os.makedirs(os.path.dirname(LOG) or ".", exist_ok=True)
    server = await asyncio.start_server(session, "0.0.0.0", int(os.environ.get("AA_SINK_PORT", "2525")))
    print("sink listening on %s, logging to %s" % (server.sockets[0].getsockname(), LOG), flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
