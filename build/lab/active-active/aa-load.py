#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The active-active lab's load and its checks (aa-lab.sh load / check).

Runs in a container on the lab's front network, beside the nodes, the balancer and
the sink. Python's standard library only.

  send        N messages over SMTP, spread over the hosts named (a node each, or
              the balancer), each with Message-ID <TAG.i@load.test> and X-AA-Seq:
              TAG.i; what each host answered is written to --out, one JSON line a
              message. --proxy opens each connection with a PROXY v1 header, which
              a node wants from the front network (node.sh trusts it); the
              balancer adds its own, so a load through it goes without
  check-sink  every recipient of every message --sent says was accepted, against
              the sink's log: delivered once, delivered more than once, or not at
              all. The two numbers the design is held to are the second and the
              third
  imap        for an account, on every host named: the UIDs of a folder - unique,
              ascending, as many as expected - whether each one can be fetched,
              and whether what comes back is the message the load sent (its
              X-AA-Seq field) and not a placeholder

The account's password comes from the environment, AA_ACCOUNT_PASSWORD, never from
the command line. Exit 0 when the check found nothing wrong, 1 when it did.
"""

import argparse
import imaplib
import json
import smtplib
import sys
import threading
import time
from email.utils import formatdate


def parse_hosts(text):
    hosts = []
    for item in text.split(","):
        host, _, port = item.strip().rpartition(":")
        hosts.append((host, int(port)))
    return hosts


class ProxySMTP(smtplib.SMTP):
    """SMTP that opens with a PROXY protocol v1 header, as the balancer does.

    A node trusts the lab's front network for the PROXY protocol (node.sh), and
    a listener that trusts a peer waits for the header before it greets: a load
    sent straight to a node without one is never answered. The header names the
    load's own address, so the node sees the client it would see behind the
    balancer.
    """

    def _get_socket(self, host, port, timeout):
        sock = super()._get_socket(host, port, timeout)
        source, source_port = sock.getsockname()[:2]
        target, target_port = sock.getpeername()[:2]
        sock.sendall(("PROXY TCP4 %s %s %d %d\r\n" % (source, target, source_port, target_port)).encode())
        return sock


def cmd_send(args):
    hosts = parse_hosts(args.hosts)
    rcpts = [r.strip() for r in args.to.split(",") if r.strip()]
    lock = threading.Lock()
    out = open(args.out, "a", encoding="utf-8")
    next_index = {"i": 0}
    stats = {"accepted": 0, "refused": 0, "errors": 0}

    def worker():
        connections = {}
        while True:
            with lock:
                i = next_index["i"]
                if i >= args.count:
                    break
                next_index["i"] += 1
            host = hosts[i % len(hosts)]
            seq = "%s.%d" % (args.tag, i)
            body = ("From: <load@load.test>\r\nTo: %s\r\nSubject: active-active lab %s\r\nDate: %s\r\n"
                    "Message-ID: <%s@load.test>\r\nX-AA-Seq: %s\r\n\r\nmessage %s, sent to %s:%d\r\n"
                    % (", ".join("<%s>" % r for r in rcpts), seq, formatdate(), seq, seq, seq, host[0], host[1]))
            entry = {"seq": seq, "host": "%s:%d" % host, "rcpts": rcpts}
            try:
                smtp = connections.get(host)
                if smtp is None:
                    smtp = (ProxySMTP if args.proxy else smtplib.SMTP)(host[0], host[1], timeout=60)
                    smtp.ehlo("load.test")
                    connections[host] = smtp
                refused = smtp.sendmail("load@load.test", rcpts, body.encode())
                entry["refused"] = sorted(refused.keys())
                entry["code"] = 250
                with lock:
                    stats["accepted"] += 1
            except smtplib.SMTPException as e:
                entry["error"] = str(e)[:200]
                connections.pop(host, None)
                with lock:
                    stats["refused"] += 1
            except OSError as e:
                entry["error"] = str(e)[:200]
                connections.pop(host, None)
                with lock:
                    stats["errors"] += 1
            with lock:
                out.write(json.dumps(entry) + "\n")
                out.flush()
        for smtp in connections.values():
            try:
                smtp.quit()
            except (smtplib.SMTPException, OSError):
                pass

    started = time.time()
    threads = [threading.Thread(target=worker) for _ in range(args.threads)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    out.close()
    stats["seconds"] = round(time.time() - started, 1)
    print(json.dumps({"send": args.tag, **stats}))
    return 0


def cmd_check_sink(args):
    sent = {}
    for line in open(args.sent, encoding="utf-8"):
        entry = json.loads(line)
        if entry.get("code") != 250:
            continue
        for rcpt in entry["rcpts"]:
            if rcpt not in entry.get("refused", []):
                sent[(entry["seq"], rcpt.lower())] = 0
    seen = 0
    try:
        for line in open(args.sink, encoding="utf-8"):
            entry = json.loads(line)
            for rcpt in entry["rcpts"]:
                key = (entry["seq"], rcpt.lower())
                if key in sent:
                    sent[key] += 1
                    seen += 1
    except FileNotFoundError:
        pass
    once = sum(1 for n in sent.values() if n == 1)
    twice = sum(1 for n in sent.values() if n > 1)
    missing = sum(1 for n in sent.values() if n == 0)
    summary = {"check": "sink", "accepted_pairs": len(sent), "delivered_once": once,
               "delivered_more_than_once": twice, "extra_copies": sum(n - 1 for n in sent.values() if n > 1),
               "not_delivered": missing}
    if args.show:
        summary["examples_twice"] = [k[0] for k, n in sent.items() if n > 1][:5]
        summary["examples_missing"] = [k[0] for k, n in sent.items() if n == 0][:5]
    print(json.dumps(summary))
    return 0 if twice == 0 and missing == 0 else 1


def cmd_imap(args):
    import os
    password = os.environ.get("AA_ACCOUNT_PASSWORD", "")
    failed = False
    for host, port in parse_hosts(args.hosts):
        report = {"check": "imap", "host": "%s:%d" % (host, port), "account": args.user}
        try:
            imap = imaplib.IMAP4(host, port, timeout=60)
            imap.login(args.user, password)
            typ, data = imap.select(args.folder, readonly=True)
            report["exists"] = int(data[0]) if typ == "OK" else None
            typ, data = imap.uid("SEARCH", None, "ALL")
            uids = [int(u) for u in data[0].split()] if typ == "OK" and data[0] else []
            typ, data = imap.status(args.folder, "(UIDNEXT MESSAGES)")
            report["status"] = data[0].decode(errors="replace") if typ == "OK" else None
            report["uids"] = len(uids)
            report["duplicate_uids"] = len(uids) - len(set(uids))
            report["ascending"] = uids == sorted(uids)
            # Unreadable: the FETCH failed. Without the load's header: the server
            # answered with something other than the message the load sent - with
            # the file system backend, the placeholder a node writes under the
            # name of a file it cannot find (PersistentMessage::EnsureFileExistance,
            # HM5026), which is what a node serves for a message whose file is on
            # the other node's disk.
            unreadable = 0
            not_the_message = 0
            for uid in uids:
                typ, data = imap.uid("FETCH", str(uid), "(RFC822.SIZE BODY.PEEK[HEADER.FIELDS (X-AA-SEQ)])")
                if typ != "OK" or not data or data[0] is None:
                    unreadable += 1
                    continue
                header = data[0][1] if isinstance(data[0], tuple) and len(data[0]) > 1 else b""
                if b"x-aa-seq:" not in (header or b"").lower():
                    not_the_message += 1
            report["unreadable"] = unreadable
            report["not_the_message"] = not_the_message
            imap.logout()
            if args.expect is not None and len(uids) != args.expect:
                failed = True
            if report["duplicate_uids"] or not report["ascending"] or unreadable or not_the_message:
                failed = True
        except (imaplib.IMAP4.error, OSError) as e:
            report["error"] = str(e)[:200]
            failed = True
        print(json.dumps(report))
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    send = sub.add_parser("send")
    send.add_argument("--hosts", required=True, help="host:port[,host:port...]")
    send.add_argument("--count", type=int, default=1000)
    send.add_argument("--to", required=True, help="recipient[,recipient...]")
    send.add_argument("--threads", type=int, default=8)
    send.add_argument("--tag", required=True)
    send.add_argument("--out", required=True)
    send.add_argument("--proxy", action="store_true",
                      help="open each connection with a PROXY v1 header (straight to a node; not through the balancer)")
    check = sub.add_parser("check-sink")
    check.add_argument("--sent", required=True)
    check.add_argument("--sink", required=True)
    check.add_argument("--show", action="store_true")
    imap = sub.add_parser("imap")
    imap.add_argument("--hosts", required=True)
    imap.add_argument("--user", required=True)
    imap.add_argument("--folder", default="INBOX")
    imap.add_argument("--expect", type=int)
    args = parser.parse_args()
    return {"send": cmd_send, "check-sink": cmd_check_sink, "imap": cmd_imap}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
