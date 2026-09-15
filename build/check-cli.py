#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""
Runs the two command-line clients against a recorded server and checks what they
sent and what they printed.

The clients - `hmailserver/source/Tools/Cli/hmctl` (Python, any platform) and
`hmailserver/source/Tools/Cli/HmailServer.psm1` (PowerShell) - are the REST API
with a vocabulary in front of it, so the thing that can go wrong is that the
vocabulary points at the wrong route: `hmctl account list` asking
`/api/v1/accounts?domain=x` when the API lists accounts under their domain. A
real server would answer 404 and a person would find out; a check that needs a
real server is a check nobody runs.

So this stands a small HTTP server in front of both, answers whatever the route
would answer, and asserts on the request that arrived: the method, the path, the
Authorization header and the body. Every case also asserts something the client
printed, because a client that sends the right request and prints nothing useful
is no better.

The PowerShell half is skipped, with a reason, where `pwsh` is not on the path.
Both halves are skipped if the file they test is missing, which cannot happen in
a checkout and can happen in a partial copy.

Usage:
  python build/check-cli.py            # both clients
  python build/check-cli.py --python   # hmctl only
  python build/check-cli.py --verbose  # print every request that arrived
"""

import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HMCTL = os.path.join(ROOT, "hmailserver", "source", "Tools", "Cli", "hmctl")
MODULE = os.path.join(ROOT, "hmailserver", "source", "Tools", "Cli", "HmailServer.psm1")

PASSWORD = "bench-password"
EXPECTED_BASIC = "Basic " + base64.b64encode(("Administrator:" + PASSWORD).encode("utf-8")).decode("ascii")

# A domain's DKIM key rotation part way through: the current key kept, the next key
# published and propagating, an old selector retired whose record may go. The
# shape DkimRotation::StatusJson writes.
ROTATION_STATUS = {
    "domain": "example.com", "now": 1789000000,
    "policy": {"enabled": True, "interval_days": 180, "propagation_hours": 72, "retention_days": 7, "key_bits": 2048},
    "phase": "propagating",
    "current": {"selector": "s20260315", "key_file": "/var/lib/hmailserver/DKIM/s20260315._domainkey.example.com.pem",
                "generated": True, "since": 1773532800, "next_rotation_due": 1789084800,
                "records": [{"host": "s20260315._domainkey.example.com", "type": "TXT",
                             "value": "v=DKIM1; k=rsa; p=CURRENTKEY", "action": "keep"}],
                "check": {"status": "matches", "checked": 1789000000, "detail": ""}},
    "next": {"selector": "s20260915", "key_file": "/var/lib/hmailserver/DKIM/s20260915._domainkey.example.com.pem",
             "generated": True, "created": 1788900000, "first_seen": 1788950000, "switch_due": 1789209200,
             "records": [{"host": "s20260915._domainkey.example.com", "type": "TXT",
                          "value": "v=DKIM1; k=rsa; p=NEXTKEY", "action": "publish"}],
             "check": {"status": "matches", "checked": 1789000000, "detail": ""}},
    "retiring": None,
    "retired": {"selector": "s20250915", "at": 1760000000, "key_file_kept": "",
                "records": [{"host": "s20250915._domainkey.example.com", "type": "TXT", "action": "remove"}]},
    "waiting": [{"step": "switch", "reason": "propagation", "until": 1789209200,
                 "detail": "The record is propagating."}],
    "last_error": None,
}

# What the recorded server answers, by (method, path). A path not here is a 404,
# which is what an unknown route deserves and what a wrong one will get.
ANSWERS = {
    ("GET", "/api/v1/status"): {"version": "6.3.3", "database": "PostgreSQL", "uptime_seconds": 4242},
    ("GET", "/api/v1/domains"): {"domains": [
        {"name": "example.com", "active": True, "max_size_mb": 0, "account_count": 2,
         "dkim_rotation_enabled": True, "dkim_selector": "s20260915",
         "dkim_private_key_file": "/var/lib/hmailserver/DKIM/s20260915._domainkey.example.com.pem",
         "dkim_secondary_selector": "", "dkim_secondary_private_key_file": ""},
        {"name": "example.net", "active": False, "max_size_mb": 100, "account_count": 0},
    ]},
    ("GET", "/api/v1/domains/example.com"): {"name": "example.com", "active": True, "plus_addressing_enabled": True},
    ("POST", "/api/v1/domains"): {"name": "new.example", "active": True},
    ("PUT", "/api/v1/domains/example.com"): {"name": "example.com", "active": False},
    ("DELETE", "/api/v1/domains/example.net"): None,
    ("GET", "/api/v1/domains/example.com/accounts"): {"accounts": [
        {"address": "ann@example.com", "active": True, "max_size_mb": 100, "admin_level": 0},
        {"address": "bob@example.com", "active": True, "max_size_mb": 0, "admin_level": 0},
    ]},
    ("POST", "/api/v1/domains/example.com/accounts"): {"address": "new@example.com", "active": True},
    ("GET", "/api/v1/accounts/ann@example.com"): {"address": "ann@example.com", "active": True, "vacation_message": ""},
    ("PUT", "/api/v1/accounts/ann@example.com"): {"address": "ann@example.com", "active": False},
    ("DELETE", "/api/v1/accounts/bob@example.com"): None,
    ("GET", "/api/v1/domains/example.com/aliases"): {"aliases": [
        {"name": "sales@example.com", "value": "ann@example.com", "active": True}]},
    ("GET", "/api/v1/groups"): {"groups": [{"id": 1, "name": "support"}]},
    ("GET", "/api/v1/groups/1/members"): {"members": [{"id": 7, "account_id": 3, "address": "ann@example.com"}]},
    ("POST", "/api/v1/groups/1/members"): {"id": 8, "account_id": 4, "address": "bob@example.com"},
    ("GET", "/api/v1/queue"): {"messages": [
        {"id": 12, "from_address": "ann@example.com", "to_address": "far@away.test",
         "next_try": "2026-09-15T05:00:00Z", "retry_count": 1, "size": 2048}]},
    ("POST", "/api/v1/queue/12/retry"): {"queued": True},
    ("GET", "/api/v1/quarantine"): {"messages": [
        {"id": 3, "recipient": "ann@example.com", "from_address": "spam@away.test",
         "subject": "Held", "reason": "virus", "received": "2026-09-15T04:00:00Z"}]},
    ("POST", "/api/v1/quarantine/3/release"): {"released": True},
    ("GET", "/api/v1/settings"): {"welcome_smtp": "hMailServer", "max_message_size_kb": 20480},
    ("PUT", "/api/v1/settings"): {"max_message_size_kb": 51200},
    ("GET", "/api/v1/settings/antispam"): {"use_spf": True, "spam_mark_threshold": 5},
    ("PUT", "/api/v1/settings/antispam"): {"use_spf": False},
    ("GET", "/api/v1/settings/ini"): {"settings": [{"name": "DisableAUTHList", "value": "0"}]},
    ("PUT", "/api/v1/settings/ini/DisableAUTHList"): {"name": "DisableAUTHList", "value": "1"},
    ("GET", "/api/v1/logs"): {"logs": [{"name": "hmailserver_2026-09-15.log", "size": 91234}]},
    ("GET", "/api/v1/logs/hmailserver_2026-09-15.log"): {"lines": ["\"SMTPD\" 1 \"04:00:00.001\" \"127.0.0.1\" \"EHLO\""]},
    ("GET", "/api/v1/backup"): {"state": "idle", "last_backup": "2026-09-14T02:00:00Z"},
    ("POST", "/api/v1/backup"): {"state": "running"},
    ("POST", "/api/v1/rules/match"): {"match": True},
    ("GET", "/api/v1/accounts/ann@example.com/app-passwords"): {"app_passwords": [
        {"id": 2, "name": "phone", "created": "2026-09-01T10:00:00Z", "last_used": None}]},
    ("POST", "/api/v1/accounts/ann@example.com/app-passwords"): {"id": 3, "name": "tablet", "password": "abcd-efgh-ijkl"},
    ("GET", "/api/v1/rules"): {"rules": [{"name": "Tag support", "active": True, "sort_order": 1}]},
    ("GET", "/api/v1/routes"): {"routes": [{"domain_name": "partner.test", "target_smtp_host": "mx.partner.test",
                                            "target_smtp_port": 25}]},
    ("GET", "/api/v1/ports"): {"ports": [{"protocol": "SMTP", "address": "0.0.0.0", "port": 25,
                                          "connection_security": 0}]},
    ("GET", "/api/v1/domains/example.com/lists"): {"lists": [{"address": "all@example.com", "active": True,
                                                              "mode": 0}]},
    ("GET", "/api/v1/domains/example.net/accounts"): {"accounts": []},
    ("GET", "/api/v1/domains/example.net/aliases"): {"aliases": []},
    ("GET", "/api/v1/domains/example.net/lists"): {"lists": []},
    ("PUT", "/api/v1/domains/example.net"): {"name": "example.net", "active": True},
    ("GET", "/api/v1/ipranges"): {"ipranges": [
        {"id": 1, "name": "My computer", "lower_ip": "127.0.0.1", "upper_ip": "127.0.0.1", "priority": 15}]},
    ("GET", "/api/v1/reports"): {
        "from": "2026-08-17", "to": "2026-09-15", "today": "2026-09-15",
        "sections": [
            {"name": "traffic", "scope": "domain", "source": "hm_messagetrace", "csv": True,
             "summary": "Messages in and out, per day and per local domain."},
            {"name": "storage", "scope": "server", "source": "hm_metricsamples", "csv": True,
             "summary": "The size of the message store per day."}],
        "sources": {
            "message_trace": {"table": "hm_messagetrace", "enabled": False, "retention_days": 30, "oldest": ""},
            "metric_history": {"table": "hm_metricsamples", "enabled": True, "retention_days": 7,
                               "oldest": "2026-09-08 00:00:00"}},
        "not_answerable": ["Spam and virus counts per domain."]},
    ("GET", "/api/v1/reports/traffic"): {
        "from": "2026-09-01", "to": "2026-09-15", "domain": "example.com", "top": 10,
        "section": "traffic", "source": "hm_messagetrace", "enabled": True, "truncated": False,
        "note": "Counted from the message trace.",
        "columns": ["day", "domain", "incoming", "outgoing", "failed_incoming", "failed_outgoing"],
        "rows": [{"day": "2026-09-14", "domain": "example.com", "incoming": 12, "outgoing": 3,
                  "failed_incoming": 1, "failed_outgoing": 0}]},
    ("GET", "/api/v1/reports/summary"): {
        "section": "summary", "from": "2026-08-17", "to": "2026-09-15", "domain": "", "top": 10,
        "sections": {"mailboxes": {"section": "mailboxes", "columns": ["address", "megabytes"],
                                   "rows": [{"address": "ann@example.com", "megabytes": 40}]}},
        "omitted": [{"section": "volume", "reason": "counted from server-wide metrics that carry no domain"}]},
    ("GET", "/api/v1/domains/example.com/dkim/rotation"): ROTATION_STATUS,
    ("POST", "/api/v1/domains/example.com/dkim/rotation/start"): ROTATION_STATUS,
    ("POST", "/api/v1/domains/example.com/dkim/rotation/check"): ROTATION_STATUS,
    ("POST", "/api/v1/domains/example.com/dkim/rotation/switch"): ROTATION_STATUS,
    ("POST", "/api/v1/domains/example.com/dkim/rotation/retire"): ROTATION_STATUS,
    ("DELETE", "/api/v1/domains/example.com/dkim/rotation/next"): ROTATION_STATUS,
    ("POST", "/api/v1/dkim/rotation/run"): {"as_of": 1789000000, "domains": 2, "generated": 0, "checked": 1,
                                            "switched": 1, "retired": 0, "errors": 0},
}

# Answers that are refusals, by (method, path): the status code and the body.
REFUSALS = {
    ("POST", "/api/v1/domains/example.net/dkim/rotation/switch"): (409, {
        "error": "The next key's record has not resolved for the propagation period yet.",
        "reason": "propagation",
        "status": dict(ROTATION_STATUS, domain="example.net")}),
}

# The one route that answers something other than JSON. Kept apart from ANSWERS
# because it is decided by the query string, which ANSWERS is keyed without:
# the point of the case is that the client ASKS for format=csv.
REPORT_CSV = ("day,domain,incoming,outgoing\r\n"
              "2026-09-14,example.com,12,3\r\n")


class Recorder(BaseHTTPRequestHandler):
    requests = []
    lock = threading.Lock()

    def log_message(self, *arguments):
        pass

    def _handle(self, method):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        body = None
        if raw:
            try:
                body = json.loads(raw.decode("utf-8"))
            except ValueError:
                body = raw.decode("utf-8", "replace")

        with Recorder.lock:
            Recorder.requests.append({
                "method": method,
                "path": self.path,
                "authorization": self.headers.get("Authorization"),
                "content_type": self.headers.get("Content-Type"),
                "body": body,
            })

        if self.headers.get("Authorization") is None:
            self.send_error(401, "no credential")
            return

        path = self.path.split("?", 1)[0]
        query = self.path.split("?", 1)[1] if "?" in self.path else ""
        import urllib.parse as parse
        path = parse.unquote(path)

        if path.startswith("/api/v1/reports/") and "format=csv" in query:
            payload = REPORT_CSV.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/csv; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        if (method, path) in REFUSALS:
            code, refusal = REFUSALS[(method, path)]
            payload = json.dumps(refusal).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        if (method, path) not in ANSWERS:
            payload = json.dumps({"error": "no such route: %s %s" % (method, path)}).encode("utf-8")
            self.send_response(404)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        answer = ANSWERS[(method, path)]
        if answer is None:
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        payload = json.dumps(answer).encode("utf-8")
        self.send_response(201 if method == "POST" else 200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")

    def do_PUT(self):
        self._handle("PUT")

    def do_DELETE(self):
        self._handle("DELETE")


PROBLEMS = []
CHECKS = [0]


def problem(message):
    PROBLEMS.append(message)


def check(condition, message):
    CHECKS[0] += 1
    if not condition:
        problem(message)
    return condition


# (name, arguments, expected method, expected path, body assertion, output assertion)
CASES = [
    ("status", ["status"], "GET", "/api/v1/status", None, ["6.3.3", "4242"]),
    ("domain list", ["domain", "list"], "GET", "/api/v1/domains", None, ["example.com", "example.net", "NAME"]),
    ("domain list --json", ["domain", "list", "--json"], "GET", "/api/v1/domains", None, ['"name"']),
    ("domain get", ["domain", "get", "example.com"], "GET", "/api/v1/domains/example.com", None, ["plus_addressing_enabled"]),
    ("domain create", ["domain", "create", "--set", "name=new.example", "--set", "active=true"],
     "POST", "/api/v1/domains", {"name": "new.example", "active": True}, ["new.example"]),
    ("domain update", ["domain", "update", "example.com", "--set", "active=false"],
     "PUT", "/api/v1/domains/example.com", {"active": False}, []),
    ("domain delete", ["domain", "delete", "example.net"], "DELETE", "/api/v1/domains/example.net", None, ["deleted"]),
    ("account list under its domain", ["account", "list", "--domain", "example.com"],
     "GET", "/api/v1/domains/example.com/accounts", None, ["ann@example.com", "bob@example.com"]),
    ("account create under its domain",
     ["account", "create", "--domain", "example.com", "--set", "address=new@example.com", "--set", "password=secret"],
     "POST", "/api/v1/domains/example.com/accounts", {"address": "new@example.com", "password": "secret"}, ["new@example.com"]),
    ("account get by address", ["account", "get", "ann@example.com"], "GET", "/api/v1/accounts/ann@example.com", None, ["vacation_message"]),
    ("account update by address", ["account", "update", "ann@example.com", "--set", "active=false"],
     "PUT", "/api/v1/accounts/ann@example.com", {"active": False}, []),
    ("account delete by address", ["account", "delete", "bob@example.com"], "DELETE", "/api/v1/accounts/bob@example.com", None, ["deleted"]),
    ("alias list under its domain", ["alias", "list", "--domain", "example.com"],
     "GET", "/api/v1/domains/example.com/aliases", None, ["sales@example.com"]),
    ("group list", ["group", "list"], "GET", "/api/v1/groups", None, ["support"]),
    ("group members", ["member", "1", "list"], "GET", "/api/v1/groups/1/members", None, ["ann@example.com"]),
    ("group member add by address", ["member", "1", "add", "bob@example.com"],
     "POST", "/api/v1/groups/1/members", {"address": "bob@example.com"}, ["bob@example.com"]),
    ("queue list", ["queue", "list"], "GET", "/api/v1/queue", None, ["far@away.test"]),
    ("queue retry", ["queue", "retry", "12"], "POST", "/api/v1/queue/12/retry", None, ["queued"]),
    ("quarantine list", ["quarantine", "list"], "GET", "/api/v1/quarantine", None, ["spam@away.test"]),
    ("quarantine release", ["quarantine", "release", "3"], "POST", "/api/v1/quarantine/3/release", None, ["released"]),
    ("settings get", ["settings", "get"], "GET", "/api/v1/settings", None, ["max_message_size_kb"]),
    ("settings get one", ["settings", "get", "settings", "max_message_size_kb"], "GET", "/api/v1/settings", None, ["20480"]),
    ("settings get a group", ["settings", "get", "antispam"], "GET", "/api/v1/settings/antispam", None, ["use_spf"]),
    ("settings set", ["settings", "set", "--set", "max_message_size_kb=51200"],
     "PUT", "/api/v1/settings", {"max_message_size_kb": 51200}, []),
    ("settings set a group", ["settings", "set", "antispam", "--set", "use_spf=false"],
     "PUT", "/api/v1/settings/antispam", {"use_spf": False}, []),
    ("ini list", ["ini", "list"], "GET", "/api/v1/settings/ini", None, ["DisableAUTHList"]),
    ("ini set", ["ini", "set", "DisableAUTHList", "1"],
     "PUT", "/api/v1/settings/ini/DisableAUTHList", {"value": "1"}, []),
    ("log list", ["log", "list"], "GET", "/api/v1/logs", None, ["hmailserver_2026-09-15.log"]),
    ("log tail", ["log", "tail", "hmailserver_2026-09-15.log", "--lines", "5"],
     "GET", "/api/v1/logs/hmailserver_2026-09-15.log", None, ["SMTPD"]),
    ("backup status", ["backup", "status"], "GET", "/api/v1/backup", None, ["idle"]),
    ("backup start", ["backup", "start"], "POST", "/api/v1/backup", {}, ["running"]),
    ("rule criterion probe", ["match", "contains", "viagra", "cheap viagra here"],
     "POST", "/api/v1/rules/match",
     {"match_value": "viagra", "match_type": "contains", "test_value": "cheap viagra here"}, ["match"]),
    ("app passwords", ["app-password", "ann@example.com", "list"],
     "GET", "/api/v1/accounts/ann@example.com/app-passwords", None, ["phone"]),
    ("app password create", ["app-password", "ann@example.com", "create", "tablet"],
     "POST", "/api/v1/accounts/ann@example.com/app-passwords", {"name": "tablet"}, ["abcd-efgh-ijkl", "only time"]),
    ("ip ranges", ["iprange", "list"], "GET", "/api/v1/ipranges", None, ["My computer"]),
    ("report sections", ["report", "sections"], "GET", "/api/v1/reports", None,
     ["traffic", "NOT RECORDING", "cannot answer"]),
    ("report one section", ["report", "traffic", "--from", "2026-09-01", "--to", "2026-09-15",
                            "--domain", "example.com"],
     "GET", "/api/v1/reports/traffic", None, ["Counted from the message trace", "example.com", "INCOMING"]),
    ("report summary", ["report", "summary"], "GET", "/api/v1/reports/summary", None,
     ["mailboxes", "ann@example.com", "not shown"]),
    ("report as CSV", ["report", "traffic", "--csv", "-"], "GET", "/api/v1/reports/traffic", None,
     ["day,domain,incoming,outgoing", "2026-09-14,example.com,12,3"]),
    ("the escape hatch", ["api", "GET", "/api/v1/status"], "GET", "/api/v1/status", None, ["6.3.3"]),
    ("dkim status", ["dkim", "status", "example.com"], "GET", "/api/v1/domains/example.com/dkim/rotation", None,
     ["example.com: propagating",
      "publish  s20260915._domainkey.example.com TXT v=DKIM1; k=rsa; p=NEXTKEY",
      "keep     s20260315._domainkey.example.com TXT v=DKIM1; k=rsa; p=CURRENTKEY",
      "remove   s20250915._domainkey.example.com TXT",
      "switch due 2026-09-12 10:33 UTC", "The record is propagating. Until 2026-09-12 10:33 UTC."]),
    ("dkim status --json", ["dkim", "status", "example.com", "--json"], "GET", "/api/v1/domains/example.com/dkim/rotation",
     None, ['"phase": "propagating"']),
    ("dkim rotate", ["dkim", "rotate", "example.com"], "POST", "/api/v1/domains/example.com/dkim/rotation/start", {},
     ["publish  s20260915._domainkey.example.com"]),
    ("dkim rotate --selector", ["dkim", "rotate", "example.com", "--selector", "mail2026"],
     "POST", "/api/v1/domains/example.com/dkim/rotation/start", {"selector": "mail2026"}, ["s20260915"]),
    ("dkim check", ["dkim", "check", "example.com"], "POST", "/api/v1/domains/example.com/dkim/rotation/check", {},
     ["check: matches"]),
    ("dkim switch", ["dkim", "switch", "example.com"], "POST", "/api/v1/domains/example.com/dkim/rotation/switch", {},
     ["propagating"]),
    ("dkim switch --force", ["dkim", "switch", "example.com", "--force"],
     "POST", "/api/v1/domains/example.com/dkim/rotation/switch", {"force": True}, ["propagating"]),
    ("dkim retire", ["dkim", "retire", "example.com"], "POST", "/api/v1/domains/example.com/dkim/rotation/retire", {},
     ["remove"]),
    ("dkim cancel", ["dkim", "cancel", "example.com"], "DELETE", "/api/v1/domains/example.com/dkim/rotation/next", None,
     ["example.com"]),
    ("dkim run", ["dkim", "run"], "POST", "/api/v1/dkim/rotation/run", {}, ["switched"]),
    ("dkim run --seconds-ahead", ["dkim", "run", "--seconds-ahead", "262800"],
     "POST", "/api/v1/dkim/rotation/run", {"seconds_ahead": 262800}, ["as_of"]),
]


def run_python_cases(url, verbose):
    for name, arguments, method, path, body, expected in CASES:
        with Recorder.lock:
            Recorder.requests = []

        command = [sys.executable, HMCTL, "--url", url, "--password", PASSWORD] + arguments
        completed = subprocess.run(command, capture_output=True, text=True, timeout=60)

        with Recorder.lock:
            requests = list(Recorder.requests)

        if verbose:
            print("  %-34s %s" % (name, requests))

        if not check(requests, "hmctl %s: sent no request at all (%s)" % (name, completed.stderr.strip())):
            continue

        sent = requests[-1]
        sent_path = sent["path"].split("?", 1)[0]
        import urllib.parse as parse
        sent_path = parse.unquote(sent_path)

        check(sent["method"] == method,
              "hmctl %s: sent %s, expected %s" % (name, sent["method"], method))
        check(sent_path == path,
              "hmctl %s: asked for %s, expected %s" % (name, sent_path, path))
        check(sent["authorization"] == EXPECTED_BASIC,
              "hmctl %s: Authorization was %r" % (name, sent["authorization"]))
        if body is not None:
            check(sent["body"] == body,
                  "hmctl %s: body was %r, expected %r" % (name, sent["body"], body))
        check(completed.returncode == 0,
              "hmctl %s: exit %d - %s" % (name, completed.returncode, completed.stderr.strip()))
        for text in expected:
            check(text in completed.stdout,
                  "hmctl %s: %r is not in the output:\n%s" % (name, text, completed.stdout))


def run_python_refusals(url):
    """The refusals, which are as much of the contract as the successes."""
    def run(arguments, expect_code=None):
        return subprocess.run([sys.executable, HMCTL, "--url", url] + arguments,
                              capture_output=True, text=True, timeout=60,
                              env=dict(os.environ, HMAIL_PASSWORD=PASSWORD, HMAIL_API_KEY=""))

    completed = run(["account", "list"])
    check(completed.returncode != 0 and "--domain" in (completed.stderr + completed.stdout),
          "hmctl account list with no domain should say which flag is missing, said: %s%s"
          % (completed.stdout, completed.stderr))

    completed = run(["domain", "create"])
    check(completed.returncode != 0 and "--set" in (completed.stderr + completed.stdout),
          "hmctl domain create with nothing to set should say so, said: %s%s" % (completed.stdout, completed.stderr))

    # A refused switch: the status beside the refusal is printed, so the person
    # sees what is in the way, and the refusal carries its reason word.
    completed = run(["dkim", "switch", "example.net"])
    check(completed.returncode == 1 and "(propagation)" in completed.stderr
          and "propagation period" in completed.stderr and "example.net: propagating" in completed.stdout,
          "hmctl dkim switch refused with 409 should print the status and the reason, said: %s%s"
          % (completed.stdout, completed.stderr))

    completed = run(["dkim", "rotate"])
    check(completed.returncode != 0 and "domain" in (completed.stderr + completed.stdout),
          "hmctl dkim rotate with no domain should say which argument is missing, said: %s%s"
          % (completed.stdout, completed.stderr))

    completed = run(["domain", "get", "nothing.here"])
    check(completed.returncode == 4,
          "hmctl on a 404 should exit 4, exited %d" % completed.returncode)

    completed = subprocess.run(
        [sys.executable, HMCTL, "--url", "https://example.invalid:8045", "--insecure", "--password", "x", "status"],
        capture_output=True, text=True, timeout=60)
    check(completed.returncode != 0 and "loopback" in completed.stderr,
          "--insecure against a remote host must be refused with a reason, said: %s" % completed.stderr)

    completed = subprocess.run([sys.executable, HMCTL, "--url", url, "status"],
                               capture_output=True, text=True, timeout=60,
                               env=dict((k, v) for k, v in os.environ.items()
                                        if k not in ("HMAIL_PASSWORD", "HMAIL_API_KEY", "HMAIL_PASSWORD_FILE")),
                               stdin=subprocess.DEVNULL)
    check(completed.returncode != 0 and "credential" in completed.stderr,
          "with no credential and no terminal, hmctl must say so rather than prompt: %s" % completed.stderr)


POWERSHELL_CASES = [
    ("Get-HmDomain", "Get-HmDomain | Select-Object -ExpandProperty name", "GET", "/api/v1/domains", None, ["example.com"]),
    ("Get-HmDomain -Name", "Get-HmDomain -Name example.com | Select-Object -ExpandProperty name",
     "GET", "/api/v1/domains/example.com", None, ["example.com"]),
    ("New-HmDomain", "New-HmDomain -Name new.example -Active $true | Out-String",
     "POST", "/api/v1/domains", {"name": "new.example", "active": True}, ["new.example"]),
    ("Remove-HmDomain", "Remove-HmDomain -Name example.net -Confirm:$false; 'removed'",
     "DELETE", "/api/v1/domains/example.net", None, ["removed"]),
    ("Get-HmAccount", "Get-HmAccount -Domain example.com | Select-Object -ExpandProperty address",
     "GET", "/api/v1/domains/example.com/accounts", None, ["ann@example.com"]),
    ("New-HmAccount", "New-HmAccount -Domain example.com -Address new@example.com -Password secret | Out-String",
     "POST", "/api/v1/domains/example.com/accounts", {"address": "new@example.com", "password": "secret"}, ["new@example.com"]),
    ("Set-HmAccount", "Set-HmAccount -Address ann@example.com -Property @{active=$false} | Out-String",
     "PUT", "/api/v1/accounts/ann@example.com", {"active": False}, []),
    ("Get-HmSetting", "Get-HmSetting | Select-Object -ExpandProperty max_message_size_kb",
     "GET", "/api/v1/settings", None, ["20480"]),
    ("Set-HmSetting", "Set-HmSetting -Property @{max_message_size_kb=51200} | Out-String",
     "PUT", "/api/v1/settings", {"max_message_size_kb": 51200}, []),
    ("Set-HmSetting -Group", "Set-HmSetting -Group antispam -Property @{use_spf=$false} | Out-String",
     "PUT", "/api/v1/settings/antispam", {"use_spf": False}, []),
    ("Get-HmQueue", "Get-HmQueue | Select-Object -ExpandProperty to_address", "GET", "/api/v1/queue", None, ["far@away.test"]),
    ("Get-HmStatus", "Get-HmStatus | Select-Object -ExpandProperty version", "GET", "/api/v1/status", None, ["6.3.3"]),
    ("Invoke-HmApi", "Invoke-HmApi -Method GET -Path /api/v1/status | Select-Object -ExpandProperty database",
     "GET", "/api/v1/status", None, ["PostgreSQL"]),
    ("Get-HmDkimRotation", "Get-HmDkimRotation -Domain example.com | Select-Object -ExpandProperty phase",
     "GET", "/api/v1/domains/example.com/dkim/rotation", None, ["propagating"]),
    ("Get-HmDkimRotation -Records",
     "Get-HmDkimRotation -Domain example.com -Records | Where-Object Action -eq publish | ForEach-Object { $_.Host + ' ' + $_.Value }",
     "GET", "/api/v1/domains/example.com/dkim/rotation", None, ["s20260915._domainkey.example.com v=DKIM1; k=rsa; p=NEXTKEY"]),
    ("Start-HmDkimRotation", "Start-HmDkimRotation -Domain example.com -Selector mail2026 | Select-Object -ExpandProperty phase",
     "POST", "/api/v1/domains/example.com/dkim/rotation/start", {"selector": "mail2026"}, ["propagating"]),
    ("Test-HmDkimRotation", "(Test-HmDkimRotation -Domain example.com).next.check.status",
     "POST", "/api/v1/domains/example.com/dkim/rotation/check", {}, ["matches"]),
    ("Complete-HmDkimRotation -Force", "(Complete-HmDkimRotation -Domain example.com -Force -Confirm:$false).domain",
     "POST", "/api/v1/domains/example.com/dkim/rotation/switch", {"force": True}, ["example.com"]),
    ("Remove-HmDkimRetiringKey", "(Remove-HmDkimRetiringKey -Domain example.com -Confirm:$false).retired.selector",
     "POST", "/api/v1/domains/example.com/dkim/rotation/retire", {}, ["s20250915"]),
    ("Stop-HmDkimRotation", "(Stop-HmDkimRotation -Domain example.com -Confirm:$false).domain",
     "DELETE", "/api/v1/domains/example.com/dkim/rotation/next", None, ["example.com"]),
    ("Invoke-HmDkimRotation", "(Invoke-HmDkimRotation -SecondsAhead 262800).switched",
     "POST", "/api/v1/dkim/rotation/run", {"seconds_ahead": 262800}, ["1"]),
]


def run_powershell_config_cases(url, tmp, verbose):
    """The module's configuration verbs, held to the same document and the same
    refusals as hmctl's. The point of the pair is that they ask for the same
    thing; a document one writes is a document the other reads."""
    pwsh = shutil.which("pwsh") or shutil.which("powershell")
    if not pwsh or not os.path.exists(MODULE):
        return

    document = os.path.join(tmp, "ps-config.json")
    prologue = ("$ErrorActionPreference='Stop'; Import-Module '%s' -Force; "
                "Connect-HmServer -Url '%s' -Password (ConvertTo-SecureString '%s' -AsPlainText -Force) | Out-Null; "
                % (MODULE.replace("'", "''"), url, PASSWORD))

    def run(script):
        return subprocess.run([pwsh, "-NoProfile", "-NonInteractive", "-Command", prologue + script],
                              capture_output=True, text=True, timeout=180)

    with Recorder.lock:
        Recorder.requests = []
    completed = run("Export-HmConfiguration -Path '%s'" % document.replace("'", "''"))
    with Recorder.lock:
        exporting = list(Recorder.requests)
    check(completed.returncode == 0,
          "Export-HmConfiguration: exit %d - %s" % (completed.returncode, completed.stderr.strip()[:400]))
    check(all(r["method"] == "GET" for r in exporting),
          "Export-HmConfiguration made a request that was not a GET: %s" % sorted({r["method"] for r in exporting}))

    if not os.path.exists(document):
        problem("Export-HmConfiguration wrote no file")
        return

    with open(document, encoding="utf-8-sig") as handle:
        written = json.load(handle)

    check(written.get("version") == 1, "the module's document carries no version")
    names = [row.get("name") for row in written.get("sections", {}).get("domains", [])]
    check("example.com" in names, "the module's document holds no domains: %s" % names)
    addresses = [row.get("address") for row in written.get("sections", {}).get("accounts", [])]
    check("ann@example.com" in addresses, "the module's document holds no accounts: %s" % addresses)
    for row in written.get("sections", {}).get("domains", []):
        if row.get("name") == "example.com":
            check(row.get("dkim_rotation_enabled") is True and "dkim_selector" not in row,
                  "the module's document carries the selector the rotation owns: %s" % row)

    # The same document, read by the other client: one vocabulary, one file.
    completed = subprocess.run(
        [sys.executable, HMCTL, "--url", url, "--password", PASSWORD, "config", "diff", document],
        capture_output=True, text=True, timeout=120)
    check(completed.returncode == 0,
          "hmctl does not agree that the module's document matches the server (exit %d):\n%s"
          % (completed.returncode, completed.stdout))

    completed = run("Compare-HmConfiguration -Path '%s' | Measure-Object | Select-Object -ExpandProperty Count"
                    % document.replace("'", "''"))
    check(completed.returncode == 0 and completed.stdout.strip() in ("0", ""),
          "Compare-HmConfiguration of a matching document should be empty, said %r %s"
          % (completed.stdout.strip(), completed.stderr.strip()[:200]))

    # An export from before the last switch matches, and is not sent. Made by the
    # module itself, because the module compares rows as JSON text and a document
    # re-serialised here would differ in the order of its keys, not in anything
    # the rotation owns.
    stale_path = os.path.join(tmp, "ps-stale.json")
    completed = run("$d = Get-Content -Path '%s' -Raw | ConvertFrom-Json; "
                    "$d.sections.domains | Where-Object name -eq 'example.com' | "
                    "Add-Member -NotePropertyName dkim_selector -NotePropertyValue 's20260315'; "
                    "$d | ConvertTo-Json -Depth 20 | Set-Content -Path '%s' -Encoding utf8; "
                    "Compare-HmConfiguration -Path '%s' | Measure-Object | Select-Object -ExpandProperty Count"
                    % (document.replace("'", "''"), stale_path.replace("'", "''"), stale_path.replace("'", "''")))
    check(completed.returncode == 0 and completed.stdout.strip() in ("0", ""),
          "Compare-HmConfiguration of a document whose only difference is a rotation-owned selector should be empty, "
          "said %r %s" % (completed.stdout.strip(), completed.stderr.strip()[:200]))

    # The negative control: the same edit on a domain whose rotation is off is a difference.
    completed = run("$d = Get-Content -Path '%s' -Raw | ConvertFrom-Json; "
                    "$d.sections.domains | Where-Object name -eq 'example.net' | "
                    "Add-Member -NotePropertyName dkim_selector -NotePropertyValue 's20260315'; "
                    "$d | ConvertTo-Json -Depth 20 | Set-Content -Path '%s' -Encoding utf8; "
                    "Compare-HmConfiguration -Path '%s' | Where-Object Key -eq 'example.net' | Measure-Object | Select-Object -ExpandProperty Count"
                    % (document.replace("'", "''"), stale_path.replace("'", "''"), stale_path.replace("'", "''")))
    check(completed.returncode == 0 and completed.stdout.strip() == "1",
          "Compare-HmConfiguration must still see a selector change on a domain whose rotation is off, said %r %s"
          % (completed.stdout.strip(), completed.stderr.strip()[:200]))

    # A change, planned but not applied.
    with open(document, encoding="utf-8-sig") as handle:
        changed = json.load(handle)
    changed["sections"]["settings"]["settings"]["max_message_size_kb"] = 51200
    changed_path = os.path.join(tmp, "ps-changed.json")
    with open(changed_path, "w", encoding="utf-8") as handle:
        json.dump(changed, handle, indent=2, sort_keys=True)

    with Recorder.lock:
        Recorder.requests = []
    completed = run("Import-HmConfiguration -Path '%s' | Out-Null" % changed_path.replace("'", "''"))
    with Recorder.lock:
        planning = list(Recorder.requests)
    check(completed.returncode == 0,
          "Import-HmConfiguration without -Force: exit %d - %s" % (completed.returncode, completed.stderr.strip()[:300]))
    check(all(r["method"] == "GET" for r in planning),
          "Import-HmConfiguration without -Force wrote something: %s" % [r["method"] for r in planning])

    with Recorder.lock:
        Recorder.requests = []
    completed = run("Import-HmConfiguration -Path '%s' -Force -Confirm:$false | Out-Null" % changed_path.replace("'", "''"))
    with Recorder.lock:
        applying = list(Recorder.requests)
    writes = [(r["method"], r["path"], r["body"]) for r in applying if r["method"] != "GET"]
    check(("PUT", "/api/v1/settings", {"max_message_size_kb": 51200}) in writes,
          "Import-HmConfiguration -Force did not write the setting: %s" % writes)


def run_powershell_cases(url, verbose):
    pwsh = shutil.which("pwsh") or shutil.which("powershell")
    if not pwsh:
        print("  (no pwsh on this machine: the PowerShell module is not checked here)")
        return False
    if not os.path.exists(MODULE):
        print("  (no HmailServer.psm1: skipped)")
        return False

    for name, script, method, path, body, expected in POWERSHELL_CASES:
        with Recorder.lock:
            Recorder.requests = []

        full = ("$ErrorActionPreference='Stop'; Import-Module '%s' -Force; "
                "Connect-HmServer -Url '%s' -Password (ConvertTo-SecureString '%s' -AsPlainText -Force) | Out-Null; %s"
                % (MODULE.replace("'", "''"), url, PASSWORD, script))
        completed = subprocess.run([pwsh, "-NoProfile", "-NonInteractive", "-Command", full],
                                   capture_output=True, text=True, timeout=120)

        with Recorder.lock:
            requests = list(Recorder.requests)

        if verbose:
            print("  %-34s %s" % (name, requests))

        if not check(requests, "%s: sent no request (%s)" % (name, completed.stderr.strip()[:400])):
            continue

        sent = requests[-1]
        import urllib.parse as parse
        sent_path = parse.unquote(sent["path"].split("?", 1)[0])

        check(sent["method"] == method, "%s: sent %s, expected %s" % (name, sent["method"], method))
        check(sent_path == path, "%s: asked for %s, expected %s" % (name, sent_path, path))
        check(sent["authorization"] == EXPECTED_BASIC, "%s: Authorization was %r" % (name, sent["authorization"]))
        if body is not None:
            check(sent["body"] == body, "%s: body was %r, expected %r" % (name, sent["body"], body))
        check(completed.returncode == 0, "%s: exit %d - %s" % (name, completed.returncode, completed.stderr.strip()[:400]))
        for text in expected:
            check(text in completed.stdout, "%s: %r is not in the output:\n%s" % (name, text, completed.stdout))

    return True


def run_config_cases(url, tmp):
    """The whole configuration as one document: exported, compared, applied.

    Three properties are checked rather than described, because they are what
    makes a document usable in a repository: an export of an unchanged server is
    the same bytes twice, a document that matches produces an empty plan and
    exit 0, and `apply` without --force writes nothing at all.
    """
    def run(arguments):
        return subprocess.run(
            [sys.executable, HMCTL, "--url", url, "--password", PASSWORD] + arguments,
            capture_output=True, text=True, timeout=120)

    document = os.path.join(tmp, "config.json")

    with Recorder.lock:
        Recorder.requests = []
    completed = run(["config", "export", document])
    check(completed.returncode == 0, "config export: exit %d - %s" % (completed.returncode, completed.stderr))
    with Recorder.lock:
        exporting = list(Recorder.requests)
    check(all(r["method"] == "GET" for r in exporting),
          "config export made a request that was not a GET: %s" % sorted({r["method"] for r in exporting}))

    if not os.path.exists(document):
        problem("config export wrote no file")
        return

    with open(document, encoding="utf-8") as handle:
        first = handle.read()

    check('"version": 1' in first, "the document carries no version")
    check("example.com" in first and "ann@example.com" in first,
          "the document holds neither the domain nor its accounts")
    check("all@example.com" in first, "the document holds no distribution list")
    check('"dkim_rotation_enabled": true' in first and "s20260915" not in first and "dkim_selector" not in first,
          "a domain whose DKIM key rotation is on must not carry the selector and key files the rotation owns:\n%s"
          % first[:1200])
    check("mx.partner.test" in first, "the document holds no route")

    second_path = os.path.join(tmp, "config-again.json")
    run(["config", "export", second_path])
    if os.path.exists(second_path):
        with open(second_path, encoding="utf-8") as handle:
            second = handle.read()
        check(first == second, "two exports of an unchanged server are not the same bytes")

    completed = run(["config", "diff", document])
    check(completed.returncode == 0,
          "a document that matches must exit 0, exited %d:\n%s" % (completed.returncode, completed.stdout))
    check("already matches" in completed.stdout, "a matching document should say so:\n%s" % completed.stdout)

    changed = json.loads(first)
    changed["sections"]["settings"]["settings"]["max_message_size_kb"] = 51200
    for row in changed["sections"]["domains"]:
        if row.get("name") == "example.net":
            row["active"] = True
    changed_path = os.path.join(tmp, "changed.json")
    with open(changed_path, "w", encoding="utf-8") as handle:
        json.dump(changed, handle, indent=2, sort_keys=True)

    completed = run(["config", "diff", changed_path])
    check(completed.returncode == 1, "a document that differs must exit 1, exited %d" % completed.returncode)
    check("max_message_size_kb" in completed.stdout,
          "the plan does not name the changed setting:\n%s" % completed.stdout)
    check("example.net" in completed.stdout, "the plan does not name the changed domain:\n%s" % completed.stdout)

    with Recorder.lock:
        Recorder.requests = []
    completed = run(["config", "apply", changed_path])
    with Recorder.lock:
        planning = list(Recorder.requests)
    check(completed.returncode == 0, "apply without --force should exit 0, exited %d" % completed.returncode)
    check(all(r["method"] == "GET" for r in planning),
          "apply without --force wrote something: %s" % [r["method"] for r in planning])
    check("Nothing was changed" in completed.stdout, "apply without --force must say so:\n%s" % completed.stdout)

    with Recorder.lock:
        Recorder.requests = []
    run(["config", "apply", changed_path, "--force"])
    with Recorder.lock:
        applying = list(Recorder.requests)
    writes = [(r["method"], r["path"], r["body"]) for r in applying if r["method"] != "GET"]
    check(("PUT", "/api/v1/settings", {"max_message_size_kb": 51200}) in writes,
          "the setting was not written: %s" % writes)
    check(any(method == "PUT" and path == "/api/v1/domains/example.net" for method, path, _ in writes),
          "the domain was not written: %s" % writes)

    # An export from before the last switch: the old selector, for a domain whose
    # rotation is on. Replaying it would ask for the old key back.
    stale = json.loads(first)
    for row in stale["sections"]["domains"]:
        if row.get("name") == "example.com":
            row["dkim_selector"] = "s20260315"
            row["dkim_private_key_file"] = "/var/lib/hmailserver/DKIM/s20260315._domainkey.example.com.pem"
    stale_path = os.path.join(tmp, "stale.json")
    with open(stale_path, "w", encoding="utf-8") as handle:
        json.dump(stale, handle, indent=2, sort_keys=True)

    completed = run(["config", "diff", stale_path])
    check(completed.returncode == 0 and "already matches" in completed.stdout,
          "a document whose only difference is a selector the rotation owns must match, exit %d:\n%s"
          % (completed.returncode, completed.stdout))

    # The negative control: the same document with the rotation off in it and on
    # the server is a real difference, because turning it off is configuration.
    control = json.loads(json.dumps(stale))
    for row in control["sections"]["domains"]:
        if row.get("name") == "example.com":
            row["dkim_rotation_enabled"] = False
    control_path = os.path.join(tmp, "control.json")
    with open(control_path, "w", encoding="utf-8") as handle:
        json.dump(control, handle, indent=2, sort_keys=True)
    completed = run(["config", "diff", control_path])
    check(completed.returncode == 1 and "dkim_rotation_enabled" in completed.stdout,
          "turning the rotation off in a document must be a difference, exit %d:\n%s"
          % (completed.returncode, completed.stdout))

    with Recorder.lock:
        Recorder.requests = []
    run(["config", "apply", stale_path, "--force"])
    with Recorder.lock:
        applying = list(Recorder.requests)
    check(not any(r["method"] == "PUT" and r["path"] == "/api/v1/domains/example.com" for r in applying),
          "apply --force of a stale document wrote the domain whose rotation owns its selector: %s"
          % [(r["method"], r["path"], r["body"]) for r in applying if r["method"] != "GET"])

    fewer = json.loads(first)
    fewer["sections"]["domains"] = [r for r in fewer["sections"]["domains"] if r.get("name") != "example.net"]
    fewer_path = os.path.join(tmp, "fewer.json")
    with open(fewer_path, "w", encoding="utf-8") as handle:
        json.dump(fewer, handle, indent=2, sort_keys=True)

    with Recorder.lock:
        Recorder.requests = []
    completed = run(["config", "apply", fewer_path, "--force"])
    with Recorder.lock:
        applying = list(Recorder.requests)
    deleted = [r["path"] for r in applying if r["method"] == "DELETE"]
    check(not deleted, "apply --force deleted without --allow-delete: %s" % deleted)
    check("left alone" in completed.stdout, "apply must say what it left alone:\n%s" % completed.stdout)

    with Recorder.lock:
        Recorder.requests = []
    run(["config", "apply", fewer_path, "--force", "--allow-delete"])
    with Recorder.lock:
        applying = list(Recorder.requests)
    check(any(r["method"] == "DELETE" and r["path"] == "/api/v1/domains/example.net" for r in applying),
          "apply --allow-delete did not delete the domain the document dropped: %s"
          % [(r["method"], r["path"]) for r in applying])


def run_import_cases(url, tmp):
    """The CSV import, which is the one verb with logic of its own."""
    path = os.path.join(tmp, "accounts.csv")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("address,password,active,max_size_mb\r\n")
        handle.write("ann@example.com,secret,true,100\r\n")       # exists: left alone
        handle.write("carol@example.com,secret2,true,50\r\n")     # made
        handle.write("dave@example.com,secret3,false,\r\n")       # made, empty column ignored
    with Recorder.lock:
        Recorder.requests = []

    completed = subprocess.run(
        [sys.executable, HMCTL, "--url", url, "--password", PASSWORD, "accounts", "import", path, "--dry-run"],
        capture_output=True, text=True, timeout=60)

    with Recorder.lock:
        requests = list(Recorder.requests)

    check(completed.returncode == 0, "accounts import --dry-run: exit %d - %s" % (completed.returncode, completed.stderr))
    check(all(r["method"] == "GET" for r in requests),
          "accounts import --dry-run made a request that was not a GET: %s" % [r["method"] for r in requests])
    check("2 would be made" in completed.stdout,
          "accounts import --dry-run should say two would be made:\n%s" % completed.stdout)
    check("ann@example.com (exists; left alone)" in completed.stdout,
          "accounts import --dry-run should leave the existing account alone and say so:\n%s" % completed.stdout)

    with Recorder.lock:
        Recorder.requests = []

    completed = subprocess.run(
        [sys.executable, HMCTL, "--url", url, "--password", PASSWORD, "accounts", "import", path],
        capture_output=True, text=True, timeout=60)

    with Recorder.lock:
        requests = list(Recorder.requests)

    posts = [r for r in requests if r["method"] == "POST"]
    check(len(posts) == 2, "accounts import should have posted two accounts, posted %d" % len(posts))
    if len(posts) == 2:
        check(posts[0]["body"] == {"address": "carol@example.com", "password": "secret2", "active": True, "max_size_mb": 50},
              "accounts import sent %r for carol" % posts[0]["body"])
        check(posts[1]["body"] == {"address": "dave@example.com", "password": "secret3", "active": False},
              "accounts import sent %r for dave (an empty column must not be sent)" % posts[1]["body"])

    # Export.
    out = os.path.join(tmp, "out.csv")
    completed = subprocess.run(
        [sys.executable, HMCTL, "--url", url, "--password", PASSWORD, "accounts", "export", "example.com", out],
        capture_output=True, text=True, timeout=60)
    check(completed.returncode == 0, "accounts export: exit %d - %s" % (completed.returncode, completed.stderr))
    if os.path.exists(out):
        with open(out, encoding="utf-8") as handle:
            text = handle.read()
        check("address" in text.splitlines()[0], "accounts export wrote no header: %r" % text[:80])
        check("ann@example.com" in text, "accounts export wrote no accounts: %r" % text[:200])
        check("password" not in text,
              "accounts export must never write a password column - the server does not have them to give")


def run_report_cases(url, tmp):
    """The reports, whose whole meaning is in the query string.

    Every other case here asserts the path, because that is where a client goes
    wrong: it asks the wrong route. A report asks the right route with the
    wrong window, which looks identical in a path assertion and is a wrong
    number in a spreadsheet - so these assert what came after the "?".
    """
    def run(arguments):
        with Recorder.lock:
            Recorder.requests = []
        completed = subprocess.run([sys.executable, HMCTL, "--url", url, "--password", PASSWORD] + arguments,
                                   capture_output=True, text=True, timeout=60)
        with Recorder.lock:
            sent = list(Recorder.requests)
        return completed, (sent[-1]["path"] if sent else "")

    completed, path = run(["report", "traffic", "--from", "2026-09-01", "--to", "2026-09-15",
                           "--domain", "example.com", "--top", "25"])
    check("from=2026-09-01" in path and "to=2026-09-15" in path,
          "hmctl report must send the window it was given, sent %s" % path)
    check("domain=example.com" in path, "hmctl report must send the domain, sent %s" % path)
    check("top=25" in path, "hmctl report must send --top, sent %s" % path)
    check("format=csv" not in path, "hmctl report without --csv must not ask for CSV, sent %s" % path)
    check(completed.returncode == 0, "hmctl report: exit %d - %s" % (completed.returncode, completed.stderr))

    out = os.path.join(tmp, "traffic.csv")
    completed, path = run(["report", "traffic", "--csv", out])
    check("format=csv" in path, "hmctl report --csv must ask the server for the CSV, sent %s" % path)
    check(completed.returncode == 0, "hmctl report --csv: exit %d - %s" % (completed.returncode, completed.stderr))
    if os.path.exists(out):
        with open(out, encoding="utf-8") as handle:
            text = handle.read()
        check(text.startswith("day,domain,incoming,outgoing"),
              "hmctl report --csv must write the server's own CSV, wrote %r" % text[:80])
    else:
        problem("hmctl report --csv wrote no file")

    completed, path = run(["report", "nonsense"])
    check(completed.returncode != 0 and "nonsense" in (completed.stdout + completed.stderr),
          "hmctl report with an unknown section must refuse it by name, said: %s%s"
          % (completed.stdout, completed.stderr))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--python", action="store_true", help="hmctl only")
    parser.add_argument("--powershell", action="store_true", help="the PowerShell module only")
    parser.add_argument("--verbose", action="store_true")
    arguments = parser.parse_args()

    if not os.path.exists(HMCTL):
        print("No hmctl at %s" % HMCTL, file=sys.stderr)
        return 2

    server = HTTPServer(("127.0.0.1", 0), Recorder)
    port = server.server_address[1]
    url = "http://127.0.0.1:%d" % port
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    import tempfile
    tmp = tempfile.mkdtemp(prefix="hmctl-check-")

    powershell_ran = False
    try:
        if not arguments.powershell:
            print("hmctl against a recorded server")
            run_python_cases(url, arguments.verbose)
            run_python_refusals(url)
            run_import_cases(url, tmp)
            run_config_cases(url, tmp)
            run_report_cases(url, tmp)
        if not arguments.python:
            print("the PowerShell module against the same server")
            powershell_ran = run_powershell_cases(url, arguments.verbose)
            if powershell_ran:
                run_powershell_config_cases(url, tmp, arguments.verbose)
    finally:
        server.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    if PROBLEMS:
        for line in PROBLEMS:
            print("  PROBLEM %s" % line)
        print("")
        print("%d of %d checks failed." % (len(PROBLEMS), CHECKS[0]))
        return 1

    print("%d checks passed%s." % (CHECKS[0], "" if powershell_ran or arguments.python else ", PowerShell skipped"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
