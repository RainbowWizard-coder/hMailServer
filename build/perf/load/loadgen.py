#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The load generator behind hmailserver/docs/PublishedNumbers.md: messages per
second in and out, IMAP sessions per core and memory per session, against any
SMTP and IMAP server, so that the same run points at hMailServer and at Postfix
and Dovecot on the same machine.

Python 3.9 or later (3.11 for --tls starttls) and nothing but its standard
library (psutil is used when it is installed, for the server's memory; see
sampler.py). Why Python: every
number here is limited by the server long before it is by the generator,
provided the generator is spread over processes (--processes) and says when it
was not - each mode records its own processes' CPU and warns when one of them
was busy for more than 85% of the run, the sign that the generator was the
ceiling. A mail server that stores and indexes each message reaches thousands
a second, not the hundreds of thousands a benchmark of a web server needs; one
asyncio process drives several thousand SMTP transactions or IMAP commands a
second on one core, and --processes multiplies that by the cores given to it.

Modes:

  smtp-in         N clients deliver M messages of a size distribution to local
                  mailboxes. Messages accepted per second (the 250 after the
                  dot), latency percentiles, errors; with --verify-imap-port,
                  also delivered per second: until every accepted message is
                  counted in the mailboxes by IMAP STATUS.
  smtp-out        the same clients send to a remote domain the server relays to
                  the sink this tool runs (--sink-listen); relayed messages per
                  second measured AT THE SINK, and the end-to-end latency from
                  the client's MAIL FROM to the sink's receipt. On Windows,
                  with the sink on the server's own machine, each relayed
                  message's connection holds one of the loopback's dynamic
                  ports in TIME_WAIT for two minutes, 16,384 ports by default:
                  20,000 messages ran out of them on the Windows bench and
                  some 4.5% bounced, so PublishedNumbers.md runs 12,000 there,
                  with the ports given back between two runs, and the tool
                  says so when it is asked for more.
  imap-sessions   N concurrent IMAP sessions (LOGIN, SELECT, then FETCH, NOOP
                  and IDLE by weight), N stepped up until a latency percentile
                  passes its limit: the most sessions per core that stayed
                  under it, and the server's memory per session. The idle
                  point the memory is fitted through is taken once the
                  server's memory has stopped falling (--idle-wait), and a
                  run whose idle point is above its first step's - a server
                  still holding the last run's sessions - says its memory
                  figure is not valid. A run that passes its last step says
                  so: the limit was not reached.
  imaptest        Dovecot's imaptest in a benchmark profile against any server.
  sink            the sink alone: accept and discard, count, report.
  machine         the machine this runs on, as JSON (for the server's machine,
                  when the generator runs on another: --server-machine).
  prepare-hmailserver
                  the domain, accounts, route, IP range and fsync setting a run
                  needs on hMailServer, through its REST API (--cleanup undoes).
  self-test       every mode against a toy server of this tool's own, at low
                  load, on this machine: proves the generator, not any server.

Every load mode takes --host and --port, writes one JSON document (--json) and
a Markdown table (--markdown) with the machine's CPU, cores, RAM, OS and disk,
and takes --label and --notes for what the machine cannot say about itself:
the server and its version, the database, the fsync policy, TLS.

Passwords come from the environment, not the command line (a process's
arguments are in every user's process list): HMLOAD_PASSWORD for the load
mailboxes (IMAP, and SMTP AUTH unless HMLOAD_SMTP_PASSWORD is set), and
HMLOAD_ADMIN_PASSWORD for prepare-hmailserver.

Examples (PublishedNumbers.md has the whole recipe):

  python build/perf/load/loadgen.py self-test
  python build/perf/load/loadgen.py smtp-in --host 10.0.0.5 --port 25 \\
      --rcpt-template "load{n}@example.test" --mailboxes 50 --clients 32 --messages 20000 \\
      --processes 4 --verify-imap-port 143 --server-process hmailserver \\
      --label "hMailServer 6.3.4, MariaDB, fsync on" --json in.json --markdown in.md
"""

import argparse
import json
import multiprocessing
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
TREE = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

import imapload  # noqa: E402
import imaptestrun  # noqa: E402
import machine  # noqa: E402
import report  # noqa: E402
import sampler as sampling  # noqa: E402
import smtpload  # noqa: E402

TOOL = "build/perf/load/loadgen.py"
DEFAULT_SIZES = "2k:25,8k:35,32k:25,128k:10,1m:5"
DEFAULT_OPS = "noop:3,fetch-flags:3,fetch-header:2,fetch-body:1,idle:1"

# The most messages smtp-out relays on Windows to a sink on the server's own
# machine before the tool warns: the runs on server1 of 7 October 2026 relayed
# 12,000 whole eight times, two minutes and more apart, and 20,000 three times
# with 824 to 940 bounced (PublishedNumbers.md).
WINDOWS_LOOPBACK_MESSAGES = 12000


def say(text):
    sys.stderr.write(text + "\n")
    sys.stderr.flush()


# ---------------------------------------------------------------- shared

def add_common(parser, port):
    parser.add_argument("--host", default="127.0.0.1", help="the server under test")
    parser.add_argument("--port", type=int, default=port)
    parser.add_argument("--tls", choices=("none", "starttls", "implicit"), default="none",
                        help="TLS for the clients: the same for both servers in a comparison (default none)")
    parser.add_argument("--insecure", action="store_true", help="accept any certificate")
    parser.add_argument("--timeout", type=float, default=60.0, help="seconds to wait for one reply")
    parser.add_argument("--processes", type=int, default=max(1, min(4, (os.cpu_count() or 2) // 2)),
                        help="generator worker processes (default: half the cores, at most 4)")
    parser.add_argument("--seed", type=int, default=49)
    parser.add_argument("--server", default="", help="what the target is: hMailServer 6.3.4, Postfix 3.10 + Dovecot 2.4, ...")
    parser.add_argument("--label", default="", help="a name for the run: the server, the database, the fsync policy")
    parser.add_argument("--notes", default="", help="anything else the reader must know")
    parser.add_argument("--data-path", default="", help="where the server keeps its data, when it is on this machine: the disk described")
    parser.add_argument("--server-machine", default="", help="a machine JSON written by 'loadgen.py machine' on the server's machine")
    parser.add_argument("--json", default="", help="write the JSON result here")
    parser.add_argument("--markdown", default="", help="write the Markdown table here")
    sampling.add_arguments(parser)


def document_for(args, mode, parameters):
    doc = {
        "tool": TOOL,
        "format": report.FORMAT_VERSION,
        "mode": mode,
        "label": args.label,
        "notes": args.notes,
        "started": report.now_iso(),
        "target": {"server": args.server, "host": args.host, "port": args.port, "tls": args.tls},
        "parameters": parameters,
        "machine": machine.collect(args.data_path or None),
    }
    if args.server_machine:
        doc["server_machine"] = machine.load(args.server_machine)
    return doc


def server_cores(args, doc):
    if getattr(args, "server_cores", 0):
        return args.server_cores
    host = doc.get("server_machine") or doc["machine"]
    return (host.get("cpu") or {}).get("logical_cores") or os.cpu_count() or 1


def spread(total, parts):
    return [total // parts + (1 if k < total % parts else 0) for k in range(parts)]


def memory_rows(window):
    rows = []
    if not window:
        return rows
    figure, name = sampling.memory_figure(window)
    if figure is not None:
        rows.append(("Server memory, mean (%s)" % name, "%s MiB" % report.mib(figure)))
    for key, name in (("rss_bytes_peak", "RSS"), ("pss_bytes_peak", "PSS"), ("private_bytes_peak", "private bytes"),
                      ("cgroup_anon_bytes_peak", "cgroup anon"), ("cgroup_current_bytes_peak", "cgroup charge")):
        if key in window:
            rows.append(("Server memory, peak (%s)" % name, "%s MiB" % report.mib(window[key])))
    if window.get("cpu_cores_used") is not None:
        rows.append(("Server CPU used", "%s cores" % window["cpu_cores_used"]))
    if window.get("cgroup_event_max") or window.get("cgroup_event_oom"):
        rows.append(("Memory limit reached (cgroup events)", "max %s, oom %s - the run is not valid" % (
            window.get("cgroup_event_max", 0), window.get("cgroup_event_oom", 0))))
    return rows


# ---------------------------------------------------------------- SMTP

def add_smtp(parser, rcpt_template, mail_from):
    parser.add_argument("--clients", type=int, default=16, help="concurrent SMTP connections (default 16)")
    parser.add_argument("--messages", type=int, default=2000, help="messages in the run (default 2000)")
    parser.add_argument("--per-connection", type=int, default=10, help="messages sent on one connection before QUIT (default 10)")
    parser.add_argument("--sizes", default=DEFAULT_SIZES, help="size:weight,... (default %s)" % DEFAULT_SIZES.replace("%", "%%"))
    parser.add_argument("--mail-from", default=mail_from)
    parser.add_argument("--rcpt-template", default=rcpt_template, help="recipient address; {n} is the mailbox number")
    parser.add_argument("--mailboxes", type=int, default=10, help="how many mailboxes {n} runs over (default 10)")
    parser.add_argument("--rcpts-per-message", type=int, default=1)
    parser.add_argument("--auth-user", default="", help="SMTP AUTH PLAIN as this user (password: HMLOAD_SMTP_PASSWORD or HMLOAD_PASSWORD)")
    parser.add_argument("--no-pipelining", action="store_true", help="one command at a time even where PIPELINING is offered")


def smtp_config(args, run_id):
    return {
        "host": args.host, "port": args.port, "tls": args.tls, "insecure": args.insecure, "timeout": args.timeout,
        "sizes": args.sizes, "seed": args.seed, "run_id": run_id, "mail_from": args.mail_from,
        "rcpt_template": args.rcpt_template, "mailboxes": args.mailboxes, "rcpts_per_message": args.rcpts_per_message,
        "messages": args.messages, "per_connection": max(1, args.per_connection),
        "user": args.auth_user,
        "password": smtpload.secret_from("", "HMLOAD_SMTP_PASSWORD") or smtpload.secret_from("", "HMLOAD_PASSWORD"),
        "no_pipelining": args.no_pipelining,
    }


def run_smtp_clients(args, cfg):
    """The clients in their processes; the merged figures."""
    workers = max(1, min(args.processes, args.clients))
    clients = spread(args.clients, workers)
    queue = multiprocessing.Queue()
    start_at = time.time() + 1.0 + 0.5 * workers
    processes = []
    for worker in range(workers):
        process = multiprocessing.Process(target=smtpload.smtp_worker,
                                          args=(cfg, worker, workers, clients[worker], start_at, queue))
        process.start()
        processes.append(process)
    results = [queue.get() for _ in processes]
    for process in processes:
        process.join()
    merged = {"latencies": [], "accepted": 0, "failed": 0, "bytes": 0, "errors": {}, "connections": 0,
              "first_send": None, "last_ack": None, "cpu_seconds": [], "start_at": start_at}
    for result in results:
        merged["latencies"] += result["latencies"]
        for key in ("accepted", "failed", "bytes", "connections"):
            merged[key] += result[key]
        for key, value in result["errors"].items():
            merged["errors"][key] = merged["errors"].get(key, 0) + value
        if result["first_send"] and (merged["first_send"] is None or result["first_send"] < merged["first_send"]):
            merged["first_send"] = result["first_send"]
        if result["last_ack"] and (merged["last_ack"] is None or result["last_ack"] > merged["last_ack"]):
            merged["last_ack"] = result["last_ack"]
        merged["cpu_seconds"].append(result["cpu_seconds"])
    return merged


def smtp_parameters(args):
    return {
        "clients": args.clients, "messages": args.messages, "per_connection": args.per_connection,
        "sizes": args.sizes, "mail_from": args.mail_from, "rcpt_template": args.rcpt_template,
        "mailboxes": args.mailboxes, "rcpts_per_message": args.rcpts_per_message,
        "auth": bool(args.auth_user), "pipelining": not args.no_pipelining, "processes": args.processes,
        "seed": args.seed,
    }


def smtp_rows(merged, seconds):
    latency = report.summary(merged["latencies"])
    rows = [
        ("Messages accepted", "%d of %d sent (%d failed)" % (merged["accepted"], merged["accepted"] + merged["failed"], merged["failed"])),
        ("Accepted per second", report.cell(report.rate(merged["accepted"], seconds))),
        ("Accepted MiB per second", report.cell(report.rate(merged["bytes"] / 1048576.0, seconds))),
        ("Latency MAIL FROM to 250, p50 / p95 / p99 / max (ms)", "%s / %s / %s / %s" % (
            report.cell(latency.get("p50")), report.cell(latency.get("p95")), report.cell(latency.get("p99")),
            report.cell(latency.get("max")))),
    ]
    if merged["errors"]:
        rows.append(("Errors", ", ".join("%s x%d" % (k, v) for k, v in sorted(merged["errors"].items()))))
    return rows, latency


def cmd_smtp_in(args):
    run_id = uuid.uuid4().hex[:12]
    cfg = smtp_config(args, run_id)
    doc = document_for(args, "smtp-in", smtp_parameters(args))
    sampler = sampling.from_arguments(args)
    counter = None
    baseline = 0
    if args.verify_imap_port:
        factory = smtpload.MessageFactory(args.sizes, args.seed, run_id, args.mail_from, args.rcpt_template,
                                          args.mailboxes, args.rcpts_per_message)
        users = [args.rcpt_template.format(n=n) for n in factory.mailbox_numbers(args.messages)]
        counter = smtpload.MailboxCounter(args.verify_imap_host or args.host, args.verify_imap_port, args.verify_tls,
                                          args.insecure, users, smtpload.secret_from("", "HMLOAD_PASSWORD"))
        baseline = counter.total()
        say("mailboxes hold %d messages before the run" % baseline)
    sampled_from = sampler.mark()
    sampler.start()
    say("smtp-in: %d messages, %d clients, %d processes, to %s:%d" % (args.messages, args.clients, args.processes, args.host, args.port))
    merged = run_smtp_clients(args, cfg)
    seconds = (merged["last_ack"] - merged["first_send"]) if merged["first_send"] and merged["last_ack"] else None
    rows, latency = smtp_rows(merged, seconds)
    delivered = None
    end = merged["last_ack"] or time.time()
    if counter is not None:
        expected = merged["accepted"] * args.rcpts_per_message
        deadline = time.time() + args.drain_timeout
        count = 0
        while time.time() < deadline:
            count = counter.total() - baseline
            if count >= expected:
                break
            time.sleep(0.25)
        reached = time.time()
        counter.close()
        delivered = {"expected": expected, "counted": count, "complete": count >= expected,
                     "seconds_from_first_send": round(reached - merged["first_send"], 3) if merged["first_send"] else None,
                     "poll_interval_seconds": 0.25}
        end = reached
        if count >= expected and merged["first_send"]:
            rows.append(("Delivered per second (all in the mailboxes, IMAP STATUS)",
                         report.cell(report.rate(count, reached - merged["first_send"]))))
        else:
            rows.append(("Delivered", "%d of %d after %d s: incomplete, no rate" % (count, expected, args.drain_timeout)))
    sampler.stop()
    window = sampler.window(sampled_from, sampler.mark()) if sampler.enabled else None
    rows += memory_rows(window)
    doc["finished"] = report.now_iso()
    doc["generator"] = report.generator_verdict(merged["cpu_seconds"], ((merged["last_ack"] or time.time()) - merged["start_at"]) or 1)
    doc["results"] = {
        "accepted": merged["accepted"], "failed": merged["failed"], "bytes": merged["bytes"],
        "connections": merged["connections"], "errors": merged["errors"], "seconds": seconds,
        "accepted_per_second": report.rate(merged["accepted"], seconds),
        "latency_ms": latency, "delivered": delivered,
        "delivered_per_second": report.rate(delivered["counted"], delivered["seconds_from_first_send"])
        if delivered and delivered["complete"] else None,
        "server": {"sampler": sampler.describe(), "window": window},
    }
    report.write(doc, rows, args.json, args.markdown)
    return 0 if merged["accepted"] and not merged["failed"] else 1


def loopback_sink(host):
    """True when the sink listens where a server on this machine reaches it
    over the loopback: a loopback address, every address, or no address."""
    host = host.strip("[]")
    return host in ("", "0.0.0.0", "::", "::1", "localhost") or host.startswith("127.")


def cmd_smtp_out(args):
    run_id = uuid.uuid4().hex[:12]
    cfg = smtp_config(args, run_id)
    parameters = smtp_parameters(args)
    parameters["sink"] = args.sink_listen
    doc = document_for(args, "smtp-out", parameters)
    host, _, port = args.sink_listen.rpartition(":")
    if os.name == "nt" and args.messages > WINDOWS_LOOPBACK_MESSAGES and loopback_sink(host):
        say("smtp-out: on Windows each message relayed to a sink on the server's own machine holds one of the "
            "loopback's dynamic ports in TIME_WAIT for two minutes (16,384 ports by default); %d messages may run "
            "out of them, and on the Windows bench 20,000 did, some 4.5%% bouncing. PublishedNumbers.md runs %d "
            "there, two minutes or more apart." % (args.messages, WINDOWS_LOOPBACK_MESSAGES))
    parent, child = multiprocessing.Pipe()
    sink = multiprocessing.Process(target=smtpload.sink_process, args=(host or "0.0.0.0", int(port), run_id, child))
    sink.start()
    ready = parent.recv()
    if not ready.get("ready"):
        say("the sink could not listen on %s: %s" % (args.sink_listen, ready.get("error")))
        sink.join()
        return 2
    sampler = sampling.from_arguments(args)
    sampled_from = sampler.mark()
    sampler.start()
    say("smtp-out: %d messages to %s, relayed by %s:%d to the sink on %s" % (
        args.messages, args.rcpt_template, args.host, args.port, args.sink_listen))
    merged = run_smtp_clients(args, cfg)
    seconds = (merged["last_ack"] - merged["first_send"]) if merged["first_send"] and merged["last_ack"] else None
    rows, latency = smtp_rows(merged, seconds)
    deadline = time.time() + args.drain_timeout
    expected = merged["accepted"]
    while True:
        parent.send("report")
        figures = parent.recv()
        if figures["run_messages"] >= expected or time.time() > deadline:
            break
        time.sleep(0.25)
    parent.send("stop")
    figures = parent.recv()
    sink.join()
    sampler.stop()
    sink_seconds = (figures["last"] - figures["first"]) if figures["first"] and figures["last"] else None
    end_to_end_seconds = (figures["last"] - merged["first_send"]) if figures["last"] and merged["first_send"] else None
    e2e = report.summary(figures["end_to_end_ms"])
    complete = figures["run_messages"] >= expected
    rows = [r for r in rows if not r[0].startswith("Accepted MiB")]
    rows += [
        ("Relayed messages at the sink", "%d of %d accepted%s" % (figures["run_messages"], expected, "" if complete else " - INCOMPLETE")),
        ("Relayed per second at the sink, first to last arrival", report.cell(report.rate(figures["run_messages"], sink_seconds))),
        ("Relayed per second, first MAIL FROM to last arrival", report.cell(report.rate(figures["run_messages"], end_to_end_seconds))),
        ("Latency client MAIL FROM to sink, p50 / p95 / p99 / max (ms)", "%s / %s / %s / %s" % (
            report.cell(e2e.get("p50")), report.cell(e2e.get("p95")), report.cell(e2e.get("p99")), report.cell(e2e.get("max")))),
        ("Busiest second at the sink", "%d messages" % figures["busiest_second"]),
    ]
    window = sampler.window(sampled_from, sampler.mark()) if sampler.enabled else None
    rows += memory_rows(window)
    doc["finished"] = report.now_iso()
    doc["generator"] = report.generator_verdict(merged["cpu_seconds"], ((merged["last_ack"] or time.time()) - merged["start_at"]) or 1)
    del figures["end_to_end_ms"]
    doc["results"] = {
        "accepted": merged["accepted"], "failed": merged["failed"], "errors": merged["errors"], "seconds": seconds,
        "accepted_per_second": report.rate(merged["accepted"], seconds), "latency_ms": latency,
        "sink": figures, "sink_complete": complete,
        "relayed_per_second_at_sink": report.rate(figures["run_messages"], sink_seconds),
        "relayed_per_second_end_to_end": report.rate(figures["run_messages"], end_to_end_seconds),
        "end_to_end_latency_ms": e2e,
        "server": {"sampler": sampler.describe(), "window": window},
    }
    report.write(doc, rows, args.json, args.markdown)
    return 0 if complete and not merged["failed"] else 1


def cmd_sink(args):
    host, _, port = args.listen.rpartition(":")
    stats = smtpload.SinkStats(args.run or None)
    import asyncio

    async def main():
        server = await smtpload.start_sink(host or "0.0.0.0", int(port), stats)
        say("sink on %s; Ctrl-C ends it%s" % (args.listen, " after %d s" % args.seconds if args.seconds else ""))
        started = time.time()
        last = 0
        async with server:
            while not args.seconds or time.time() - started < args.seconds:
                await asyncio.sleep(5)
                say("%6.0f s  %d messages (+%d)" % (time.time() - started, stats.run_messages, stats.run_messages - last))
                last = stats.run_messages
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
    figures = stats.report()
    seconds = (figures["last"] - figures["first"]) if figures["first"] and figures["last"] else None
    figures["per_second"] = report.rate(figures["run_messages"], seconds)
    figures["end_to_end_ms"] = report.summary(figures["end_to_end_ms"])
    text = json.dumps(figures, indent=2) + "\n"
    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    sys.stdout.write(text)
    return 0


# ---------------------------------------------------------------- IMAP

def settle(sampler, most_seconds, span=None):
    """The idle server's memory, taken once it has stopped falling: windows of
    three samples one after another until one is no more than 1 per cent
    below the one before it, for at most most_seconds (0: the first window
    alone, as every run took it until 8 October 2026). A server that has just
    closed the last run's sessions is still giving their memory back, and an
    idle point taken then is a busy server's. (window, how it was taken)."""
    span = span if span is not None else max(3.0, 3 * sampler.interval)
    began = time.time()
    previous = None
    figures = []
    while True:
        started = time.time()
        time.sleep(span)
        window = sampler.window(started, time.time())
        figure, _ = sampling.memory_figure(window)
        figures.append(figure)
        if figure is None:
            settled = None
        elif previous is not None and figure >= previous * 0.99:
            settled = True
        elif time.time() - began >= most_seconds:
            # Still falling when the time ran out; or not asked to wait.
            settled = False if most_seconds > 0 else None
        else:
            previous = figure
            continue
        return window, {"seconds": round(time.time() - began, 1), "settled": settled, "figures": figures}


def idle_point_problem(idle_figure, first_step_figure, first_step_sessions, settled):
    """Why the memory per session of a run is not valid, or None. The idle
    point is the server with no session of the run's: above the first step's
    memory, the server still held something the steps then gave back - on
    7 October 2026 a run begun ten seconds after the last held its 1,600
    sessions, 526 MiB against 238 MiB at the first step - and a slope fitted
    through it is pulled down."""
    if idle_figure is not None and first_step_figure is not None and idle_figure > first_step_figure:
        return ("the idle server's memory (%s MiB) is above the first step's (%s MiB at %d sessions): the server was not "
                "idle when the run began" % (report.mib(idle_figure), report.mib(first_step_figure), first_step_sessions))
    if settled is False:
        return "the idle server's memory was still falling when the run began (--idle-wait)"
    return None


def cmd_imap_sessions(args):
    steps = [int(s) for s in args.steps.split(",") if s.strip()]
    imapload.parse_ops(args.ops)
    cfg = {
        "host": args.host, "port": args.port, "tls": args.tls, "insecure": args.insecure, "timeout": args.timeout,
        "user_template": args.user_template, "users": args.users,
        "password": smtpload.secret_from("", "HMLOAD_PASSWORD"), "ops": args.ops, "think_ms": args.think_ms,
        "idle_seconds": args.idle_seconds, "seed": args.seed, "connect_rate": args.connect_rate,
    }
    parameters = {
        "steps": steps, "step_seconds": args.step_seconds, "settle_seconds": args.settle_seconds, "ops": args.ops,
        "think_ms": args.think_ms, "idle_seconds": args.idle_seconds, "users": args.users,
        "user_template": args.user_template, "connect_rate": args.connect_rate,
        "latency_limit_ms": args.latency_limit_ms, "latency_percentile": args.latency_percentile,
        "max_error_rate": args.max_error_rate, "processes": args.processes, "seed": args.seed,
        "idle_wait": args.idle_wait,
    }
    doc = document_for(args, "imap-sessions", parameters)
    cores = server_cores(args, doc)
    sampler = sampling.from_arguments(args)
    sampler.start()
    baseline_window = None
    baseline_taken = None
    if sampler.enabled:
        baseline_window, baseline_taken = settle(sampler, args.idle_wait)
        say("imap-sessions: the idle server sampled after %s s%s" % (
            baseline_taken["seconds"], {True: ", its memory steady", False: ", its memory STILL FALLING"}.get(baseline_taken["settled"], "")))
    workers = max(1, args.processes)
    pipes = []
    processes = []
    for worker in range(workers):
        parent, child = multiprocessing.Pipe()
        process = multiprocessing.Process(target=imapload.imap_worker, args=(cfg, worker, workers, child))
        process.start()
        pipes.append(parent)
        processes.append(process)
    results = []
    best = None
    cpu_seconds = [0.0] * workers
    run_started = time.time()
    fraction = args.latency_percentile / 100.0
    try:
        for step in steps:
            say("imap-sessions: %d sessions" % step)
            shares = spread(step, workers)
            for pipe, share in zip(pipes, shares):
                pipe.send(("target", share))
            alive = 0
            failures = 0
            logins = []
            for pipe in pipes:
                _, a, f, l = pipe.recv()
                alive += a
                failures += f
                logins += l
            window_start = time.time() + args.settle_seconds
            window_end = window_start + args.step_seconds
            time.sleep(max(0.0, window_end - time.time()))
            records = []
            errors = {}
            alive_after = 0
            for index, pipe in enumerate(pipes):
                pipe.send(("collect", window_start, window_end))
                _, r, e, a, cpu = pipe.recv()
                records += r
                alive_after += a
                cpu_seconds[index] = cpu
                for key, value in e.items():
                    errors[key] = errors.get(key, 0) + value
            latencies = [r[2] for r in records]
            by_op = {}
            for _, name, ms, _ in records:
                by_op.setdefault(name, []).append(ms)
            failed_ops = len([r for r in records if not r[3]])
            error_count = sum(errors.values()) + failures
            error_rate = error_count / float(max(1, len(records) + error_count))
            ordered = sorted(latencies)
            measured = report.percentile(ordered, fraction)
            window = sampler.window(window_start, window_end) if sampler.enabled else None
            passed = (alive_after >= step and failures == 0 and measured is not None
                      and measured <= args.latency_limit_ms and error_rate <= args.max_error_rate)
            entry = {
                "sessions": step, "alive": alive_after, "open_failures": failures,
                "operations": len(records), "operations_per_second": report.rate(len(records), args.step_seconds),
                "failed_operations": failed_ops, "errors": errors, "error_rate": round(error_rate, 5),
                "latency_ms": report.summary(latencies), "latency_by_operation_ms": {k: report.summary(v) for k, v in sorted(by_op.items())},
                "login_ms": report.summary(logins), "percentile_measured_ms": round(measured, 2) if measured is not None else None,
                "passed": passed, "server": window,
            }
            results.append(entry)
            say("  %d alive, %d ops, p%g %s ms, errors %d: %s" % (
                alive_after, len(records), args.latency_percentile, report.cell(entry["percentile_measured_ms"]),
                error_count, "passed" if passed else "FAILED"))
            if passed:
                best = entry
            elif not args.keep_going:
                break
    finally:
        for pipe in pipes:
            try:
                pipe.send(("stop",))
                pipe.recv()
            except (EOFError, OSError):
                pass
        for process in processes:
            process.join(timeout=60)
        sampler.stop()
    wall = time.time() - run_started

    # Memory per session: the slope of the memory figure over the sessions, by
    # least squares through the baseline (no sessions) and every measured step.
    points = []
    figure_name = None
    base_figure, base_name = sampling.memory_figure(baseline_window)
    if base_figure is not None:
        points.append((0, base_figure))
        figure_name = base_name
    for entry in results:
        figure, name = sampling.memory_figure(entry["server"])
        if figure is not None and (figure_name is None or name == figure_name):
            figure_name = name
            points.append((entry["alive"], figure))
    per_session = None
    if len(points) >= 2:
        mean_x = sum(p[0] for p in points) / float(len(points))
        mean_y = sum(p[1] for p in points) / float(len(points))
        spread_x = sum((p[0] - mean_x) ** 2 for p in points)
        if spread_x > 0:
            per_session = sum((p[0] - mean_x) * (p[1] - mean_y) for p in points) / spread_x
    first_figure = sampling.memory_figure(results[0]["server"])[0] if results else None
    memory_warning = idle_point_problem(base_figure, first_figure, results[0]["sessions"] if results else 0,
                                        baseline_taken["settled"] if baseline_taken else None)
    if memory_warning and per_session is not None:
        say("imap-sessions: memory per session not valid: " + memory_warning)
    # Every step passed: the procedure did not find the limit, and the most
    # sessions it can show is its last step's.
    at_last_step = bool(best) and best is results[-1] and best["sessions"] == steps[-1]

    memory_cell = "not sampled"
    if per_session is not None:
        memory_cell = "%s KiB" % round(per_session / 1024.0, 1)
        if memory_warning:
            memory_cell = "not valid (%s KiB fitted): %s" % (round(per_session / 1024.0, 1), memory_warning)
    rows = [
        ("Latency limit", "p%g of every operation at most %s ms, no errors, every session alive" % (
            args.latency_percentile, args.latency_limit_ms)),
        ("Most sessions under the limit", (str(best["sessions"]) + (", the last step: the limit was not reached" if at_last_step else ""))
         if best else "none: the first step failed"),
        ("Server cores (logical)", str(cores)),
        ("Sessions per core", (report.cell(round(best["sessions"] / float(cores), 1)) + (", at the last step" if at_last_step else ""))
         if best else "-"),
        ("Memory per session (%s, slope over the steps)" % (figure_name or "not sampled"), memory_cell),
    ]
    if best and best.get("server") and best["server"].get("cpu_cores_used") is not None:
        rows.append(("Server CPU at that step", "%s cores" % best["server"]["cpu_cores_used"]))
    for entry in results:
        rows.append(("Step %d sessions" % entry["sessions"], "%s ops/s, p50 %s, p%g %s, p99 %s ms, %d errors, %s%s" % (
            report.cell(entry["operations_per_second"]), report.cell(entry["latency_ms"].get("p50")),
            args.latency_percentile, report.cell(entry["percentile_measured_ms"]), report.cell(entry["latency_ms"].get("p99")),
            sum(entry["errors"].values()) + entry["open_failures"], "passed" if entry["passed"] else "failed",
            (", memory %s MiB" % report.mib(sampling.memory_figure(entry["server"])[0])) if entry["server"] else "")))
    doc["finished"] = report.now_iso()
    doc["generator"] = report.generator_verdict(cpu_seconds, wall)
    doc["results"] = {
        "server_cores": cores, "best_sessions": best["sessions"] if best else None,
        "sessions_per_core": round(best["sessions"] / float(cores), 2) if best else None,
        "best_is_last_step": at_last_step,
        "memory_per_session_bytes": int(per_session) if per_session is not None else None,
        "memory_warning": memory_warning,
        "memory_figure": figure_name, "memory_points": points, "baseline": baseline_window, "baseline_taken": baseline_taken,
        "steps": results, "server": {"sampler": sampler.describe()},
    }
    report.write(doc, rows, args.json, args.markdown)
    return 0 if best else 1


def cmd_imaptest(args):
    users = [args.user_template.format(n=n) for n in range(1, args.users + 1)]
    password = smtpload.secret_from("", "HMLOAD_PASSWORD")
    users_dir = imaptestrun.write_users(users, password or "unset")
    try:
        cmd, how = imaptestrun.command(args, users_dir, TREE)
        if args.dry_run:
            sys.stdout.write(" ".join(cmd) + "\n")
            return 0
        doc = document_for(args, "imaptest", {
            "profile": args.profile, "profile_arguments": imaptestrun.PROFILES[args.profile],
            "extra_arguments": args.imaptest_arg, "clients": args.clients, "seconds": args.seconds,
            "users": args.users, "user_template": args.user_template, "mbox": args.mbox, "run_as": how,
            "command": [c for c in cmd],
        })
        sampler = sampling.from_arguments(args)
        started = sampler.mark()
        sampler.start()
        say("imaptest (%s, %s profile): %d clients for %d s against %s:%d" % (how, args.profile, args.clients, args.seconds, args.host, args.port))
        code, output = imaptestrun.run(cmd, args.seconds + 300)
        sampler.stop()
        ended = sampler.mark()
    finally:
        try:
            os.remove(os.path.join(users_dir, "users"))
            os.rmdir(users_dir)
        except OSError:
            pass
    if args.log:
        with open(args.log, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(output)
    parsed = imaptestrun.parse_output(output)
    window = sampler.window(started, ended) if sampler.enabled else None
    rows = [("imaptest exit code", str(code)), ("Seconds of counters", str(parsed["seconds"]))]
    for name, value in sorted((parsed.get("per_second_steady") or {}).items()):
        rows.append(("%s per second (steady)" % name, report.cell(value)))
    rows.append(("imaptest errors", str(parsed["error_count"])))
    rows += memory_rows(window)
    doc["finished"] = report.now_iso()
    doc["results"] = {"exit_code": code, "imaptest": parsed, "output_tail": output.splitlines()[-60:],
                      "server": {"sampler": sampler.describe(), "window": window}}
    report.write(doc, rows, args.json, args.markdown)
    return 0 if code == 0 and parsed["seconds"] else 1


# ---------------------------------------------------------------- the rest

def cmd_machine(args):
    data = {"tool": TOOL, "collected": report.now_iso(), "machine": machine.collect(args.data_path or None)}
    text = json.dumps(data, indent=2) + "\n"
    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    sys.stdout.write(text)
    return 0


def cmd_prepare(args):
    import hmprepare
    password = smtpload.secret_from("", "HMLOAD_ADMIN_PASSWORD")
    if not password:
        say("HMLOAD_ADMIN_PASSWORD is not set: the administrator's password, from the environment")
        return 2
    rest = hmprepare.Rest(args.rest, password, args.insecure)
    if args.cleanup:
        return 0 if hmprepare.cleanup(rest, args, say) else 1
    account_password = smtpload.secret_from("", "HMLOAD_PASSWORD")
    if not account_password:
        say("HMLOAD_PASSWORD is not set: the load accounts' password, from the environment")
        return 2
    return 0 if hmprepare.prepare(rest, args, account_password, say) else 1


def cmd_toy_server(args):
    import toyserver
    relay_host, _, relay_port = args.relay.rpartition(":") if args.relay else ("", "", "0")
    return toyserver.main(args.listen, args.smtp_port, args.imap_port, args.domain, relay_host or None,
                          int(relay_port or 0), os.environ.get("HMLOAD_PASSWORD", ""))


def free_port():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def cmd_self_test(args):
    """Every mode end to end against the toy server, at low load. Each check
    is printed with its verdict; the exit code is the number that failed."""
    failures = []

    def check(name, condition, detail=""):
        say("%s  %s%s" % ("PASS" if condition else "FAIL", name, (" - " + detail) if detail else ""))
        if not condition:
            failures.append(name)

    # Pure parts first.
    check("sizes parse", smtpload.parse_sizes("2k:1,1m:3") == [(2048, 1.0), (1048576, 3.0)])
    check("percentile", report.percentile([1, 2, 3, 4], 0.5) == 2.5 and report.percentile([7], 0.99) == 7)
    parsed = imaptestrun.parse_output(imaptestrun.SAMPLE_OUTPUT)
    check("imaptest output parsed", parsed["seconds"] == 4 and parsed["totals"]["Logi"] == 46
          and parsed["totals_source"] == "imaptest" and parsed["error_count"] == 1
          and parsed["clients_active_max"] == 10, json.dumps(parsed)[:200])
    factory = smtpload.MessageFactory("4k:1,64k:1", 1, "t", "a@b", "u{n}@c", 3)
    payload, rcpts, size = factory.build(5)
    check("message built", payload.endswith(b"\r\n.\r\n") and b"\r\n.\r\n" not in payload[:-5]
          and abs(len(payload) - size) < 1024 and rcpts == ["u3@c"], "%d bytes for %d" % (len(payload), size))
    fake = tempfile.mkdtemp(prefix="loadgen-cgroups-")
    cgroups = []
    for number, (current, anon, usec) in enumerate(((1000, 600, 2000000), (500, 300, 1000000))):
        directory = os.path.join(fake, str(number))
        os.mkdir(directory)
        for name, text in (("memory.current", "%d\n" % current), ("memory.stat", "file 5\nanon %d\n" % anon),
                           ("memory.events", "low 0\nmax 0\noom 0\n"), ("cpu.stat", "usage_usec %d\n" % usec),
                           ("cgroup.procs", "1\n2\n")):
            with open(os.path.join(directory, name), "w") as handle:
                handle.write(text)
        cgroups.append(directory)
    got = sampling.Sampler(cgroups=cgroups).sample()
    check("cgroups read and added up", got.get("cgroup_current_bytes") == 1500 and got.get("cgroup_anon_bytes") == 900
          and got.get("cpu_seconds") == 3.0 and got.get("processes") == 4, json.dumps(got))
    shutil.rmtree(fake, ignore_errors=True)
    info = machine.collect()
    check("machine collected", bool((info.get("cpu") or {}).get("logical_cores")) and bool(info.get("os")),
          "%s, %s" % ((info.get("cpu") or {}).get("model"), (info.get("os") or {}).get("pretty")))

    # The generator's IP range above every range holding its addresses: the
    # shipped "My computer" (127.0.0.1, priority 30) and "Internet" (10), not a
    # range elsewhere, nor this tool's own from a run before.
    import hmprepare
    shipped = [{"name": "My computer", "lower": "127.0.0.1", "upper": "127.0.0.1", "priority": 30},
               {"name": "Internet", "lower": "0.0.0.0", "upper": "255.255.255.255", "priority": 10},
               {"name": "Elsewhere", "lower": "10.0.0.0", "upper": "10.0.0.255", "priority": 500},
               {"name": "loadgen", "lower": "127.0.0.1", "upper": "127.0.0.1", "priority": 900},
               {"name": "IPv6", "lower": "::", "upper": "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", "priority": 700}]
    loopback = hmprepare.range_priority(shipped, "127.0.0.1", "127.0.0.1")
    lab = hmprepare.range_priority(shipped, "172.18.0.1", "172.18.0.1")
    check("prepare-hmailserver: the range's priority above the ranges holding its addresses",
          loopback[0] == 31 and [r["name"] for r in loopback[1]] == ["My computer", "Internet"]
          and lab[0] == 20 and hmprepare.range_priority([], "127.0.0.1", "127.0.0.1")[0] == 20,
          "127.0.0.1: %d, 172.18.0.1: %d" % (loopback[0], lab[0]))

    # The idle point of imap-sessions: a server still holding the last run's
    # sessions (7 October 2026, run 2: 526.4 MiB idle, 238.5 MiB at 25
    # sessions) voids the memory figure; one that had let them go does not.
    mib = 1048576
    check("imap-sessions: a busy idle point voids the memory per session",
          idle_point_problem(526.4 * mib, 238.5 * mib, 25, True) is not None
          and idle_point_problem(234.2 * mib, 258.0 * mib, 25, True) is None
          and idle_point_problem(234.2 * mib, 258.0 * mib, 25, False) is not None
          and idle_point_problem(None, 258.0 * mib, 25, None) is None)

    class Falling(object):
        """A sampler whose server gives memory back for three windows, then holds it."""
        interval = 0.01

        def __init__(self, figures):
            self.figures = list(figures)

        def window(self, start, end):
            figure = self.figures.pop(0) if len(self.figures) > 1 else self.figures[0]
            return {"private_bytes_mean": figure}

    window, taken = settle(Falling([526 * mib, 400 * mib, 260 * mib, 235 * mib, 234.5 * mib]), 60, span=0.01)
    still, still_taken = settle(Falling([526 * mib, 400 * mib, 300 * mib, 200 * mib, 100 * mib]), 0.03, span=0.02)
    at_once, at_once_taken = settle(Falling([526 * mib, 100 * mib]), 0, span=0.01)
    check("smtp-out: a sink the server reaches over the loopback",
          all(loopback_sink(h) for h in ("127.0.0.1", "0.0.0.0", "", "::1", "[::1]", "localhost", "127.0.0.2"))
          and not any(loopback_sink(h) for h in ("192.168.11.150", "10.0.0.20", "172.18.0.1")))
    check("imap-sessions: the idle point waits for the memory to stop falling",
          taken["settled"] is True and window["private_bytes_mean"] == 234.5 * mib and len(taken["figures"]) == 5
          and still_taken["settled"] is False and at_once_taken["settled"] is None and len(at_once_taken["figures"]) == 1,
          "%s; %s; %s" % (taken, still_taken, at_once_taken))

    work = tempfile.mkdtemp(prefix="loadgen-selftest-")
    smtp_port, imap_port, sink_port = free_port(), free_port(), free_port()
    env = dict(os.environ, HMLOAD_PASSWORD="selftest-password")
    me = [sys.executable, os.path.abspath(__file__)]
    toy = subprocess.Popen(me + ["toy-server", "--smtp-port", str(smtp_port), "--imap-port", str(imap_port),
                                 "--domain", "toy.test", "--relay", "127.0.0.1:%d" % sink_port],
                           env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        line = toy.stdout.readline().decode("utf-8", "replace")
        check("toy server started", "ready" in line, line.strip())
        common = ["--host", "127.0.0.1", "--processes", "2", "--server-pid", str(toy.pid), "--sample-interval", "0.5",
                  "--label", "self-test against the toy server", "--server", "toy server (self-test)"]

        def run(mode, extra):
            out = os.path.join(work, mode + ".json")
            markdown = os.path.join(work, mode + ".md")
            completed = subprocess.run(me + [mode] + common + extra + ["--json", out, "--markdown", markdown],
                                       env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
            text = completed.stdout.decode("utf-8", "replace")
            try:
                with open(out, "rb") as handle:
                    return completed.returncode, json.loads(handle.read().decode("utf-8")), text
            except (OSError, ValueError):
                return completed.returncode, None, text

        code, doc, text = run("smtp-in", ["--port", str(smtp_port), "--clients", "4", "--messages", "200",
                                          "--sizes", "2k:3,32k:1", "--rcpt-template", "load{n}@toy.test",
                                          "--mailboxes", "5", "--verify-imap-port", str(imap_port),
                                          "--auth-user", "load1@toy.test"])
        ok = doc is not None and doc["results"]["accepted"] == 200 and doc["results"]["delivered"]["complete"]
        check("smtp-in: 200 accepted and counted in the mailboxes", ok and code == 0,
              "%s/s accepted, %s/s delivered, p95 %s ms" % (doc["results"]["accepted_per_second"], doc["results"]["delivered_per_second"],
                                                          doc["results"]["latency_ms"].get("p95")) if doc else text[-600:])
        check("smtp-in: server memory sampled", bool(doc and doc["results"]["server"]["window"]),
              json.dumps(doc["results"]["server"]["sampler"]) if doc else "")

        code, doc, text = run("smtp-out", ["--port", str(smtp_port), "--clients", "4", "--messages", "200",
                                           "--sizes", "2k:1", "--rcpt-template", "sink{n}@sink.test",
                                           "--mail-from", "loadsend@toy.test", "--sink-listen", "127.0.0.1:%d" % sink_port,
                                           "--drain-timeout", "60"])
        ok = doc is not None and doc["results"]["sink"]["run_messages"] == 200
        check("smtp-out: 200 relayed and counted at the sink", ok and code == 0,
              "%s/s at the sink, end-to-end p95 %s ms" % (doc["results"]["relayed_per_second_at_sink"],
                                                         doc["results"]["end_to_end_latency_ms"].get("p95")) if doc else text[-600:])

        code, doc, text = run("imap-sessions", ["--port", str(imap_port), "--steps", "5,10", "--step-seconds", "4",
                                                "--settle-seconds", "1", "--think-ms", "200", "--idle-seconds", "0.5",
                                                "--user-template", "load{n}@toy.test", "--users", "5",
                                                "--latency-limit-ms", "500", "--idle-wait", "15"])
        ok = doc is not None and doc["results"]["best_sessions"] == 10 and all(s["operations"] > 0 for s in doc["results"]["steps"])
        ok = ok and doc["results"]["best_is_last_step"] is True and (doc["results"]["baseline_taken"] or {}).get("settled") is not None
        check("imap-sessions: two steps passed with operations recorded, the last step and the idle point said", ok and code == 0,
              "sessions per core %s, memory per session %s bytes (%s)" % (
                  doc["results"]["sessions_per_core"], doc["results"]["memory_per_session_bytes"],
                  doc["results"]["memory_figure"]) if doc else text[-600:])
        if doc:
            ops = set()
            for step in doc["results"]["steps"]:
                ops.update(step["latency_by_operation_ms"].keys())
            check("imap-sessions: every operation exercised", ops >= set(imapload.OPERATIONS), ", ".join(sorted(ops)))

        # prepare-hmailserver against a fake REST listener that records the calls.
        import http.server
        import threading
        calls = []

        class Fake(http.server.BaseHTTPRequestHandler):
            def log_message(self, *arguments):
                pass

            def answer(self, status, body):
                data = json.dumps(body).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def handle_any(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(length).decode("utf-8")) if length else None
                calls.append((self.command, self.path, body, self.headers.get("Authorization", "")))
                if self.command == "GET" and self.path == "/api/v1/routes":
                    self.answer(200, {"routes": [{"id": 7, "domain_name": "sink.test"}, {"id": 8, "domain_name": "other.test"}]})
                elif self.command == "GET" and self.path == "/api/v1/ipranges":
                    self.answer(200, [{"id": 3, "name": "loadgen", "lower": "172.18.0.1", "upper": "172.18.0.1", "priority": 99},
                                      {"id": 1, "name": "Internet", "lower": "0.0.0.0", "upper": "255.255.255.255", "priority": 10},
                                      {"id": 4, "name": "Lab network", "lower": "172.18.0.0", "upper": "172.18.255.255", "priority": 30}])
                elif self.command == "GET":
                    self.answer(200, {"name": "MessageStoreFsync", "value": "1"})
                else:
                    self.answer(201 if self.command == "POST" else 200, {"ok": True})
            do_GET = do_POST = do_PUT = do_DELETE = handle_any

        fake_rest = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fake)
        threading.Thread(target=fake_rest.serve_forever, daemon=True).start()
        try:
            completed = subprocess.run(me + ["prepare-hmailserver", "--rest", "http://127.0.0.1:%d" % fake_rest.server_address[1],
                                             "--accounts", "3", "--route", "sink.test=172.18.0.1:10365",
                                             "--client-range", "172.18.0.1-172.18.0.1", "--fsync", "on"],
                                       env=dict(env, HMLOAD_ADMIN_PASSWORD="admin-secret"),
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        finally:
            fake_rest.shutdown()
        made = [c for c in calls if c[0] == "POST" and c[1] == "/api/v1/domains/example.test/accounts"]
        wanted = [
            ("DELETE", "/api/v1/routes/7"), ("DELETE", "/api/v1/ipranges/3"),
            ("PUT", "/api/v1/settings/ini/MessageStoreFsync"), ("POST", "/api/v1/routes"), ("POST", "/api/v1/ipranges"),
        ]
        seen = [(c[0], c[1]) for c in calls]
        ranges = [c[2] for c in calls if c[:2] == ("POST", "/api/v1/ipranges")]
        check("prepare-hmailserver: the REST calls", completed.returncode == 0 and len(made) == 3
              and all(w in seen for w in wanted) and ("DELETE", "/api/v1/routes/8") not in seen
              and ranges and ranges[0]["lower"] == "172.18.0.1" and ranges[0]["spam_protection"] is False
              and ranges[0]["priority"] == 31
              and made[0][2]["password"] == "selftest-password" and all(c[3].startswith("Basic ") for c in calls),
              "%d calls" % len(calls) if completed.returncode == 0 else completed.stdout.decode("utf-8", "replace")[-400:])

        completed = subprocess.run(me + ["imaptest", "--host", "hmailserver", "--port", "1143", "--dry-run",
                                         "--image", "dovecot/imaptest:pinned", "--docker", "--network", "hmlab",
                                         "--mbox", os.path.abspath(__file__)],
                                   env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        text = completed.stdout.decode("utf-8", "replace")
        check("imaptest: command built", completed.returncode == 0 and "userfile=/u/users" in text
              and "no_tracking" in text and "selftest-password" not in text, text.strip()[:200])
    finally:
        toy.terminate()
        try:
            toy.wait(timeout=10)
        except subprocess.TimeoutExpired:
            toy.kill()
    say("results and tables in %s" % work)
    say("%d failed" % len(failures) if failures else "all passed")
    return len(failures)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog="See the module's docstring and hmailserver/docs/PublishedNumbers.md.")
    commands = parser.add_subparsers(dest="command")

    p = commands.add_parser("smtp-in", help="deliver to local mailboxes")
    add_common(p, 25)
    add_smtp(p, "load{n}@example.test", "loadgen@loadgen.invalid")
    p.add_argument("--verify-imap-port", type=int, default=0, help="count delivery by IMAP STATUS on this port")
    p.add_argument("--verify-imap-host", default="", help="(default --host)")
    p.add_argument("--verify-tls", choices=("none", "starttls", "implicit"), default="none")
    p.add_argument("--drain-timeout", type=int, default=600, help="seconds to wait for delivery (default 600)")
    p.set_defaults(func=cmd_smtp_in)

    p = commands.add_parser("smtp-out", help="relay to this tool's sink")
    add_common(p, 25)
    add_smtp(p, "sink{n}@sink.test", "loadsend@example.test")
    p.add_argument("--sink-listen", default="0.0.0.0:10365", help="where the sink listens (default 0.0.0.0:10365)")
    p.add_argument("--drain-timeout", type=int, default=600, help="seconds to wait for the sink to have them all")
    p.set_defaults(func=cmd_smtp_out)

    p = commands.add_parser("imap-sessions", help="sessions per core under a latency limit, memory per session")
    add_common(p, 143)
    p.add_argument("--steps", default="25,50,100,200,400,800,1600", help="session counts, in order")
    p.add_argument("--step-seconds", type=float, default=30.0, help="the measured window of each step")
    p.add_argument("--settle-seconds", type=float, default=5.0, help="between a step's last login and its window")
    p.add_argument("--ops", default=DEFAULT_OPS, help="operation:weight,... (default %s)" % DEFAULT_OPS)
    p.add_argument("--think-ms", type=float, default=1000.0, help="a session's mean pause between operations")
    p.add_argument("--idle-seconds", type=float, default=5.0, help="how long an IDLE lasts before DONE")
    p.add_argument("--user-template", default="load{n}@example.test")
    p.add_argument("--users", type=int, default=10, help="sessions are spread over this many users")
    p.add_argument("--connect-rate", type=float, default=50.0, help="new sessions a second, across the processes")
    p.add_argument("--latency-limit-ms", type=float, default=200.0)
    p.add_argument("--latency-percentile", type=float, default=95.0)
    p.add_argument("--max-error-rate", type=float, default=0.0)
    p.add_argument("--server-cores", type=int, default=0, help="the server's logical cores (default: from the machine details)")
    p.add_argument("--keep-going", action="store_true", help="run every step even after one fails")
    p.add_argument("--idle-wait", type=float, default=120.0,
                   help="seconds the idle server's memory may take to stop falling before it is sampled (default 120; 0 takes it at once)")
    p.set_defaults(func=cmd_imap_sessions)

    p = commands.add_parser("imaptest", help="Dovecot's imaptest in a benchmark profile")
    add_common(p, 143)
    p.add_argument("--profile", choices=sorted(imaptestrun.PROFILES), default="benchmark")
    p.add_argument("--clients", type=int, default=50)
    p.add_argument("--seconds", type=int, default=60)
    p.add_argument("--user-template", default="load{n}@example.test")
    p.add_argument("--users", type=int, default=10)
    p.add_argument("--mbox", default="", help="the mbox imaptest APPENDs from (imaptest's src/tests/default.mbox)")
    p.add_argument("--imaptest-binary", default="", help="imaptest's path (default: on the PATH, else docker)")
    p.add_argument("--docker", action="store_true", help="run the image even where imaptest is on the PATH")
    p.add_argument("--image", default="", help="the image (default: IMAPTEST_IMAGE from build/lab/lab.sh)")
    p.add_argument("--network", default="", help="docker network to run on (the lab's: hmlab)")
    p.add_argument("--imaptest-arg", action="append", default=[], help="one more imaptest argument (repeatable)")
    p.add_argument("--log", default="", help="write imaptest's whole output here")
    p.add_argument("--dry-run", action="store_true", help="print the command and stop")
    p.set_defaults(func=cmd_imaptest)

    p = commands.add_parser("sink", help="the SMTP sink alone")
    p.add_argument("--listen", default="0.0.0.0:10365")
    p.add_argument("--run", default="", help="count only messages carrying this X-Loadgen-Run")
    p.add_argument("--seconds", type=int, default=0, help="stop after this long (default: at Ctrl-C)")
    p.add_argument("--json", default="")
    p.set_defaults(func=cmd_sink)

    p = commands.add_parser("machine", help="this machine's details, as JSON")
    p.add_argument("--data-path", default="")
    p.add_argument("--json", default="")
    p.set_defaults(func=cmd_machine)

    p = commands.add_parser("prepare-hmailserver", help="accounts, route, IP range and fsync over REST")
    p.add_argument("--rest", default="http://127.0.0.1:8045", help="the REST listener (default http://127.0.0.1:8045)")
    p.add_argument("--insecure", action="store_true")
    p.add_argument("--domain", default="example.test")
    p.add_argument("--account-template", default="load{n}@example.test")
    p.add_argument("--accounts", type=int, default=10)
    p.add_argument("--route", default="", help="DOMAIN=HOST:PORT, e.g. sink.test=172.18.0.1:10365")
    p.add_argument("--client-range", default="", help="LOW-HIGH addresses the generator connects from")
    p.add_argument("--fsync", choices=("on", "off"), default="", help="set MessageStoreFsync")
    p.add_argument("--cleanup", action="store_true", help="delete what a prepare made")
    p.set_defaults(func=cmd_prepare)

    p = commands.add_parser("toy-server", help="the self-test's toy server, not a mail server")
    p.add_argument("--listen", default="127.0.0.1")
    p.add_argument("--smtp-port", type=int, required=True)
    p.add_argument("--imap-port", type=int, required=True)
    p.add_argument("--domain", default="toy.test")
    p.add_argument("--relay", default="")
    p.set_defaults(func=cmd_toy_server)

    p = commands.add_parser("self-test", help="every mode against a toy server, at low load")
    p.set_defaults(func=cmd_self_test)

    args = parser.parse_args(argv)
    if not getattr(args, "func", None):
        parser.print_help()
        return 2
    return args.func(args)


if __name__ == "__main__":
    multiprocessing.freeze_support()
    sys.exit(main())
