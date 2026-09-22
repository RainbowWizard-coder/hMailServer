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
import atexit
import base64
import csv
import json
import os
import re
import shutil
import subprocess
import tempfile
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HMCTL = os.path.join(ROOT, "hmailserver", "source", "Tools", "Cli", "hmctl")
MODULE = os.path.join(ROOT, "hmailserver", "source", "Tools", "Cli", "HmailServer.psm1")

PASSWORD = "bench-password"

# An armoured block for the two store cases. Its contents are never parsed
# here - the recorded server answers whatever it is handed - so what this
# proves is that the clients read a FILE and send its text as the key.
KEY_FILE = os.path.join(tempfile.gettempdir(), "hmctl-openpgp-case.asc")
KEY_TEXT = ("-----BEGIN PGP PUBLIC KEY BLOCK-----\n\n"
            "mDMEaqoCExYJKwYBBAHaRw8BAQdA1BnI224Vt/ATv5Bz3LFdMeQvJKTMb+poSTbQ\n"
            "-----END PGP PUBLIC KEY BLOCK-----\n")
with open(KEY_FILE, "w", encoding="utf-8", newline="") as _key_handle:
    _key_handle.write(KEY_TEXT)

EXPECTED_BASIC = "Basic " + base64.b64encode(("Administrator:" + PASSWORD).encode("utf-8")).decode("ascii")

# A backup passphrase in a file, because neither client will take one as an
# argument: the process list is public, and this is the secret that opens every
# mailbox on the server at once. The two backup cases below read it from here.
#
# The file is this run's own and goes away with it. A fixed name in the shared
# temp directory would have been two runs on one machine writing each other's
# file half way through, and one file left lying about afterwards with a
# passphrase in it - which is a poor advertisement from the checker whose whole
# subject is that a passphrase is never left where somebody can read it.
BACKUP_PASSPHRASE = "correct horse battery staple"
_passphrase_handle, BACKUP_PASSPHRASE_FILE = tempfile.mkstemp(prefix="hmctl-check-backup-passphrase-", suffix=".txt")
with os.fdopen(_passphrase_handle, "w", encoding="utf-8") as _passphrase_file:
    _passphrase_file.write(BACKUP_PASSPHRASE + "\n")
atexit.register(lambda: os.path.exists(BACKUP_PASSPHRASE_FILE) and os.remove(BACKUP_PASSPHRASE_FILE))

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

# A rotation started by hand on a domain whose schedule is off, switched and still
# keeping its old selector: the server holds the domain's selectors (and refuses a
# change to them with 409) until that selector retires.
HELD_ROTATION_STATUS = dict(
    ROTATION_STATUS, domain="example.org", phase="idle",
    policy={"enabled": False, "interval_days": 180, "propagation_hours": 72, "retention_days": 7, "key_bits": 2048},
    current=dict(ROTATION_STATUS["current"], selector="s20260901",
                 key_file="/var/lib/hmailserver/DKIM/s20260901._domainkey.example.org.pem"),
    next=None,
    retiring={"selector": "s20260301", "key_file": "/var/lib/hmailserver/DKIM/s20260301._domainkey.example.org.pem",
              "generated": True, "since": 1788220800, "retire_due": 1788825600,
              "records": [{"host": "s20260301._domainkey.example.org", "type": "TXT",
                           "value": "v=DKIM1; k=rsa; p=OLDKEY", "action": "keep"}],
              "check": {"status": "matches", "checked": 1789000000, "detail": ""}},
    retired=None,
    waiting=[{"step": "retire", "reason": "retention", "until": 1788825600, "detail": ""},
             {"step": "start", "reason": "not_scheduled", "until": 0, "detail": ""}])

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
        {"name": "example.org", "active": True, "max_size_mb": 0, "account_count": 0,
         "dkim_rotation_enabled": False, "dkim_selector": "s20260901",
         "dkim_private_key_file": "/var/lib/hmailserver/DKIM/s20260901._domainkey.example.org.pem",
         "dkim_secondary_selector": "", "dkim_secondary_private_key_file": ""},
    ]},
    ("GET", "/api/v1/domains/example.com"): {"name": "example.com", "active": True, "plus_addressing_enabled": True},
    ("POST", "/api/v1/domains"): {"name": "new.example", "active": True},
    ("PUT", "/api/v1/domains/example.com"): {"name": "example.com", "active": False},
    ("DELETE", "/api/v1/domains/example.net"): None,
    # The listing as the real route answers it (AccountListEntryJson_ in
    # RestApiRoutes.cpp): a bare array, each entry address and active and then
    # the four fields a spreadsheet export writes. This answer once carried an
    # admin_level of 0 and no names, which the real route never sent, and the
    # export check passed against it while a real export wrote empty columns.
    ("GET", "/api/v1/domains/example.com/accounts"): [
        {"address": "ann@example.com", "active": True, "max_size_mb": 100, "first_name": "Ann",
         "last_name": "Adams", "admin_level": "domain"},
        {"address": "bob@example.com", "active": True, "max_size_mb": 0, "first_name": "",
         "last_name": "", "admin_level": "user"},
    ],
    ("POST", "/api/v1/domains/example.com/accounts"): {"address": "new@example.com", "created": True},
    # Renaming a mailbox, and moving one to another domain. The GET says whether
    # a move would be refused now and what this mailbox has been through; the
    # POST answers the document MailboxMove::OutcomeJson builds, which is also
    # what Account.MoveTo answers over COM.
    ("GET", "/api/v1/accounts/ann@example.com/move"): {
        "address": "ann@example.com", "in_use": False, "in_use_reason": "",
        "last_move": {"id": 4, "account_id": 12, "old_address": "anne@example.com",
                      "new_address": "ann@example.com", "stage": 4, "stage_name": "complete",
                      "complete": True, "keep_alias": True, "messages": 812,
                      "started": "2026-09-16 01:59:58", "finished": "2026-09-16 02:00:03",
                      "detail": "anne@example.com moved to ann@example.com"}},
    ("POST", "/api/v1/accounts/ann@example.com/move"): {
        "old_address": "ann@example.com", "new_address": "ann@example.net",
        "old_domain": "example.com", "new_domain": "example.net", "account_id": 12,
        "alias_kept": True, "store_moved": True, "messages": 812, "folders": 9,
        "aliases_repointed": 2, "list_memberships": 1, "queued_recipients": 0,
        "quarantine_entries": 3, "journal_id": 5},
    # A domain that is not in the domain listing, so the configuration cases
    # never read it: the export and the round trip of the spreadsheet, with a
    # name outside ASCII and a comma inside a cell.
    ("GET", "/api/v1/domains/example.org/accounts"): [
        {"address": "zoe@example.org", "active": False, "max_size_mb": 250, "first_name": "Zo\u00eb",
         "last_name": "O'Neil, Jr", "admin_level": "user"},
        {"address": "yan@example.org", "active": True, "max_size_mb": 0, "first_name": "",
         "last_name": "", "admin_level": "server"},
    ],
    ("POST", "/api/v1/domains/example.org/accounts"): {"address": "new@example.org", "created": True},
    # A create the server refuses, in its own words (ReadNewAccount_ to a key
    # restricted to named domains): the import must report the reason, not a
    # client error of its own.
    ("POST", "/api/v1/domains/example.net/accounts"): (403, {"error": "this api key is restricted to named domains "
                                                                   "and cannot make an account a server administrator; "
                                                                   "only the administrator password or a key issued for "
                                                                   "every domain can"}),
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
    ("POST", "/api/v1/accounts/ann@example.com/export"): {"exported": 3, "folders": 2, "contacts": 1, "calendar_objects": 0, "log_lines": 4, "archive": "C:\\exports\\ann.zip"},
    ("POST", "/api/v1/addresses/erase"): {"removed": 7, "include_archive": True, "include_logs": True},
    ("GET", "/api/v1/quarantine"): {"messages": [
        {"id": 3, "recipient": "ann@example.com", "from_address": "spam@away.test",
         "subject": "Held", "reason": "virus", "received": "2026-09-15T04:00:00Z"}]},
    ("POST", "/api/v1/quarantine/3/release"): {"released": True},
    ("GET", "/api/v1/settings"): {"welcome_smtp": "hMailServer", "max_message_size_kb": 20480},
    ("PUT", "/api/v1/settings"): {"max_message_size_kb": 51200},
    ("GET", "/api/v1/settings/antispam"): {"use_spf": True, "spam_mark_threshold": 5},
    ("PUT", "/api/v1/settings/antispam"): {"use_spf": False},
    ("POST", "/api/v1/settings/breach-check/test"): {"reachable": True, "status": 200, "entries": 812, "padded": True, "milliseconds": 41},
    ("GET", "/api/v1/settings/bounces"): {"processing_enabled": False, "hard_bounce_threshold": 1,
                                          "suppression_action": "refuse"},
    ("PUT", "/api/v1/settings/bounces"): {"processing_enabled": True, "hard_bounce_threshold": 1,
                                          "suppression_action": "observe"},
    # Web Push: the private key is write-only, so a GET never carries it and an
    # export cannot either.
    ("GET", "/api/v1/settings/push"): {"enabled": False, "subject": "", "time_to_live_seconds": 86400,
                                       "max_devices_per_account": 10, "max_attempts": 5, "vapid_public_key": ""},
    ("PUT", "/api/v1/settings/push"): {"enabled": True, "subject": "", "time_to_live_seconds": 86400,
                                       "max_devices_per_account": 10, "max_attempts": 5, "vapid_public_key": ""},
    ("GET", "/api/v1/domains/example.com/suppressions"): {
        "domain": "example.com", "processing_enabled": True, "action": "refuse", "total": 1, "offset": 0,
        "items": [{"address": "gone@elsewhere.test", "source": "bounce", "category": "hard", "status": "5.1.1",
                   "diagnostic": "550 5.1.1 User unknown", "remote_mta": "mx.elsewhere.test",
                   "created": "2026-09-14 10:00:00", "expires": "", "expired": False}]},
    ("POST", "/api/v1/domains/example.com/suppressions"): {
        "address": "old@elsewhere.test", "source": "manual", "category": "none", "status": "",
        "diagnostic": "old export", "remote_mta": "", "created": "2026-09-15 09:00:00",
        "expires": "2026-12-14 09:00:00", "expired": False},
    ("DELETE", "/api/v1/domains/example.com/suppressions/gone@elsewhere.test"): {"removed": True},
    # The OpenPGP key directory: a domain's listing, one account's document,
    # the store and the removal. Only public keys are ever held, so nothing
    # here carries a secret.
    ("GET", "/api/v1/settings/openpgp"): {"web_key_directory_enabled": True, "key_maximum_bytes": 65536,
                                          "keys_per_account": 8, "require_address_in_user_id": True},
    ("PUT", "/api/v1/settings/openpgp"): {"web_key_directory_enabled": True, "keys_per_account": 4},
    ("GET", "/api/v1/domains/example.com/openpgp"): {
        "domain": "example.com", "server_enabled": True, "domain_enabled": True, "publish_new_accounts": False,
        "total": 1, "keys": [{"id": 1, "address": "ann@example.com",
                              "fingerprint": "c29b6cce361e73ba13724cb8bbee452b9b0a0bb7", "version": 4,
                              "algorithm": 22, "algorithm_name": "EdDSA", "bits": 256,
                              "user_id": "Ann <ann@example.com>", "created": 1789526547, "expires": 0,
                              "bytes": 413, "uploaded": 1789526600, "uploaded_by": "administrator",
                              "hashed_local_part": "ftnwpzeeynpbixr6qyd8grpy8xuzcqwt",
                              "advanced_url": "https://openpgpkey.example.com/.well-known/openpgpkey/example.com/hu/ftnwpzeeynpbixr6qyd8grpy8xuzcqwt?l=ann",
                              "direct_url": "https://example.com/.well-known/openpgpkey/hu/ftnwpzeeynpbixr6qyd8grpy8xuzcqwt?l=ann",
                              "published": True}]},
    ("GET", "/api/v1/accounts/ann@example.com/openpgp"): {
        "address": "ann@example.com", "publish": True, "server_enabled": True, "domain_enabled": True,
        "published": True, "hashed_local_part": "ftnwpzeeynpbixr6qyd8grpy8xuzcqwt",
        "advanced_url": "https://openpgpkey.example.com/.well-known/openpgpkey/example.com/hu/ftnwpzeeynpbixr6qyd8grpy8xuzcqwt?l=ann",
        "direct_url": "https://example.com/.well-known/openpgpkey/hu/ftnwpzeeynpbixr6qyd8grpy8xuzcqwt?l=ann",
        "limit": 8, "keys": []},
    ("PUT", "/api/v1/accounts/ann@example.com/openpgp"): {
        "id": 2, "fingerprint": "800b26ccc36e0e3b4a61a26b19fe7a2fe905df20", "version": 4, "algorithm": 1,
        "bits": 2048, "user_id": "Ann <ann@example.com>", "created": 1789526561, "expires": 0, "bytes": 1030,
        "uploaded": 1789526700, "uploaded_by": "administrator", "address": "ann@example.com",
        "advanced_url": "https://openpgpkey.example.com/.well-known/openpgpkey/example.com/hu/ftnwpzeeynpbixr6qyd8grpy8xuzcqwt?l=ann"},
    ("DELETE", "/api/v1/accounts/ann@example.com/openpgp/800b26ccc36e0e3b4a61a26b19fe7a2fe905df20"): {"removed": True},
    ("GET", "/api/v1/domains/example.com/bounces"): {
        "domain": "example.com", "total": 1, "offset": 0,
        "items": [{"address": "gone@elsewhere.test", "hard": 2, "soft": 1, "mailbox_full": 0, "policy": 0, "other": 0,
                   "consecutive_hard": 2, "last_status": "5.1.1", "last_category": "hard",
                   "last_bounce": "2026-09-14 10:00:00", "last_delivered": "", "last_complaint": "",
                   "suppressed": True}]},
    ("POST", "/api/v1/bounces/classify"): {"category": "other", "suppresses": False, "trusted": False, "status": "5.1.1"},
    ("POST", "/api/v1/bounces/expire"): {"suppressions_expired": 2, "counts_pruned": 5, "report_keys_pruned": 40},
    ("GET", "/api/v1/settings/complaints"): {"processing_enabled": False, "mailbox": "",
                                             "suppression_threshold": 0, "retention_days": 180},
    ("PUT", "/api/v1/settings/complaints"): {"processing_enabled": True, "mailbox": "fbl@example.com",
                                             "suppression_threshold": 1, "retention_days": 180},
    ("GET", "/api/v1/domains/example.com/complaints"): {
        "domain": "example.com", "processing_enabled": True, "mailbox_configured": True,
        "window_days": 7, "sent": 4000, "complaints": 1, "rate_ppm": 250,
        "rate_judged": True, "total": 1, "offset": 0,
        "items": [{"address": "reader@provider.test", "sender": "anna@example.com", "account_id": 1,
                   "feedback_type": "abuse", "counts": True, "provider": "SomeGenerator/1.0",
                   "source_ip": "192.0.2.1", "reported_domain": "example.com",
                   "message_id": "abc.123@example.com", "subject": "Our newsletter",
                   "arrival": "2026-09-15 09:00:00", "received": "2026-09-15 09:04:00"}]},
    ("GET", "/api/v1/complaints/rates"): {
        "processing_enabled": True, "mailbox": "fbl@example.com", "window_days": 7, "minimum_volume": 100,
        "domains": [{"domain": "example.com", "sent": 4000, "complaints": 1, "rate_ppm": 250, "judged": True},
                    {"domain": "second.example", "sent": 12, "complaints": 1, "rate_ppm": 0, "judged": False}]},
    ("POST", "/api/v1/complaints/expire"): {"complaints_pruned": 3},
    # The reports a domain receives, read (RestApiDomainReports.cpp): the
    # summary the chart is drawn from, the listing, one report's records, the
    # TLS-RPT reports and the sweep.
    ("GET", "/api/v1/domains/example.com/dmarc-reports/summary"): {
        "domain": "example.com", "days": 30, "since": 1786838400, "until": 1789430400,
        "report_mailbox": "dmarc@example.com", "mailbox_configured": True,
        "reports": 1, "messages": 15, "passed": 12, "failed": 3, "dkim_passed": 12, "spf_passed": 12,
        "quarantined": 3, "rejected": 0,
        "reporters": [{"org": "google.com", "reports": 1, "messages": 15, "checked": 1, "authenticated": 1}],
        "by_day": [{"day": "2026-09-15", "messages": 15, "passed": 12, "failed": 3}],
        "sources": [{"ip": "192.0.2.10", "messages": 12, "passed": 12, "failed": 0, "dkim_passed": 12, "spf_passed": 12,
                     "quarantined": 0, "rejected": 0, "header_from": "example.com", "reporters": 1},
                    {"ip": "198.51.100.7", "messages": 3, "passed": 0, "failed": 3, "dkim_passed": 0, "spf_passed": 0,
                     "quarantined": 3, "rejected": 0, "header_from": "example.com", "reporters": 1}],
        "sources_total": 2},
    ("GET", "/api/v1/domains/example.com/dmarc-reports"): {
        "domain": "example.com", "report_mailbox": "dmarc@example.com", "mailbox_configured": True, "total": 1, "offset": 0,
        "items": [{"id": 31, "org": "google.com", "email": "noreply-dmarc-support@google.com", "report_id": "12345678901234567890",
                   "begin": 1789430400, "end": 1789516799, "policy_domain": "example.com", "adkim": "r", "aspf": "r",
                   "p": "quarantine", "sp": "reject", "pct": 100, "messages": 15, "passed": 12, "received": 1789520000,
                   "source": "google.com!example.com!1789430400!1789516799.xml.gz", "records": 2,
                   "message_dmarc": "pass", "message_from": "google.com"}]},
    ("GET", "/api/v1/domains/example.com/dmarc-reports/31"): {
        "id": 31, "org": "google.com", "email": "noreply-dmarc-support@google.com", "report_id": "12345678901234567890",
        "begin": 1789430400, "end": 1789516799, "policy_domain": "example.com", "adkim": "r", "aspf": "r",
        "p": "quarantine", "sp": "reject", "pct": 100, "messages": 15, "passed": 12, "received": 1789520000,
        "source": "google.com!example.com!1789430400!1789516799.xml.gz", "message_dmarc": "pass", "message_from": "google.com",
        "records": [{"source_ip": "192.0.2.10", "count": 12, "disposition": "none", "dkim": "pass", "spf": "pass",
                     "header_from": "example.com", "envelope_from": "example.com",
                     "dkim_auth": "d=example.com s=s1 result=pass", "spf_auth": "d=example.com scope=mfrom result=pass"},
                    {"source_ip": "198.51.100.7", "count": 3, "disposition": "quarantine", "dkim": "fail", "spf": "fail",
                     "header_from": "example.com", "envelope_from": "", "dkim_auth": "",
                     "spf_auth": "d=forger.test scope=mfrom result=pass"}]},
    ("GET", "/api/v1/domains/example.com/tls-reports"): {
        "domain": "example.com", "days": 30, "since": 1786838400, "mailbox_configured": True,
        "sessions_succeeded": 5326, "sessions_failed": 303, "total": 1, "offset": 0,
        "items": [{"id": 41, "org": "Company-X", "contact": "sts-reporting@company-x.example",
                   "report_id": "5065427c-23d3-47ca-b6e0-946ea0e8c4be", "begin": 1789430400, "end": 1789516799,
                   "policy_type": "sts", "policy_domain": "example.com", "successful": 5326, "failed": 303,
                   "failures": [{"result_type": "certificate-expired", "sending_mta_ip": "2001:db8:abcd:12::1",
                                 "receiving_mx_hostname": "mx1.mail.example.com", "receiving_ip": "203.0.113.6", "count": 100}],
                   "received": 1789520000, "source": "company-x.example!example.com!1789430400!1789516799!001.json.gz"}]},
    ("POST", "/api/v1/domain-reports/expire"): {"dmarc_reports_pruned": 2, "tls_reports_pruned": 1, "older_than_days": 365, "complete": True},
    # The shape RestApiSettings.cpp's IniList answers, both halves: names for
    # the Linux test shim, settings for these two clients. Until 15 September
    # 2026 this fake answered a settings array the real route did not send,
    # and both clients exported an empty ini section against every real server
    # while this check passed. check_ini_route_shape() below reads the route's
    # source so that the fake cannot drift from it again.
    ("GET", "/api/v1/settings/ini"): {
        "names": ["DisableAUTHList", "OAuth2HmacSecret"],
        "settings": [{"name": "DisableAUTHList", "value": "0", "secret": False},
                     {"name": "OAuth2HmacSecret", "value": "(set)", "secret": True}]},
    ("PUT", "/api/v1/settings/ini/DisableAUTHList"): {"name": "DisableAUTHList", "value": "1", "present": True},
    # What this server has changed from stock: the shape SettingsDiff::Json
    # writes, which GET /api/v1/settings/changed answers and COM's
    # Settings.GetChangedFromDefault answers from the same function. A plain ini
    # key, a secret named and not shown, a name the server ships no default for,
    # a property of the other store, and one the file's [SettingsOverride]
    # section is forcing over what is stored.
    ("GET", "/api/v1/settings/changed"): {
        "changed": 5, "considered": 391,
        "settings": [
            {"store": "ini", "name": "LogLevel", "value": "5", "default": "9", "secret": False, "known": True,
             "overridden": False},
            {"store": "ini", "name": "OAuth2HmacSecret", "value": "(set)", "default": "", "secret": True, "known": True,
             "overridden": False},
            {"store": "ini", "name": "OurOwnScriptKey", "value": "yes", "default": "", "secret": False, "known": False,
             "overridden": False},
            {"store": "ini", "name": "DnsServer", "value": "10.0.0.1", "default": "", "secret": False, "known": True,
             "overridden": True},
            {"store": "database", "name": "smtpnoofretries", "value": "6", "default": "4", "secret": False, "known": True,
             "overridden": False},
        ]},
    ("GET", "/api/v1/logs"): {"logs": [{"name": "hmailserver_2026-09-15.log", "size": 91234}]},
    ("GET", "/api/v1/logs/hmailserver_2026-09-15.log"): {"lines": ["\"SMTPD\" 1 \"04:00:00.001\" \"127.0.0.1\" \"EHLO\""]},
    ("GET", "/api/v1/backup"): {"state": "idle", "last_backup": "2026-09-14T02:00:00Z"},
    ("POST", "/api/v1/backup"): {"state": "running"},
    ("POST", "/api/v1/backup/restore"): {"started": True, "encrypted": True,
                                         "contains": {"settings": False, "domains": True, "messages": True}},
    ("PUT", "/api/v1/settings/backup"): {"destination": "/var/backups", "encryption_passphrase_set": True},
    ("GET", "/api/v1/backup/archives"): {"destination": "/var/backups", "archives": [
        {"name": "HMBackup 2026-09-21 020000.7z", "path": "/var/backups/HMBackup 2026-09-21 020000.7z",
         "created": "2026-09-21 02:00:00", "size": 1048576, "encrypted": True}]},
    ("GET", "/api/v1/backup/HMBackup 2026-09-21 020000.7z/mailboxes/ann@example.com"): {
        "archive": "/var/backups/HMBackup 2026-09-21 020000.7z", "address": "ann@example.com",
        "folders": [{"path": "INBOX", "subscribed": True, "special_use": 0, "messages": [
            {"id": 4101, "folder": "INBOX", "subject": "The Tuesday figures", "from": "bob@example.com",
             "date": "2026-09-15 09:12:00", "size": 2048, "flags": 1, "keywords": ""}]}],
        "messages": 1, "subjects": True, "subjects_note": ""},
    ("POST", "/api/v1/backup/HMBackup 2026-09-21 020000.7z/mailboxes/ann@example.com/restore"): {
        "job": 7, "state": "queued", "archive": "/var/backups/HMBackup 2026-09-21 020000.7z", "address": "ann@example.com",
        "mode": "merge", "message": 0, "status": "/api/v1/backup/restore-mailbox/7"},
    ("GET", "/api/v1/backup/restore-mailbox/7"): {
        "job": 7, "state": "done", "archive": "/var/backups/HMBackup 2026-09-21 020000.7z", "address": "ann@example.com",
        "mode": "merge", "message": 0, "folders": 3, "added": 12, "skipped": 4, "failed": 0, "error": "",
        "started": "2026-09-22 12:00:00", "finished": "2026-09-22 12:00:03"},
    ("POST", "/api/v1/rules/match"): {"match": True},
    ("GET", "/api/v1/accounts/ann@example.com/app-passwords"): {"app_passwords": [
        {"id": 2, "name": "phone", "created": "2026-09-01T10:00:00Z", "last_used": None}]},
    ("POST", "/api/v1/accounts/ann@example.com/app-passwords"): {"id": 3, "name": "tablet", "password": "abcd-efgh-ijkl"},
    # The clients that have signed in as ann: a phone on an app password and a
    # browser. The shape SignInStore::ToJson writes.
    ("GET", "/api/v1/accounts/ann@example.com/devices"): {"devices": [
        {"id": "0123456789abcdef0123456789abcdef", "protocol": "imap", "address": "203.0.113.9", "agent": "",
         "credential": "app_password", "credential_name": "phone", "first_seen": "2026-09-01 08:00:00",
         "last_seen": "2026-09-16 07:30:00", "sign_ins": 412, "revoked": False, "revoked_at": ""},
        {"id": "fedcba9876543210fedcba9876543210", "protocol": "web", "address": "198.51.100.4", "agent": "Mozilla/5.0",
         "credential": "password", "credential_name": "", "first_seen": "2026-09-14 11:02:00",
         "last_seen": "2026-09-16 09:15:00", "sign_ins": 6, "revoked": False, "revoked_at": ""}]},
    ("DELETE", "/api/v1/accounts/ann@example.com/devices/0123456789abcdef0123456789abcdef"): {
        "revoked": True, "forgotten": False, "sessions_ended": 0, "connections_dropped": 1, "credential_disabled": True},
    ("GET", "/api/v1/rules"): {"rules": [{"name": "Tag support", "active": True, "sort_order": 1}]},
    ("GET", "/api/v1/routes"): {"routes": [{"domain_name": "partner.test", "target_smtp_host": "mx.partner.test",
                                            "target_smtp_port": 25}]},
    ("GET", "/api/v1/ports"): {"ports": [{"protocol": "SMTP", "address": "0.0.0.0", "port": 25,
                                          "connection_security": 0}]},
    ("GET", "/api/v1/domains/example.com/lists"): {"lists": [{"address": "all@example.com", "active": True,
                                                              "mode": 0, "membership": "domain"}]},
    # The one list of example.com finds its members by query rather than by
    # hand, so its members are one row kept by hand and the accounts the query
    # answers - which is what this route exists to say.
    ("GET", "/api/v1/lists/all@example.com/members"): {
        "address": "all@example.com", "membership": "domain", "membership_kind": "domain", "count": 2,
        "members": [{"address": "ann@example.com", "source": "row"},
                    {"address": "bob@example.com", "source": "query"}]},
    ("GET", "/api/v1/domains/example.net/accounts"): [],
    ("GET", "/api/v1/domains/example.net/aliases"): {"aliases": []},
    ("GET", "/api/v1/domains/example.net/lists"): {"lists": []},
    ("PUT", "/api/v1/domains/example.net"): {"name": "example.net", "active": True},
    ("GET", "/api/v1/domains/example.org/aliases"): {"aliases": []},
    ("GET", "/api/v1/domains/example.org/lists"): {"lists": []},
    ("PUT", "/api/v1/domains/example.org"): {"name": "example.org", "active": False},
    ("GET", "/api/v1/domains/example.org/dkim/rotation"): HELD_ROTATION_STATUS,
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
    # Spam traps: a domain's, the record of what has addressed one, and the
    # clearing of that record.
    ("GET", "/api/v1/domains/example.com/spam-traps"): {"domain": "example.com", "spam_traps": [
        {"id": 701, "address": "never-used@example.com", "local_part": "never-used", "created": "2026-09-15 08:00:00"}]},
    ("POST", "/api/v1/domains/example.com/spam-traps"): (201, {"id": 702, "address": "sales-2004@example.com",
                                                              "local_part": "sales-2004", "created": "2026-09-16 00:00:00"}),
    ("DELETE", "/api/v1/spam-traps/never-used@example.com"): {"deleted": True},
    ("GET", "/api/v1/spam-traps/hits"): {"items": [{"id": 801, "time": "2026-09-15 09:12:00", "address": "never-used@example.com", "domain": "example.com", "ip_address": "203.0.113.9", "sender": "harvester@spammer.example", "banned": True, "blocked": True, "note": "banned for 1440 minutes; sender blocked for 30 days"}], "hits_since_start": 1},
    ("GET", "/api/v1/domains/example.com/spam-traps/hits"): {"items": [{"id": 801, "time": "2026-09-15 09:12:00", "address": "never-used@example.com", "domain": "example.com", "ip_address": "203.0.113.9", "sender": "harvester@spammer.example", "banned": True, "blocked": True, "note": "banned for 1440 minutes; sender blocked for 30 days"}]},
    ("DELETE", "/api/v1/spam-traps/hits"): {"removed": 1},
    # The journaling and dual-delivery rules, which the REST listing answers
    # with as a bare array, as the other small collections do.
    ("GET", "/api/v1/journal-rules"): [
        {"id": 601, "scope": "domain", "target": "example.com", "journal_address": "archive@compliance.example",
         "direction": "both", "mode": "report", "enabled": True, "note": "the auditors"}],
    ("POST", "/api/v1/journal-rules"): (201, {"id": 602, "scope": "account", "target": "ann@example.com",
                                             "journal_address": "copy@second-system.example", "direction": "outbound",
                                             "mode": "copy", "enabled": True, "note": ""}),
    ("PUT", "/api/v1/journal-rules/601"): {"id": 601, "scope": "domain", "target": "example.com",
                                          "journal_address": "archive@compliance.example", "direction": "both",
                                          "mode": "report", "enabled": False, "note": "the auditors"},
    ("DELETE", "/api/v1/journal-rules/601"): {"deleted": True},
    ("GET", "/api/v1/domains/example.com/dkim/rotation"): ROTATION_STATUS,
    ("POST", "/api/v1/domains/example.com/dkim/rotation/start"): ROTATION_STATUS,
    ("POST", "/api/v1/domains/example.com/dkim/rotation/check"): ROTATION_STATUS,
    ("POST", "/api/v1/domains/example.com/dkim/rotation/switch"): ROTATION_STATUS,
    ("POST", "/api/v1/domains/example.com/dkim/rotation/retire"): ROTATION_STATUS,
    ("DELETE", "/api/v1/domains/example.com/dkim/rotation/next"): ROTATION_STATUS,
    # The webhook subscriptions and their queue. No secret is in either answer;
    # secret_set is what the server says instead.
    ("GET", "/api/v1/webhooks"): {
        "webhooks": [
            {"name": "ticketing", "url": "https://tickets.example/hook", "secret_set": True,
             "events": "message.bounced", "domains": "example.com", "addresses": "", "enabled": True,
             "include_subject": False, "max_per_minute": 0, "created": 1789000000,
             "pending": 1, "delivered": 12, "dead_lettered": 0, "dropped": 0}],
        "events": ["message.received", "message.delivered", "message.bounced", "message.quarantined", "account.locked"],
    },
    ("GET", "/api/v1/webhooks/ticketing"): {
        "name": "ticketing", "url": "https://tickets.example/hook", "secret_set": True,
        "events": "message.bounced", "domains": "example.com", "addresses": "", "enabled": True,
        "include_subject": False, "max_per_minute": 0, "created": 1789000000,
        "pending": 1, "delivered": 12, "dead_lettered": 0, "dropped": 0},
    ("POST", "/api/v1/webhooks"): {"name": "billing", "url": "https://hooks.example/mail", "secret_set": True,
                                   "events": "", "domains": "", "addresses": "", "enabled": True,
                                   "include_subject": False, "max_per_minute": 0, "created": 1789000400,
                                   "pending": 0, "delivered": 0, "dead_lettered": 0, "dropped": 0},
    ("PUT", "/api/v1/webhooks/ticketing"): {"name": "ticketing", "url": "https://tickets.example/hook",
                                            "secret_set": True, "events": "message.bounced", "domains": "example.com",
                                            "addresses": "", "enabled": False, "include_subject": False,
                                            "max_per_minute": 0, "created": 1789000000,
                                            "pending": 1, "delivered": 12, "dead_lettered": 0, "dropped": 0},
    ("DELETE", "/api/v1/webhooks/ticketing"): {"deleted": True},
    ("GET", "/api/v1/webhooks/deliveries"): {"deliveries": [
        {"id": 41, "created": 1789000100, "attempts": 3, "next_attempt": 0, "webhook": "ticketing",
         "event": "message.bounced", "state": "dead-lettered", "error": "the endpoint answered 502",
         "body": "{\"event\":\"message.bounced\"}"}]},
    ("POST", "/api/v1/webhooks/run"): {"delivered": 2, "removed": 7},
    ("POST", "/api/v1/webhooks/test"): {"queued": 1},
    ("POST", "/api/v1/dkim/rotation/run"): {"as_of": 1789000000, "domains": 2, "generated": 0, "checked": 1,
                                            "switched": 1, "retired": 0, "errors": 0},

    # The reputation watch, with nothing listed: one address checked against two
    # lists, an IPv6 address a list does not cover, and a private address that is
    # deliberately not looked up at all.
    # The sample domain: present, so that status prints what is in it, and so that
    # remove has something to report. The state document carries no password - the
    # server mints one per creation and keeps only its hash - so status says so
    # instead of printing one.
    ("GET", "/api/v1/setup/sample-domain"): {
        "state": "present", "domain": "example.test", "domain_id": 42, "exists": True,
        "list": "everyone@example.test", "password_shown_once": True,
        "accounts": ["alice@example.test", "bob@example.test", "carol@example.test"]},
    ("POST", "/api/v1/setup/sample-domain"): (201, {
        "created": True, "domain": "example.test", "domain_id": 42, "password": "7TQ4M-KJ9XD-2VWHF-BNRE5",
        "accounts_created": 3, "list": "everyone@example.test", "list_members": 3, "rules": 1,
        "messages": 8, "deliveries": 14,
        "message": "example.test was created with 3 mailboxes, the list everyone@example.test and 8 messages on "
                   "their way into it. All of the mailboxes sign in with the password 7TQ4M-KJ9XD-2VWHF-BNRE5, "
                   "which is shown here once and is not stored anywhere."}),
    ("DELETE", "/api/v1/setup/sample-domain"): {
        "removed": True, "domain": "example.test", "domain_id": 42, "accounts": 3, "lists": 1, "rules": 1,
        "messages": 14, "queued": 0,
        "message": "example.test was removed: 3 mailboxes, 1 distribution list(s), 1 rule(s) and 14 message(s) went with it."},
    ("GET", "/api/v1/reputation"): {
        "enabled": True,
        "lists": [{"zone": "bl.example.test", "family": "any", "codes": ""},
                  {"zone": "ipv4.example.test", "family": "ipv4", "codes": ""}],
        "lists_problem": "",
        "addresses": [
            {"address": "192.0.2.25", "family": "ipv4", "sources": ["bind"], "checked": 1789000000, "listed": False,
             "results": [
                 {"list": "bl.example.test", "status": "not_listed", "listed": False, "code": "", "reason": "",
                  "checked": 1789000000, "listed_since": 0},
                 {"list": "ipv4.example.test", "status": "not_listed", "listed": False, "code": "", "reason": "",
                  "checked": 1789000000, "listed_since": 0}]},
            {"address": "2001:db8::25", "family": "ipv6", "sources": ["setting"], "checked": 1789000000, "listed": False,
             "results": [
                 {"list": "bl.example.test", "status": "not_listed", "listed": False, "code": "", "reason": "",
                  "checked": 1789000000, "listed_since": 0},
                 {"list": "ipv4.example.test", "status": "not_applicable", "listed": None, "code": "",
                  "reason": "The list covers IPv4 addresses only, so it has nothing to say about this one.",
                  "checked": 1789000000, "listed_since": 0}]}],
        "skipped": [{"address": "10.0.0.5", "source": "rule", "reason": "private",
                     "detail": "A private address. It is not what another server sees."}],
        "last_checked": 1789000000, "next_check": 1789086400, "now": 1789000100,
        "counts": {"addresses": 2, "lists": 2, "listed": 0, "not_listed": 3, "unknown": 0,
                   "not_applicable": 1, "not_checked": 0, "skipped": 1}},
}

# A check answers the same report with a run member saying what it did.
ANSWERS[("POST", "/api/v1/reputation/check")] = dict(
    ANSWERS[("GET", "/api/v1/reputation")],
    run={"as_of": 1789000200, "addresses": 2, "lists": 2, "lookups": 8, "listed": 0, "unknown": 0, "stored": True})

# Answers that are refusals, by (method, path): the status code and the body.
REFUSALS = {
    ("POST", "/api/v1/domains/example.net/dkim/rotation/switch"): (409, {
        "error": "The next key's record has not resolved for the propagation period yet.",
        "reason": "propagation",
        "status": dict(ROTATION_STATUS, domain="example.net")}),
}

# Routes that answer once and refuse afterwards, which is what the real one does:
# a sample domain that is already there is refused with 409. It cannot be a plain
# REFUSALS entry, because the success case has to be able to run as well - and a
# route that answers 201 and then 409 to the same call is the server's own
# behaviour rather than a trick.
REFUSED_AFTER_THE_FIRST = {
    ("POST", "/api/v1/setup/sample-domain"): (409, {
        "error": "The sample domain example.test is already here. Remove it first if you want a fresh one.",
        "sample": {"state": "present", "domain": "example.test", "domain_id": 42, "exists": True,
                   "list": "everyone@example.test", "password_shown_once": True,
                   "accounts": ["alice@example.test", "bob@example.test", "carol@example.test"]}}),
}

# The one route that answers something other than JSON. Kept apart from ANSWERS
# because it is decided by the query string, which ANSWERS is keyed without:
# the point of the case is that the client ASKS for format=csv.
REPORT_CSV = ("day,domain,incoming,outgoing\r\n"
              "2026-09-14,example.com,12,3\r\n")


class Recorder(BaseHTTPRequestHandler):
    requests = []
    lock = threading.Lock()

    # The routes of REFUSED_AFTER_THE_FIRST that have already been answered once.
    # Not cleared between cases: "the second time it is asked" is the whole point.
    served = set()

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

        if (method, path) in REFUSED_AFTER_THE_FIRST:
            with Recorder.lock:
                already = (method, path) in Recorder.served
                Recorder.served.add((method, path))
            if already:
                code, refusal = REFUSED_AFTER_THE_FIRST[(method, path)]
                payload = json.dumps(refusal).encode("utf-8")
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
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
        status = 201 if method == "POST" else 200
        if isinstance(answer, tuple):
            # (status, body): a refusal, answered as the real route answers one.
            status, answer = answer
        if answer is None:
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        payload = json.dumps(answer).encode("utf-8")
        self.send_response(status)
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

ROTATION_OWNED_KEYS = ("dkim_selector", "dkim_private_key_file", "dkim_secondary_selector", "dkim_secondary_private_key_file")


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
    ("settings get push", ["settings", "get", "push"], "GET", "/api/v1/settings/push", None,
     ["max_devices_per_account"]),
    ("settings set push", ["settings", "set", "push", "--set", "enabled=true"],
     "PUT", "/api/v1/settings/push", {"enabled": True}, []),
    ("account get by address", ["account", "get", "ann@example.com"], "GET", "/api/v1/accounts/ann@example.com", None, ["vacation_message"]),
    ("account update by address", ["account", "update", "ann@example.com", "--set", "active=false"],
     "PUT", "/api/v1/accounts/ann@example.com", {"active": False}, []),
    ("account delete by address", ["account", "delete", "bob@example.com"], "DELETE", "/api/v1/accounts/bob@example.com", None, ["deleted"]),
    ("mailbox status", ["mailbox", "status", "ann@example.com"],
     "GET", "/api/v1/accounts/ann@example.com/move", None,
     ["ann@example.com", "anne@example.com -> ann@example.com", "complete"]),
    ("mailbox move keeps the old address unless told not to",
     ["mailbox", "move", "ann@example.com", "ann@example.net"],
     "POST", "/api/v1/accounts/ann@example.com/move",
     {"new_address": "ann@example.net", "keep_alias": True},
     ["ann@example.com moved to ann@example.net", "812 message(s) in 9 folder(s)",
      "2 alias(es) re-pointed", "The old address is kept as an alias"]),
    ("spamtrap list", ["spamtrap", "list", "--domain", "example.com"],
     "GET", "/api/v1/domains/example.com/spam-traps", None, ["never-used@example.com"]),
    ("spamtrap create", ["spamtrap", "create", "--domain", "example.com", "--set", "address=sales-2004"],
     "POST", "/api/v1/domains/example.com/spam-traps", {"address": "sales-2004"}, ["sales-2004@example.com"]),
    ("spamtrap delete", ["spamtrap", "delete", "never-used@example.com"],
     "DELETE", "/api/v1/spam-traps/never-used@example.com", None, ["deleted"]),
    ("traphits list", ["traphits", "list"], "GET", "/api/v1/spam-traps/hits", None,
     ["never-used@example.com", "203.0.113.9", "harvester@spammer.example"]),
    ("traphits list --domain", ["traphits", "list", "--domain", "example.com"],
     "GET", "/api/v1/domains/example.com/spam-traps/hits", None, ["never-used@example.com"]),
    ("traphits list --limit", ["traphits", "list", "--limit", "20"],
     "GET", "/api/v1/spam-traps/hits", None, ["harvester@spammer.example"]),
    ("traphits clear", ["traphits", "clear"], "DELETE", "/api/v1/spam-traps/hits", None, ["removed"]),
    ("journalrule list", ["journalrule", "list"], "GET", "/api/v1/journal-rules", None,
     ["example.com", "archive@compliance.example", "report"]),
    ("journalrule create", ["journalrule", "create", "--set", "scope=account", "--set", "target=ann@example.com",
                            "--set", "journal_address=copy@second-system.example", "--set", "direction=outbound",
                            "--set", "mode=copy"],
     "POST", "/api/v1/journal-rules",
     {"scope": "account", "target": "ann@example.com", "journal_address": "copy@second-system.example",
      "direction": "outbound", "mode": "copy"},
     ["ann@example.com", "copy@second-system.example"]),
    ("journalrule update", ["journalrule", "update", "601", "--set", "enabled=false"],
     "PUT", "/api/v1/journal-rules/601", {"enabled": False}, ["example.com"]),
    ("journalrule delete", ["journalrule", "delete", "601"],
     "DELETE", "/api/v1/journal-rules/601", None, ["deleted"]),
    ("mailbox move --drop-alias",
     ["mailbox", "move", "ann@example.com", "ann@example.net", "--drop-alias"],
     "POST", "/api/v1/accounts/ann@example.com/move",
     {"new_address": "ann@example.net", "keep_alias": False}, ["moved to ann@example.net"]),
    ("alias list under its domain", ["alias", "list", "--domain", "example.com"],
     "GET", "/api/v1/domains/example.com/aliases", None, ["sales@example.com"]),
    ("list list shows the membership query beside the list", ["list", "list", "--domain", "example.com"],
     "GET", "/api/v1/domains/example.com/lists", None, ["all@example.com", "domain"]),
    ("listmember list is the rows and the query's answers, each said which",
     ["listmember", "list", "--address", "all@example.com"],
     "GET", "/api/v1/lists/all@example.com/members", None, ["ann@example.com", "row", "bob@example.com", "query"]),
    ("group list", ["group", "list"], "GET", "/api/v1/groups", None, ["support"]),
    ("group members", ["member", "1", "list"], "GET", "/api/v1/groups/1/members", None, ["ann@example.com"]),
    ("group member add by address", ["member", "1", "add", "bob@example.com"],
     "POST", "/api/v1/groups/1/members", {"address": "bob@example.com"}, ["bob@example.com"]),
    ("queue list", ["queue", "list"], "GET", "/api/v1/queue", None, ["far@away.test"]),
    ("queue retry", ["queue", "retry", "12"], "POST", "/api/v1/queue/12/retry", None, ["queued"]),
    ("privacy export", ["privacy", "export", "ann@example.com", "--archive", "C:\\exports\\ann.zip"],
     "POST", "/api/v1/accounts/ann@example.com/export", {"archive": "C:\\exports\\ann.zip"}, ["exported 3 message(s)", "1 contact(s)"]),
    ("privacy erase", ["privacy", "erase", "ann@example.com", "--archive", "--logs"],
     "POST", "/api/v1/addresses/erase", {"address": "ann@example.com", "include_archive": True, "include_logs": True}, ["erased 7 trace(s)"]),
    ("quarantine list", ["quarantine", "list"], "GET", "/api/v1/quarantine", None, ["spam@away.test"]),
    ("quarantine release", ["quarantine", "release", "3"], "POST", "/api/v1/quarantine/3/release", None, ["released"]),
    ("settings get", ["settings", "get"], "GET", "/api/v1/settings", None, ["max_message_size_kb"]),
    ("settings get one", ["settings", "get", "settings", "max_message_size_kb"], "GET", "/api/v1/settings", None, ["20480"]),
    ("settings get a group", ["settings", "get", "antispam"], "GET", "/api/v1/settings/antispam", None, ["use_spf"]),
    ("settings set", ["settings", "set", "--set", "max_message_size_kb=51200"],
     "PUT", "/api/v1/settings", {"max_message_size_kb": 51200}, []),
    ("settings set a group", ["settings", "set", "antispam", "--set", "use_spf=false"],
     "PUT", "/api/v1/settings/antispam", {"use_spf": False}, []),
    ("settings breach-test", ["settings", "breach-test", "--endpoint", "https://mirror.example/range/"],
     "POST", "/api/v1/settings/breach-check/test", {"url": "https://mirror.example/range/"}, ["812 entries", "padded"]),
    ("settings breach-test, configured endpoint", ["settings", "breach-test"],
     "POST", "/api/v1/settings/breach-check/test", {}, ["reachable"]),
    ("complaints of a domain", ["complaint", "list", "--domain", "example.com"],
     "GET", "/api/v1/domains/example.com/complaints", None, ["reader@provider.test", "abuse", "SomeGenerator/1.0"]),
    ("complaints of one sender", ["complaint", "list", "--domain", "example.com", "--search", "anna"],
     "GET", "/api/v1/domains/example.com/complaints", None, ["reader@provider.test"]),
    ("complaint rates", ["complaints", "rates"], "GET", "/api/v1/complaints/rates", None,
     ["example.com", "250 in a million", "too little sent to judge"]),
    ("complaints expire", ["complaints", "expire"], "POST", "/api/v1/complaints/expire", None,
     ["3 old complaints"]),
    ("dmarc summary", ["dmarc", "summary", "example.com"], "GET", "/api/v1/domains/example.com/dmarc-reports/summary", None,
     ["1 report(s) from 1 reporter(s)", "15 message(s), 12 passed DMARC, 3 failed", "192.0.2.10", "198.51.100.7"]),
    ("dmarc summary --days", ["dmarc", "summary", "example.com", "--days", "90"], "GET", "/api/v1/domains/example.com/dmarc-reports/summary", None,
     ["192.0.2.10"]),
    ("dmarc reports", ["dmarc", "reports", "example.com"], "GET", "/api/v1/domains/example.com/dmarc-reports", None,
     ["google.com", "quarantine", "12345678901234567890", "MESSAGE_DMARC", "pass"]),
    ("dmarc report", ["dmarc", "report", "example.com", "31"], "GET", "/api/v1/domains/example.com/dmarc-reports/31", None,
     ["p=quarantine sp=reject adkim=r aspf=r pct=100", "198.51.100.7", "d=forger.test scope=mfrom result=pass"]),
    ("dmarc tls", ["dmarc", "tls", "example.com"], "GET", "/api/v1/domains/example.com/tls-reports", None,
     ["5326 session(s) succeeded and 303 failed", "Company-X", "sts"]),
    ("dmarc expire", ["dmarc", "expire"], "POST", "/api/v1/domain-reports/expire", None,
     ["2 DMARC report(s) and 1 TLS-RPT report(s) read more than 365 days ago removed."]),
    ("dmarc expire --older-than-days", ["dmarc", "expire", "--older-than-days", "0"], "POST", "/api/v1/domain-reports/expire",
     {"older_than_days": 0}, ["2 DMARC report(s)"]),
    ("settings get complaints", ["settings", "get", "complaints"], "GET", "/api/v1/settings/complaints", None,
     ["suppression_threshold"]),
    ("settings set complaints", ["settings", "set", "complaints", "--set", "processing_enabled=true",
                                 "--set-string", "mailbox=fbl@example.com"],
     "PUT", "/api/v1/settings/complaints", {"processing_enabled": True, "mailbox": "fbl@example.com"}, []),
    ("ini list", ["ini", "list"], "GET", "/api/v1/settings/ini", None, ["DisableAUTHList", "OAuth2HmacSecret", "(set)"]),
    ("ini list --json", ["ini", "list", "--json"], "GET", "/api/v1/settings/ini", None, ['"name": "DisableAUTHList"', '"value": "0"']),
    ("ini set", ["ini", "set", "DisableAUTHList", "1"],
     "PUT", "/api/v1/settings/ini/DisableAUTHList", {"value": "1"}, []),
    ("config changed", ["config", "changed"], "GET", "/api/v1/settings/changed", None,
     ["LogLevel", "9", "database", "smtpnoofretries", "5 of 391 settings"]),
    ("config changed says when the file is forcing a value, which nothing else shows", ["config", "changed"],
     "GET", "/api/v1/settings/changed", None, ["DnsServer", "forced from [SettingsOverride]"]),
    ("config changed names a secret and never its value", ["config", "changed"],
     "GET", "/api/v1/settings/changed", None, ["OAuth2HmacSecret", "(set)"]),
    ("config changed says when it has no default rather than showing an empty column", ["config", "changed"],
     "GET", "/api/v1/settings/changed", None, ["OurOwnScriptKey", "(none: this server does not read it)"]),
    ("config changed --json is the server's own document", ["config", "changed", "--json"],
     "GET", "/api/v1/settings/changed", None, ['"considered": 391', '"changed": 5']),
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
    ("devices of an account", ["device", "ann@example.com", "list"],
     "GET", "/api/v1/accounts/ann@example.com/devices", None, ["imap", "203.0.113.9", "phone", "fedcba9876543210fedcba9876543210"]),
    ("device revoke", ["device", "ann@example.com", "revoke", "0123456789abcdef0123456789abcdef"],
     "DELETE", "/api/v1/accounts/ann@example.com/devices/0123456789abcdef0123456789abcdef", None,
     ["1 connection(s) dropped"]),
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
    ("suppression list", ["suppression", "list", "--domain", "example.com"],
     "GET", "/api/v1/domains/example.com/suppressions", None, ["gone@elsewhere.test", "hard", "5.1.1"]),
    ("suppression create", ["suppression", "create", "--domain", "example.com", "--set", "address=old@elsewhere.test",
                            "--set-string", "note=old export", "--set", "expires_days=90"],
     "POST", "/api/v1/domains/example.com/suppressions",
     {"address": "old@elsewhere.test", "note": "old export", "expires_days": 90}, ["old@elsewhere.test"]),
    ("suppression delete", ["suppression", "delete", "gone@elsewhere.test", "--domain", "example.com"],
     "DELETE", "/api/v1/domains/example.com/suppressions/gone@elsewhere.test", None, ["deleted"]),
    ("openpgp list", ["openpgp", "list", "example.com"],
     "GET", "/api/v1/domains/example.com/openpgp", None,
     ["ann@example.com", "c29b6cce361e73ba13724cb8bbee452b9b0a0bb7", "EdDSA"]),
    ("openpgp show", ["openpgp", "show", "ann@example.com"],
     "GET", "/api/v1/accounts/ann@example.com/openpgp", None, ["hashed_local_part", "ftnwpzeeynpbixr6qyd8grpy8xuzcqwt"]),
    ("openpgp store", ["openpgp", "store", "ann@example.com", KEY_FILE, "--publish"],
     "PUT", "/api/v1/accounts/ann@example.com/openpgp", {"key": KEY_TEXT, "publish": True},
     ["stored 800b26ccc36e0e3b4a61a26b19fe7a2fe905df20", "published", "/.well-known/openpgpkey/"]),
    ("openpgp remove", ["openpgp", "remove", "ann@example.com", "800b26ccc36e0e3b4a61a26b19fe7a2fe905df20"],
     "DELETE", "/api/v1/accounts/ann@example.com/openpgp/800b26ccc36e0e3b4a61a26b19fe7a2fe905df20", None,
     ["removed 800b26ccc36e0e3b4a61a26b19fe7a2fe905df20"]),
    ("openpgp publish", ["openpgp", "publish", "ann@example.com", "on"],
     "PUT", "/api/v1/accounts/ann@example.com", {"openpgp_publish": True}, ["ann@example.com"]),
    ("settings get openpgp", ["settings", "get", "openpgp"], "GET", "/api/v1/settings/openpgp", None,
     ["web_key_directory_enabled"]),
    ("settings set openpgp", ["settings", "set", "openpgp", "--set", "web_key_directory_enabled=true",
                              "--set", "keys_per_account=4"],
     "PUT", "/api/v1/settings/openpgp", {"web_key_directory_enabled": True, "keys_per_account": 4}, []),
    ("bounce counts", ["bounce", "list", "--domain", "example.com"],
     "GET", "/api/v1/domains/example.com/bounces", None, ["gone@elsewhere.test", "CONSECUTIVE_HARD"]),
    ("bounces classify", ["bounces", "classify", "550 <gone@elsewhere.test>"],
     "POST", "/api/v1/bounces/classify", {"status": "550 <gone@elsewhere.test>", "diagnostic": ""},
     ["other", "not trusted"]),
    ("bounces expire", ["bounces", "expire"], "POST", "/api/v1/bounces/expire", None,
     ["2 expired entries", "5 old counts", "40 old report keys"]),
    ("settings get bounces", ["settings", "get", "bounces"], "GET", "/api/v1/settings/bounces", None,
     ["hard_bounce_threshold"]),
    ("settings set bounces", ["settings", "set", "bounces", "--set", "processing_enabled=true",
                              "--set", "suppression_action=observe"],
     "PUT", "/api/v1/settings/bounces", {"processing_enabled": True, "suppression_action": "observe"}, []),
    ("webhook list", ["webhook", "list"], "GET", "/api/v1/webhooks", None,
     ["ticketing", "https://tickets.example/hook", "DEAD_LETTERED"]),
    ("webhook get", ["webhook", "get", "ticketing"], "GET", "/api/v1/webhooks/ticketing", None, ["secret_set"]),
    ("webhook create", ["webhook", "create", "--set", "name=billing", "--set", "url=https://hooks.example/mail",
                        "--set-string", "secret=a-shared-secret"],
     "POST", "/api/v1/webhooks",
     {"name": "billing", "url": "https://hooks.example/mail", "secret": "a-shared-secret"}, ["billing"]),
    ("webhook update", ["webhook", "update", "ticketing", "--set", "enabled=false"],
     "PUT", "/api/v1/webhooks/ticketing", {"enabled": False}, []),
    ("webhook delete", ["webhook", "delete", "ticketing"], "DELETE", "/api/v1/webhooks/ticketing", None, ["deleted"]),
    ("webhooks deliveries", ["webhooks", "deliveries", "--state", "dead-lettered"],
     "GET", "/api/v1/webhooks/deliveries", None, ["ticketing", "dead-lettered", "502"]),
    ("webhooks run", ["webhooks", "run"], "POST", "/api/v1/webhooks/run", {}, ["2 delivered, 7 removed"]),
    ("webhooks test", ["webhooks", "test", "--event", "message.bounced", "--domain", "example.com"],
     "POST", "/api/v1/webhooks/test", {"event": "message.bounced", "domain": "example.com"},
     ["Queued for 1 subscription."]),
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
    ("backup restore", ["backup", "restore", "/var/backups/one.7z", "--domains", "--messages",
                        "--passphrase-file", BACKUP_PASSPHRASE_FILE],
     "POST", "/api/v1/backup/restore",
     {"file": "/var/backups/one.7z", "restore_settings": False, "restore_domains": True,
      "restore_messages": True, "passphrase": BACKUP_PASSPHRASE},
     ["restore started", "(encrypted)", "domains, messages"]),
    ("backup passphrase set", ["backup", "passphrase", "set", "--passphrase-file", BACKUP_PASSPHRASE_FILE],
     "PUT", "/api/v1/settings/backup", {"encryption_passphrase": BACKUP_PASSPHRASE},
     ["passphrase is set", "cannot be read by anybody"]),
    ("backup passphrase clear", ["backup", "passphrase", "clear"],
     "PUT", "/api/v1/settings/backup", {"encryption_passphrase": ""}, ["cleared"]),
    ("backup archives", ["backup", "archives"], "GET", "/api/v1/backup/archives", None,
     ["HMBackup 2026-09-21 020000.7z", "(encrypted)"]),
    ("backup mailbox", ["backup", "mailbox", "HMBackup 2026-09-21 020000.7z", "ann@example.com"],
     "GET", "/api/v1/backup/HMBackup 2026-09-21 020000.7z/mailboxes/ann@example.com", None,
     ["1 message(s) in 1 folder(s)", "4101", "The Tuesday figures"]),
    ("backup restore-mailbox --replace", ["backup", "restore-mailbox", "HMBackup 2026-09-21 020000.7z", "ann@example.com",
                                         "--replace", "--passphrase-file", BACKUP_PASSPHRASE_FILE],
     "POST", "/api/v1/backup/HMBackup 2026-09-21 020000.7z/mailboxes/ann@example.com/restore",
     {"mode": "replace", "passphrase": BACKUP_PASSPHRASE}, ["replace of the mailbox", "job 7", "restore-status 7"]),
    ("backup restore-mailbox --message", ["backup", "restore-mailbox", "HMBackup 2026-09-21 020000.7z", "ann@example.com",
                                         "--message", "4101", "--folder", "Restored"],
     "POST", "/api/v1/backup/HMBackup 2026-09-21 020000.7z/mailboxes/ann@example.com/restore",
     {"message": 4101, "folder": "Restored"}, ["message 4101", "into Restored", "job 7"]),
    ("backup restore-status", ["backup", "restore-status", "7"], "GET", "/api/v1/backup/restore-mailbox/7", None,
     ["job 7 is done", "12 added", "4 skipped"]),
    ("dkim run --seconds-ahead", ["dkim", "run", "--seconds-ahead", "262800"],
     "POST", "/api/v1/dkim/rotation/run", {"seconds_ahead": 262800}, ["as_of"]),
    ("reputation status", ["reputation", "status"], "GET", "/api/v1/reputation", None,
     ["192.0.2.25", "not_listed", "not looked up", "10.0.0.5"]),
    ("reputation check", ["reputation", "check"], "POST", "/api/v1/reputation/check", {},
     ["bl.example.test", "lookup(s)"]),
    ("sample-domain status", ["sample-domain", "status"], "GET", "/api/v1/setup/sample-domain", None,
     ["example.test", "present", "alice@example.test", "everyone@example.test", "shown once"]),
    ("sample-domain create", ["sample-domain", "create"], "POST", "/api/v1/setup/sample-domain", {},
     ["was created with 3 mailboxes", "7TQ4M-KJ9XD-2VWHF-BNRE5", "before this server takes real mail"]),
    ("sample-domain remove", ["sample-domain", "remove"], "DELETE", "/api/v1/setup/sample-domain", None,
     ["was removed", "14 message(s) went with it"]),
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

    # The sample domain refused: 409 because it is already there. Exit 2 rather
    # than 1, which is what the docstring, CommandLine.md and the Linux server's
    # own --sample-domain all promise - and what lets a script that runs create at
    # the start of a demonstration tell "already there" from "could not reach the
    # server", which is also exit 1 everywhere else.
    completed = run(["sample-domain", "create"])
    check(completed.returncode == 2 and "already here" in completed.stderr,
          "hmctl sample-domain create refused with 409 should exit 2 and print the server's sentence, "
          "exited %d and said: %s%s" % (completed.returncode, completed.stdout, completed.stderr))

    # A restore with nothing chosen, and a passphrase asked for as an argument:
    # the two ways somebody reaches for this command and gets it wrong.
    # One message takes no mode: the client refuses the pair before any request.
    completed = run(["backup", "restore-mailbox", "HMBackup 2026-09-21 020000.7z", "ann@example.com", "--message", "4101", "--replace"])
    check(completed.returncode != 0 and "does not apply to a single message" in (completed.stderr + completed.stdout),
          "hmctl backup restore-mailbox --message with --replace should refuse the pair, said: %s%s"
          % (completed.stdout, completed.stderr))

    completed = run(["backup", "restore", "/var/backups/one.7z"])
    check(completed.returncode != 0 and "--domains" in (completed.stdout + completed.stderr),
          "hmctl backup restore with nothing chosen should say which flags to choose from, said: %s%s"
          % (completed.stdout, completed.stderr))

    completed = run(["backup", "passphrase", "set"])
    check(completed.returncode != 0 and "--passphrase-file" in (completed.stdout + completed.stderr)
          and "process list" in (completed.stdout + completed.stderr),
          "hmctl backup passphrase set with no passphrase should say how to give one and why not as an argument, said: %s%s"
          % (completed.stdout, completed.stderr))

    completed = run(["domain", "get", "nothing.here"])
    check(completed.returncode == 4,
          "hmctl on a 404 should exit 4, exited %d" % completed.returncode)

    # The breach check's endpoint test answers 200 whether or not the endpoint
    # could be used; hmctl turns "not reachable" into exit 1, which is what a
    # monitoring job reads.
    key = ("POST", "/api/v1/settings/breach-check/test")
    ("Get-HmComplaint", "Get-HmComplaint -Domain example.com | Select-Object -ExpandProperty address",
     "GET", "/api/v1/domains/example.com/complaints", None, ["reader@provider.test"]),
    ("Get-HmComplaintRate", "Get-HmComplaintRate | Select-Object -First 1 -ExpandProperty rate_ppm",
     "GET", "/api/v1/complaints/rates", None, ["250"]),
    ("Invoke-HmComplaintMaintenance", "Invoke-HmComplaintMaintenance -Confirm:$false | Select-Object -ExpandProperty complaints_pruned",
     "POST", "/api/v1/complaints/expire", None, ["3"]),
    ("Get-HmDmarcSummary", "Get-HmDmarcSummary -Domain example.com | Select-Object -ExpandProperty messages",
     "GET", "/api/v1/domains/example.com/dmarc-reports/summary", None, ["15"]),
    ("Get-HmDmarcReport", "Get-HmDmarcReport -Domain example.com | Select-Object -ExpandProperty org",
     "GET", "/api/v1/domains/example.com/dmarc-reports", None, ["google.com"]),
    ("Get-HmDmarcReport -Id", "(Get-HmDmarcReport -Domain example.com -Id 31).records | Select-Object -First 1 -ExpandProperty source_ip",
     "GET", "/api/v1/domains/example.com/dmarc-reports/31", None, ["192.0.2.10"]),
    ("Get-HmTlsReport", "Get-HmTlsReport -Domain example.com | Select-Object -ExpandProperty successful",
     "GET", "/api/v1/domains/example.com/tls-reports", None, ["5326"]),
    ("Invoke-HmDomainReportMaintenance", "Invoke-HmDomainReportMaintenance -Confirm:$false | Select-Object -ExpandProperty dmarc_reports_pruned",
     "POST", "/api/v1/domain-reports/expire", None, ["2"]),
    ("Invoke-HmDomainReportMaintenance -OlderThanDays", "Invoke-HmDomainReportMaintenance -OlderThanDays 0 -Confirm:$false | Select-Object -ExpandProperty tls_reports_pruned",
     "POST", "/api/v1/domain-reports/expire", {"older_than_days": 0}, ["1"]),
    ("Get-HmSetting -Group complaints", "Get-HmSetting -Group complaints -Name suppression_threshold",
     "GET", "/api/v1/settings/complaints", None, ["0"]),
    reachable = ANSWERS[key]
    ANSWERS[key] = {"reachable": False, "status": 0, "error": "The request to mirror.example failed: refused", "milliseconds": 2003}
    try:
        completed = run(["settings", "breach-test"])
    finally:
        ANSWERS[key] = reachable
    check(completed.returncode == 1 and "not reachable" in completed.stdout and "mirror.example failed" in completed.stdout,
          "hmctl settings breach-test against an endpoint that could not be used should say why and exit 1, exited %d: %s%s"
          % (completed.returncode, completed.stdout, completed.stderr))

    # A listing is what the exit code is for: a monitoring job runs this and reads
    # the code rather than the table. An unanswered list counts the same way.
    key = ("GET", "/api/v1/reputation")
    clean = ANSWERS[key]
    listed = json.loads(json.dumps(clean))
    listed["addresses"][0]["results"][0] = {"list": "bl.example.test", "status": "listed", "listed": True,
                                            "code": "127.0.0.2", "reason": "Listed for sending unsolicited mail",
                                            "checked": 1789000000, "listed_since": 1788900000}
    listed["addresses"][0]["listed"] = True
    listed["counts"]["listed"] = 1
    ANSWERS[key] = listed
    try:
        completed = run(["reputation", "status"])
    finally:
        ANSWERS[key] = clean
    check(completed.returncode == 1 and "listed" in completed.stdout and "127.0.0.2" in completed.stdout,
          "hmctl reputation status with an address on a list should print it and exit 1, exited %d: %s%s"
          % (completed.returncode, completed.stdout, completed.stderr))

    # And a pair no check has covered - a list or an address added since the last
    # one - is not clean either. The report says so in its own words; the exit code
    # has to say it too, because nothing else a monitoring job reads does.
    unchecked = json.loads(json.dumps(clean))
    unchecked["addresses"][0]["results"][0] = {"list": "added.example.test", "status": "not_checked",
                                              "listed": None, "code": "", "reason": "",
                                              "checked": 0, "listed_since": 0}
    unchecked["addresses"][0]["listed"] = None
    unchecked["counts"]["not_listed"] = 2
    unchecked["counts"]["not_checked"] = 1
    ANSWERS[key] = unchecked
    try:
        completed = run(["reputation", "status"])
    finally:
        ANSWERS[key] = clean
    check(completed.returncode == 1 and "not_checked" in completed.stdout,
          "hmctl reputation status with a pair no check has covered should print it and exit 1, exited %d: %s%s"
          % (completed.returncode, completed.stdout, completed.stderr))

    # Unless the watch is off, which is how every upgraded server starts: the
    # lists are seeded and nothing is checked against them, so every pair is
    # not_checked for ever. Alarming at every invocation, with nothing in the
    # table saying why, is what this case exists to stop.
    off = json.loads(json.dumps(unchecked))
    off["enabled"] = False
    ANSWERS[key] = off
    try:
        completed = run(["reputation", "status"])
    finally:
        ANSWERS[key] = clean
    check(completed.returncode == 0 and "the watch is off" in completed.stdout
          and "reputation_watch_enabled" in completed.stdout,
          "hmctl reputation status with the watch off should say so and exit 0, exited %d: %s%s"
          % (completed.returncode, completed.stdout, completed.stderr))

    # But an answer an earlier check got does not stop being true because the
    # watch was switched off afterwards.
    offListed = json.loads(json.dumps(listed))
    offListed["enabled"] = False
    ANSWERS[key] = offListed
    try:
        completed = run(["reputation", "status"])
    finally:
        ANSWERS[key] = clean
    check(completed.returncode == 1,
          "hmctl reputation status with the watch off and an address listed should still exit 1, exited %d: %s%s"
          % (completed.returncode, completed.stdout, completed.stderr))

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
    ("Export-HmAccountArchive", "Export-HmAccountArchive -Address ann@example.com -Path 'C:\\exports\\ann.zip' | Select-Object -ExpandProperty exported",
     "POST", "/api/v1/accounts/ann@example.com/export", {"archive": "C:\\exports\\ann.zip"}, ["3"]),
    ("Remove-HmAddressTraces", "Remove-HmAddressTraces -Address ann@example.com -IncludeArchive -IncludeLogs -Confirm:$false | Select-Object -ExpandProperty removed",
     "POST", "/api/v1/addresses/erase", {"address": "ann@example.com", "include_archive": True, "include_logs": True}, ["7"]),
    ("Remove-HmDomain", "Remove-HmDomain -Name example.net -Confirm:$false; 'removed'",
     "DELETE", "/api/v1/domains/example.net", None, ["removed"]),
    ("Get-HmAccount", "Get-HmAccount -Domain example.com | Select-Object -ExpandProperty address",
     "GET", "/api/v1/domains/example.com/accounts", None, ["ann@example.com"]),
    ("New-HmAccount", "New-HmAccount -Domain example.com -Address new@example.com -Password secret | Out-String",
     "POST", "/api/v1/domains/example.com/accounts", {"address": "new@example.com", "password": "secret"}, ["new@example.com"]),
    ("Set-HmAccount", "Set-HmAccount -Address ann@example.com -Property @{active=$false} | Out-String",
     "PUT", "/api/v1/accounts/ann@example.com", {"active": False}, []),
    ("Get-HmMailboxMove", "Get-HmMailboxMove -Address ann@example.com | Select-Object -ExpandProperty in_use",
     "GET", "/api/v1/accounts/ann@example.com/move", None, ["False"]),
    ("Move-HmMailbox", "(Move-HmMailbox -Address ann@example.com -NewAddress ann@example.net -Confirm:$false).new_address",
     "POST", "/api/v1/accounts/ann@example.com/move",
     {"new_address": "ann@example.net", "keep_alias": True}, ["ann@example.net"]),
    ("Move-HmMailbox -DropAlias", "(Move-HmMailbox -Address ann@example.com -NewAddress ann@example.net -DropAlias -Confirm:$false).new_address",
     "POST", "/api/v1/accounts/ann@example.com/move",
     {"new_address": "ann@example.net", "keep_alias": False}, ["ann@example.net"]),
    # -WhatIf must send nothing at all, which is asserted by the read before it
    # still being the last request the server saw.
    ("Move-HmMailbox -WhatIf sends nothing",
     "Get-HmMailboxMove -Address ann@example.com | Out-Null; Move-HmMailbox -Address ann@example.com -NewAddress ann@example.net -WhatIf; 'nothing was sent'",
     "GET", "/api/v1/accounts/ann@example.com/move", None, ["nothing was sent"]),
    ("Get-HmSetting", "Get-HmSetting | Select-Object -ExpandProperty max_message_size_kb",
     "GET", "/api/v1/settings", None, ["20480"]),
    ("Get-HmIniSetting", "Get-HmIniSetting | ForEach-Object { $_.name + '=' + $_.value }",
     "GET", "/api/v1/settings/ini", None, ["DisableAUTHList=0", "OAuth2HmacSecret=(set)"]),
    ("Get-HmChangedSetting", "Get-HmChangedSetting | ForEach-Object { $_.store + ' ' + $_.name + '=' + $_.value }",
     "GET", "/api/v1/settings/changed", None, ["ini LogLevel=5", "database smtpnoofretries=6"]),
    ("Get-HmChangedSetting -Summary", "Get-HmChangedSetting -Summary | Select-Object -ExpandProperty considered",
     "GET", "/api/v1/settings/changed", None, ["391"]),
    ("Set-HmSetting", "Set-HmSetting -Property @{max_message_size_kb=51200} | Out-String",
     "PUT", "/api/v1/settings", {"max_message_size_kb": 51200}, []),
    ("Set-HmSetting -Group", "Set-HmSetting -Group antispam -Property @{use_spf=$false} | Out-String",
     "PUT", "/api/v1/settings/antispam", {"use_spf": False}, []),
    ("Test-HmBreachCheckEndpoint", "Test-HmBreachCheckEndpoint -Url https://mirror.example/range/ | Select-Object -ExpandProperty entries",
     "POST", "/api/v1/settings/breach-check/test", {"url": "https://mirror.example/range/"}, ["812"]),
    ("Get-HmQueue", "Get-HmQueue | Select-Object -ExpandProperty to_address", "GET", "/api/v1/queue", None, ["far@away.test"]),
    ("Get-HmStatus", "Get-HmStatus | Select-Object -ExpandProperty version", "GET", "/api/v1/status", None, ["6.3.3"]),
    ("Get-HmReputation", "Get-HmReputation | Select-Object -ExpandProperty last_checked",
     "GET", "/api/v1/reputation", None, ["1789000000"]),
    ("Get-HmReputation -Results",
     "Get-HmReputation -Results | Where-Object Status -eq not_applicable | ForEach-Object { $_.Address + ' ' + $_.List }",
     "GET", "/api/v1/reputation", None, ["2001:db8::25 ipv4.example.test"]),
    ("Invoke-HmReputationCheck", "(Invoke-HmReputationCheck).run.lookups",
     "POST", "/api/v1/reputation/check", {}, ["8"]),
    ("Get-HmDevice", "Get-HmDevice -Address ann@example.com | ForEach-Object { $_.protocol + ' ' + $_.credential_name }",
     "GET", "/api/v1/accounts/ann@example.com/devices", None, ["imap phone", "web "]),
    ("Revoke-HmDevice",
     "(Revoke-HmDevice -Address ann@example.com -DeviceId 0123456789abcdef0123456789abcdef -Confirm:$false).connections_dropped",
     "DELETE", "/api/v1/accounts/ann@example.com/devices/0123456789abcdef0123456789abcdef", None, ["1"]),
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
    ("Get-HmSuppression", "Get-HmSuppression -Domain example.com | Select-Object -ExpandProperty address",
     "GET", "/api/v1/domains/example.com/suppressions", None, ["gone@elsewhere.test"]),
    ("Add-HmSuppression", "Add-HmSuppression -Domain example.com -Address old@elsewhere.test -Note 'old export' -ExpiresDays 90 | Select-Object -ExpandProperty expires",
     "POST", "/api/v1/domains/example.com/suppressions",
     {"address": "old@elsewhere.test", "note": "old export", "expires_days": 90}, ["2026-12-14"]),
    ("Remove-HmSuppression", "Remove-HmSuppression -Domain example.com -Address gone@elsewhere.test -Confirm:$false; 'removed'",
     "DELETE", "/api/v1/domains/example.com/suppressions/gone@elsewhere.test", None, ["removed"]),
    ("Get-HmBounceCount", "Get-HmBounceCount -Domain example.com | Select-Object -ExpandProperty consecutive_hard",
     "GET", "/api/v1/domains/example.com/bounces", None, ["2"]),
    ("Test-HmBounceStatus", "Test-HmBounceStatus -Status '550 <gone@elsewhere.test>' | Select-Object -ExpandProperty category",
     "POST", "/api/v1/bounces/classify", {"status": "550 <gone@elsewhere.test>", "diagnostic": ""}, ["other"]),
    ("Invoke-HmBounceMaintenance", "Invoke-HmBounceMaintenance -Confirm:$false | Select-Object -ExpandProperty report_keys_pruned",
     "POST", "/api/v1/bounces/expire", None, ["40"]),
    ("Get-HmWebhook", "Get-HmWebhook | Select-Object -ExpandProperty name",
     "GET", "/api/v1/webhooks", None, ["ticketing"]),
    ("New-HmWebhook", "New-HmWebhook -Name billing -Url https://hooks.example/mail -Secret 'a-shared-secret' -Confirm:$false | Select-Object -ExpandProperty name",
     "POST", "/api/v1/webhooks",
     {"name": "billing", "url": "https://hooks.example/mail", "secret": "a-shared-secret",
      "enabled": True, "include_subject": False, "max_per_minute": 0}, ["billing"]),
    ("Set-HmWebhook sends only what was given",
     "Set-HmWebhook -Name ticketing -Enabled $false -Confirm:$false | Select-Object -ExpandProperty enabled",
     "PUT", "/api/v1/webhooks/ticketing", {"enabled": False}, ["False"]),
    ("Remove-HmWebhook", "Remove-HmWebhook -Name ticketing -Confirm:$false; 'removed'",
     "DELETE", "/api/v1/webhooks/ticketing", None, ["removed"]),
    ("Get-HmWebhookDelivery", "Get-HmWebhookDelivery -State dead-lettered | Select-Object -ExpandProperty error",
     "GET", "/api/v1/webhooks/deliveries", None, ["502"]),
    ("Invoke-HmWebhookDelivery", "(Invoke-HmWebhookDelivery -Confirm:$false).delivered",
     "POST", "/api/v1/webhooks/run", {}, ["2"]),
    ("Test-HmWebhook", "(Test-HmWebhook -Event message.bounced -Domain example.com -Confirm:$false).queued",
     "POST", "/api/v1/webhooks/test", {"event": "message.bounced", "domain": "example.com"}, ["1"]),
    ("Get-HmSetting -Group bounces", "Get-HmSetting -Group bounces -Name suppression_action",
     "GET", "/api/v1/settings/bounces", None, ["refuse"]),
    ("Get-HmOpenPgpKey -Domain", "Get-HmOpenPgpKey -Domain example.com | Select-Object -ExpandProperty fingerprint",
     "GET", "/api/v1/domains/example.com/openpgp", None, ["c29b6cce361e73ba13724cb8bbee452b9b0a0bb7"]),
    ("Get-HmOpenPgpKey -Account", "(Get-HmOpenPgpKey -Account ann@example.com).hashed_local_part",
     "GET", "/api/v1/accounts/ann@example.com/openpgp", None, ["ftnwpzeeynpbixr6qyd8grpy8xuzcqwt"]),
    ("Set-HmOpenPgpKey", "(Set-HmOpenPgpKey -Account ann@example.com -Path '" + KEY_FILE.replace("\\", "\\\\") + "' -Publish -Confirm:$false).fingerprint",
     "PUT", "/api/v1/accounts/ann@example.com/openpgp", {"key": KEY_TEXT, "publish": True},
     ["800b26ccc36e0e3b4a61a26b19fe7a2fe905df20"]),
    ("Remove-HmOpenPgpKey", "Remove-HmOpenPgpKey -Account ann@example.com -Fingerprint 800b26ccc36e0e3b4a61a26b19fe7a2fe905df20 -Confirm:$false; 'removed'",
     "DELETE", "/api/v1/accounts/ann@example.com/openpgp/800b26ccc36e0e3b4a61a26b19fe7a2fe905df20", None, ["removed"]),
    ("Set-HmOpenPgpPublish", "(Set-HmOpenPgpPublish -Account ann@example.com -Enabled $true -Confirm:$false).address",
     "PUT", "/api/v1/accounts/ann@example.com", {"openpgp_publish": True}, ["ann@example.com"]),
    ("Get-HmSetting -Group openpgp", "Get-HmSetting -Group openpgp -Name keys_per_account",
     "GET", "/api/v1/settings/openpgp", None, ["8"]),
    ("Get-HmSpamTrap", "Get-HmSpamTrap -Domain example.com | Select-Object -ExpandProperty address",
     "GET", "/api/v1/domains/example.com/spam-traps", None, ["never-used@example.com"]),
    ("Add-HmSpamTrap", "(Add-HmSpamTrap -Domain example.com -Address sales-2004 -Confirm:$false).address",
     "POST", "/api/v1/domains/example.com/spam-traps", {"address": "sales-2004"}, ["sales-2004@example.com"]),
    ("Remove-HmSpamTrap", "Remove-HmSpamTrap -Address never-used@example.com -Confirm:$false; 'removed'",
     "DELETE", "/api/v1/spam-traps/never-used@example.com", None, ["removed"]),
    ("Get-HmSpamTrapHit", "Get-HmSpamTrapHit | Select-Object -ExpandProperty sender",
     "GET", "/api/v1/spam-traps/hits", None, ["harvester@spammer.example"]),
    ("Get-HmSpamTrapHit -Domain", "Get-HmSpamTrapHit -Domain example.com | Select-Object -ExpandProperty address",
     "GET", "/api/v1/domains/example.com/spam-traps/hits", None, ["never-used@example.com"]),
    ("Clear-HmSpamTrapHit", "(Clear-HmSpamTrapHit -Confirm:$false).removed",
     "DELETE", "/api/v1/spam-traps/hits", None, ["1"]),
    ("Get-HmJournalRule", "Get-HmJournalRule | Select-Object -ExpandProperty journal_address",
     "GET", "/api/v1/journal-rules", None, ["archive@compliance.example"]),
    ("New-HmJournalRule", "(New-HmJournalRule -Target ann@example.com -JournalAddress copy@second-system.example -Scope account -Direction outbound -Mode copy -Confirm:$false).target",
     "POST", "/api/v1/journal-rules",
     {"scope": "account", "target": "ann@example.com", "journal_address": "copy@second-system.example",
      "direction": "outbound", "mode": "copy"},
     ["ann@example.com"]),
    ("Set-HmJournalRule", "(Set-HmJournalRule -Id 601 -Enabled $false -Confirm:$false).enabled",
     "PUT", "/api/v1/journal-rules/601", {"enabled": False}, ["False"]),
    ("Remove-HmJournalRule", "Remove-HmJournalRule -Id 601 -Confirm:$false; 'removed'",
     "DELETE", "/api/v1/journal-rules/601", None, ["removed"]),
    ("Restore-HmBackup",
     "(Restore-HmBackup -File '/var/backups/one.7z' -Domains -Messages "
     "-Passphrase (ConvertTo-SecureString '" + BACKUP_PASSPHRASE + "' -AsPlainText -Force) -Confirm:$false).started",
     "POST", "/api/v1/backup/restore",
     {"file": "/var/backups/one.7z", "restore_settings": False, "restore_domains": True,
      "restore_messages": True, "passphrase": BACKUP_PASSPHRASE}, ["True"]),
    ("Set-HmBackupPassphrase",
     "Set-HmBackupPassphrase -Passphrase (ConvertTo-SecureString '" + BACKUP_PASSPHRASE + "' -AsPlainText -Force) "
     "-Confirm:$false | Select-Object -ExpandProperty encryption_passphrase_set",
     "PUT", "/api/v1/settings/backup", {"encryption_passphrase": BACKUP_PASSPHRASE}, ["True"]),
    ("Set-HmBackupPassphrase -Clear",
     "Set-HmBackupPassphrase -Clear -Confirm:$false | Select-Object -ExpandProperty destination",
     "PUT", "/api/v1/settings/backup", {"encryption_passphrase": ""}, ["/var/backups"]),
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
        # The script runs inside try/catch and the catch writes the exception's own
        # message to stderr itself. PowerShell's error renderer is not merely
        # wrapping a long message at the host width on the Linux runner - it was
        # clipping each line, so words in the middle of the sentence never
        # reached stderr at all and no folding could bring them back.
        wrapped = "try { " + script + " } catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }"
        completed = subprocess.run([pwsh, "-NoProfile", "-NonInteractive", "-Command", prologue + wrapped],
                                   capture_output=True, text=True, timeout=180)
        # PowerShell wraps an error record to the width of the host it is
        # running under: 120 columns on a developer's machine, 80 on the
        # Linux runner. A refusal this file reads as one sentence therefore
        # arrives with a line break inside it in CI and not here, which is how
        # "first_name and last_name" passed on a bench and failed the pull
        # request. The text is folded to single spaces before anything reads
        # it, so these checks are about what the module SAYS and not about
        # where the console chose to break it. Only the ERROR text is folded:
        # the answers on stdout are read line by line by other checks.
        completed.stderr = " ".join(re.sub(r"\[[0-9;]*m", "", completed.stderr or "").split())
        return completed

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
    ini = written.get("sections", {}).get("ini")
    check(isinstance(ini, dict) and ini.get("DisableAUTHList") == "0",
          "the module's document holds no ini keys: %r" % (ini,))
    check(isinstance(ini, dict) and "OAuth2HmacSecret" not in ini,
          "the module's document carries a secret the server had masked: %r" % (ini,))

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

    # example.org's schedule is off, and a rotation started by hand still keeps its
    # old selector, so the server holds its selectors. The document keeps them...
    for row in written.get("sections", {}).get("domains", []):
        if row.get("name") == "example.org":
            check(row.get("dkim_selector") == "s20260901",
                  "the module's document must keep the selector of a domain whose schedule is off: %s" % row)

    # ...and a document from before that rotation's switch matches while it holds them.
    held_path = os.path.join(tmp, "ps-held.json")
    completed = run("$d = Get-Content -Path '%s' -Raw | ConvertFrom-Json; "
                    "$o = $d.sections.domains | Where-Object name -eq 'example.org'; "
                    "$o.dkim_selector = 's20260301'; "
                    "$o.dkim_private_key_file = '/var/lib/hmailserver/DKIM/s20260301._domainkey.example.org.pem'; "
                    "$d | ConvertTo-Json -Depth 20 | Set-Content -Path '%s' -Encoding utf8; "
                    "Compare-HmConfiguration -Path '%s' | Where-Object Key -eq 'example.org' | Measure-Object | Select-Object -ExpandProperty Count"
                    % (document.replace("'", "''"), held_path.replace("'", "''"), held_path.replace("'", "''")))
    check(completed.returncode == 0 and completed.stdout.strip() == "0",
          "Compare-HmConfiguration of an old selector on a domain whose hand-started rotation holds it should be empty, "
          "said %r %s" % (completed.stdout.strip(), completed.stderr.strip()[:300]))

    # With a real change beside it, the change is sent and the selectors are not.
    held_change_path = os.path.join(tmp, "ps-held-change.json")
    completed = run("$d = Get-Content -Path '%s' -Raw | ConvertFrom-Json; "
                    "($d.sections.domains | Where-Object name -eq 'example.org').active = $false; "
                    "$d | ConvertTo-Json -Depth 20 | Set-Content -Path '%s' -Encoding utf8"
                    % (held_path.replace("'", "''"), held_change_path.replace("'", "''")))
    check(completed.returncode == 0, "the module could not write the held document: %s" % completed.stderr.strip()[:300])
    with Recorder.lock:
        Recorder.requests = []
    completed = run("Import-HmConfiguration -Path '%s' -Force -Confirm:$false | Out-Null" % held_change_path.replace("'", "''"))
    with Recorder.lock:
        applying = list(Recorder.requests)
    puts = [r["body"] for r in applying if r["method"] == "PUT" and r["path"] == "/api/v1/domains/example.org"]
    check(completed.returncode == 0 and len(puts) == 1 and isinstance(puts[0], dict) and puts[0].get("active") is False
          and not any(key in puts[0] for key in ("dkim_selector", "dkim_private_key_file",
                                                 "dkim_secondary_selector", "dkim_secondary_private_key_file")),
          "Import-HmConfiguration must send example.org's change without the selectors its rotation holds: exit %d, %s %s"
          % (completed.returncode, puts, completed.stderr.strip()[:300]))

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
    rotating = [row for row in json.loads(first).get("sections", {}).get("domains", []) if row.get("name") == "example.com"]
    check(len(rotating) == 1 and rotating[0].get("dkim_rotation_enabled") is True and "s20260915" not in first
          and not any(key in rotating[0] for key in ROTATION_OWNED_KEYS),
          "a domain whose DKIM key rotation is on must not carry the selector and key files the rotation owns:\n%s"
          % first[:1200])
    check("mx.partner.test" in first, "the document holds no route")

    # The ini section, which was empty against every real server until the
    # recorded server answered the real shape: the value is there, and the
    # secret the server lists as "(set)" is not - neither the name nor the
    # placeholder, which applied would overwrite the secret.
    ini = json.loads(first).get("sections", {}).get("ini")
    check(isinstance(ini, dict) and ini.get("DisableAUTHList") == "0",
          "the document's ini section does not hold DisableAUTHList=0: %r" % (ini,))
    check("OAuth2HmacSecret" not in first and "(set)" not in first,
          "the document carries a secret the server had masked")

    # A spam trap travels as its address and nothing else. The listing also
    # answers local_part - the address up to the @, which the server splits out
    # for a reader - and the create route refuses any field but address, so a
    # document carrying it would be refused by the very server it came from.
    traps = json.loads(first).get("sections", {}).get("spam_traps", [])
    check(any(row.get("address") == "never-used@example.com" and row.get("domain") == "example.com"
              for row in traps), "the document holds no spam trap: %s" % traps)
    check(not any("local_part" in row for row in traps),
          "the document carries a spam trap's local_part, which the create route refuses: %s" % traps)

    trapped = json.loads(first)
    trapped["sections"]["spam_traps"].append({"address": "sales-2004@example.com", "domain": "example.com"})
    trapped_path = os.path.join(tmp, "trapped.json")
    with open(trapped_path, "w", encoding="utf-8") as handle:
        json.dump(trapped, handle, indent=2, sort_keys=True)

    with Recorder.lock:
        Recorder.requests = []
    run(["config", "apply", trapped_path, "--force"])
    with Recorder.lock:
        applying = list(Recorder.requests)
    posts = [r["body"] for r in applying
             if r["method"] == "POST" and r["path"] == "/api/v1/domains/example.com/spam-traps"]
    check(posts == [{"address": "sales-2004@example.com"}],
          "apply --force must declare the document's spam trap with address alone: %s" % posts)

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
    changed["sections"]["ini"]["DisableAUTHList"] = "1"
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
    check("DisableAUTHList" in completed.stdout,
          "the plan does not name the changed ini key:\n%s" % completed.stdout)
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
    check(("PUT", "/api/v1/settings/ini/DisableAUTHList", {"value": "1"}) in writes,
          "the ini key was not written: %s" % writes)
    check(not any(path.endswith("/OAuth2HmacSecret") for _, path, _ in writes),
          "apply wrote the masked secret: %s" % writes)
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

    # example.org's schedule is off, and a rotation started by hand still keeps its
    # old selector, so the server holds its selectors. The document keeps them...
    check('"s20260901"' in first,
          "a domain whose schedule is off must keep its selector in the document:\n%s" % first[:1600])

    # ...a document from before that rotation's switch matches while it holds them...
    held = json.loads(first)
    for row in held["sections"]["domains"]:
        if row.get("name") == "example.org":
            row["dkim_selector"] = "s20260301"
            row["dkim_private_key_file"] = "/var/lib/hmailserver/DKIM/s20260301._domainkey.example.org.pem"
    held_path = os.path.join(tmp, "held.json")
    with open(held_path, "w", encoding="utf-8") as handle:
        json.dump(held, handle, indent=2, sort_keys=True)
    with Recorder.lock:
        Recorder.requests = []
    completed = run(["config", "diff", held_path])
    with Recorder.lock:
        asking = list(Recorder.requests)
    check(completed.returncode == 0 and "already matches" in completed.stdout,
          "an old selector on a domain whose hand-started rotation holds it must match, exit %d:\n%s"
          % (completed.returncode, completed.stdout))
    check(any(r["method"] == "GET" and r["path"] == "/api/v1/domains/example.org/dkim/rotation" for r in asking),
          "config diff did not ask the server whether a rotation holds example.org's selectors: %s"
          % [r["path"] for r in asking])

    # ...and applied with a real change beside it, the change is sent without them.
    for row in held["sections"]["domains"]:
        if row.get("name") == "example.org":
            row["active"] = False
    with open(held_path, "w", encoding="utf-8") as handle:
        json.dump(held, handle, indent=2, sort_keys=True)
    with Recorder.lock:
        Recorder.requests = []
    run(["config", "apply", held_path, "--force"])
    with Recorder.lock:
        applying = list(Recorder.requests)
    puts = [r["body"] for r in applying if r["method"] == "PUT" and r["path"] == "/api/v1/domains/example.org"]
    check(len(puts) == 1 and isinstance(puts[0], dict) and puts[0].get("active") is False
          and not any(key in puts[0] for key in ROTATION_OWNED_KEYS),
          "apply --force must send example.org's change without the selectors its rotation holds: %s" % puts)

    # The negative control: the same selector change on example.net, which no
    # rotation holds, is a difference, and applying it sends the selector.
    loose = json.loads(first)
    for row in loose["sections"]["domains"]:
        if row.get("name") == "example.net":
            row["dkim_selector"] = "s20260301"
    loose_path = os.path.join(tmp, "loose.json")
    with open(loose_path, "w", encoding="utf-8") as handle:
        json.dump(loose, handle, indent=2, sort_keys=True)
    completed = run(["config", "diff", loose_path])
    check(completed.returncode == 1 and "example.net" in completed.stdout,
          "a selector change on a domain no rotation holds must be a difference, exit %d:\n%s"
          % (completed.returncode, completed.stdout))
    with Recorder.lock:
        Recorder.requests = []
    run(["config", "apply", loose_path, "--force"])
    with Recorder.lock:
        applying = list(Recorder.requests)
    puts = [r["body"] for r in applying if r["method"] == "PUT" and r["path"] == "/api/v1/domains/example.net"]
    check(len(puts) == 1 and isinstance(puts[0], dict) and puts[0].get("dkim_selector") == "s20260301",
          "apply --force must send the selector of a domain no rotation holds: %s" % puts)

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


# The spreadsheet both clients import, and exactly what each row must become.
# Every expectation here is a body the create route (ReadNewAccount_) stores:
# a password is a string whatever it looks like, a name goes in first_name and
# last_name, active is a boolean, max_size_mb a number and admin_level one of
# three lower-case words. Seven rows are refused by the client before anything
# is sent, in the create's own words, so a dry run refuses them too; one more is
# sent and refused by the server, and must be reported in the server's words.
#
# A name is held to its 60-character column as the Windows server counts it, in
# UTF-16 code units: LOU_SURNAME is 30 characters outside the Basic Multilingual
# Plane, 60 units, and is sent; MIA_FIRST_NAME is 60 characters and 61 units, and
# is refused. A client that counted characters (Python's len) or bytes would
# disagree with the other client on one of the two.
LOU_SURNAME = "\U0001F600" * 30
MIA_FIRST_NAME = "x" * 59 + "\U0001F600"

IMPORT_CSV = (
    "address,password,first_name,last_name,active,max_size_mb,admin_level\r\n"
    "ann@example.com,secret,Ann,Adams,true,100,domain\r\n"            # exists: left alone
    "carol@example.com,20260915,Carol,Clark,yes,50,DOMAIN\r\n"        # a password of digits alone
    "dave@example.com,true,,,false,,\r\n"                             # a password that reads as a boolean
    "una@example.org,pass word,\u00dana,\"\u00d3 N\u00e9ill, Jr\",TRUE,0,user\r\n"
    "lou@example.com,Lou-Pass-1,," + LOU_SURNAME + ",,,\r\n"          # a surname the full width of its column
    "kim@example.net,Kim-Pass-1,Kim,,,,server\r\n"                    # sent, and refused by the server
    "erin@example.com,Erin-Pass-1,Erin,,maybe,,\r\n"                  # refused: active
    "frank@example.com,,Frank,,,,\r\n"                                # refused: no password
    "gina@example.com,Gina-Pass-1,,,,5.5,\r\n"                        # refused: max_size_mb
    "hal@example.com,Hal-Pass-1,,,,,root\r\n"                         # refused: admin_level
    "ivy@example.com,Ivy-Pass-1,Ivy,Adams, Jr,true,,user\r\n"         # refused: a comma outside quotes
    "mia@example.com,Mia-Pass-1," + MIA_FIRST_NAME + ",,,,\r\n"       # refused: a name one unit too wide
    "ned@example.com, Ned-Pass-1,,,,,\r\n"                            # refused: a space the password would keep
)

IMPORT_BODIES = [
    ("/api/v1/domains/example.com/accounts",
     {"address": "carol@example.com", "password": "20260915", "first_name": "Carol", "last_name": "Clark",
      "active": True, "max_size_mb": 50, "admin_level": "domain"}),
    ("/api/v1/domains/example.com/accounts",
     {"address": "dave@example.com", "password": "true", "active": False}),
    ("/api/v1/domains/example.org/accounts",
     {"address": "una@example.org", "password": "pass word", "first_name": "\u00dana",
      "last_name": "\u00d3 N\u00e9ill, Jr", "active": True, "max_size_mb": 0, "admin_level": "user"}),
    ("/api/v1/domains/example.com/accounts",
     {"address": "lou@example.com", "password": "Lou-Pass-1", "last_name": LOU_SURNAME}),
    ("/api/v1/domains/example.net/accounts",
     {"address": "kim@example.net", "password": "Kim-Pass-1", "first_name": "Kim", "admin_level": "server"}),
]

# The row the server refuses, and the words it must be reported in.
SERVER_REFUSED = ("kim@example.net", "restricted to named domains")

# Each in the words the create answers with, but the last two: the comma
# outside quotes is a fault of the file the server never sees, and a password
# with a space at its edge is one the server would store as sent - the clients
# refuse it rather than trim a password or guess.
IMPORT_REFUSED = {
    "erin@example.com": "active must be true or false",
    "frank@example.com": "address and password are required",
    "gina@example.com": "max_size_mb must be a whole number",
    "hal@example.com": "admin_level must be user, domain or server",
    "mia@example.com": "first_name and last_name are at most 60 characters each",
    "ivy@example.com": "more cells than the header has columns",
    "ned@example.com": "the password begins or ends with white space",
}

EXPORT_HEADER = ["address", "first_name", "last_name", "active", "max_size_mb", "admin_level"]

# The two passwords the round trip adds to the exported file, by address.
ROUND_TRIP_PASSWORDS = {"zoe@example.org": "Round-Trip-1", "yan@example.org": "Round-Trip-2"}


def write_import_files(tmp):
    """The three files every import case reads: the spreadsheet above, one with
    a single name column (which the documentation once showed), and one saved in
    the ANSI code page rather than UTF-8."""
    people = os.path.join(tmp, "people.csv")
    with open(people, "w", encoding="utf-8", newline="") as handle:
        handle.write(IMPORT_CSV)
    named = os.path.join(tmp, "named.csv")
    with open(named, "w", encoding="utf-8", newline="") as handle:
        handle.write("address,password,name\r\nivy@example.com,Ivy-Pass-1,Ivy Iverson\r\n")
    ansi = os.path.join(tmp, "ansi.csv")
    with open(ansi, "wb") as handle:
        handle.write(b"address,password,first_name\r\njo@example.com,Jo-Pass-1,Jos\xe9\r\n")
    return people, named, ansi


def check_export(label, path):
    """The exported file: the import's columns less the password, and every
    field the listing answered. Read with the BOM allowed, which Windows
    PowerShell 5.1 writes."""
    if not check(os.path.exists(path), "%s wrote no file" % label):
        return []
    with open(path, encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
        header = reader.fieldnames
    if not check(header == EXPORT_HEADER, "%s wrote the columns %r, expected %r" % (label, header, EXPORT_HEADER)):
        return []
    expected = []
    for entry in ANSWERS[("GET", "/api/v1/domains/example.org/accounts")]:
        expected.append({
            "address": entry["address"], "first_name": entry["first_name"], "last_name": entry["last_name"],
            "active": "true" if entry["active"] else "false", "max_size_mb": str(entry["max_size_mb"]),
            "admin_level": entry["admin_level"]})
    check(rows == expected, "%s wrote %r, expected %r" % (label, rows, expected))
    return rows


def round_trip_file(tmp, label, rows):
    """The exported rows given a password column and new addresses: what an
    administrator does to copy a domain's accounts, and the proof that the
    export writes nothing the import cannot take back."""
    path = os.path.join(tmp, "round-trip-%s.csv" % label)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["password"] + EXPORT_HEADER, lineterminator="\r\n")
        writer.writeheader()
        for row in rows:
            moved = dict(row)
            moved["password"] = ROUND_TRIP_PASSWORDS.get(row["address"], "")
            moved["address"] = row["address"].replace("@", "2@")
            writer.writerow(moved)
    return path


def check_round_trip(label, posts):
    """Each body the round trip posted carries the listing's entry back: every
    field it answered, of the type it answered, and an empty field not sent."""
    listing = ANSWERS[("GET", "/api/v1/domains/example.org/accounts")]
    check(len(posts) == len(listing), "%s round trip posted %d accounts, expected %d" % (label, len(posts), len(listing)))
    for entry, post in zip(listing, posts):
        expected = {"address": entry["address"].replace("@", "2@"), "password": ROUND_TRIP_PASSWORDS[entry["address"]]}
        for key, value in entry.items():
            if key != "address" and value != "":
                expected[key] = value
        check(post["path"] == "/api/v1/domains/example.org/accounts" and post["body"] == expected,
              "%s round trip sent %r to %s, expected %r" % (label, post["body"], post["path"], expected))


def check_import_posts(label, posts):
    sent = [(r["path"], r["body"]) for r in posts]
    check(sent == IMPORT_BODIES,
          "%s posted:\n    %r\n  expected:\n    %r" % (label, sent, IMPORT_BODIES))


def run_import_cases(url, tmp):
    """The CSV import and export, which are the verbs with logic of their own."""
    people, named, ansi = write_import_files(tmp)

    def run(arguments):
        with Recorder.lock:
            Recorder.requests = []
        completed = subprocess.run([sys.executable, HMCTL, "--url", url, "--password", PASSWORD] + arguments,
                                   capture_output=True, text=True, encoding="utf-8", timeout=60)
        with Recorder.lock:
            return completed, list(Recorder.requests)

    completed, requests = run(["accounts", "import", people, "--dry-run"])
    check(completed.returncode == 1, "accounts import --dry-run of a file with refused rows should exit 1, exited %d - %s"
          % (completed.returncode, completed.stderr))
    check(all(r["method"] == "GET" for r in requests),
          "accounts import --dry-run made a request that was not a GET: %s" % [r["method"] for r in requests])
    check("5 would be made, 1 already there, 7 refused." in completed.stdout,
          "accounts import --dry-run should count five to make, one there and seven refused:\n%s" % completed.stdout)
    check("ann@example.com (exists; left alone)" in completed.stdout,
          "accounts import --dry-run should leave the existing account alone and say so:\n%s" % completed.stdout)
    for address, reason in sorted(IMPORT_REFUSED.items()):
        check(any(address in line and reason in line for line in completed.stdout.splitlines()),
              "accounts import --dry-run should refuse %s with %r:\n%s" % (address, reason, completed.stdout))

    completed, requests = run(["accounts", "import", people])
    check(completed.returncode == 1, "accounts import with refused rows should exit 1, exited %d" % completed.returncode)
    check_import_posts("hmctl accounts import", [r for r in requests if r["method"] == "POST"])
    check("4 made, 1 already there, 8 refused." in completed.stdout,
          "accounts import should count four made, one there and eight refused:\n%s" % completed.stdout)
    check(any(SERVER_REFUSED[0] in line and SERVER_REFUSED[1] in line for line in completed.stdout.splitlines()),
          "accounts import should report the server's refusal of %s in its words:\n%s" % (SERVER_REFUSED[0], completed.stdout))

    completed, requests = run(["accounts", "import", named])
    check(completed.returncode != 0 and "'name'" in completed.stderr and "first_name and last_name" in completed.stderr,
          "accounts import of a file with a name column must refuse it and say where a name goes, said: %s%s"
          % (completed.stdout, completed.stderr))
    check(not requests, "accounts import of a file with a name column must send nothing, sent %s"
          % [(r["method"], r["path"]) for r in requests])

    completed, requests = run(["accounts", "import", ansi])
    check(completed.returncode != 0 and "not UTF-8" in completed.stderr,
          "accounts import of a file that is not UTF-8 must say so, said: %s%s" % (completed.stdout, completed.stderr))
    check(not requests, "accounts import of a file that is not UTF-8 must send nothing")

    out = os.path.join(tmp, "out.csv")
    completed, requests = run(["accounts", "export", "example.org", out])
    check(completed.returncode == 0, "accounts export: exit %d - %s" % (completed.returncode, completed.stderr))
    rows = check_export("hmctl accounts export", out)
    if os.path.exists(out):
        with open(out, encoding="utf-8") as handle:
            check("password" not in handle.readline(),
                  "accounts export must never write a password column - the server does not have them to give")

    if rows:
        completed, requests = run(["accounts", "import", round_trip_file(tmp, "hmctl", rows)])
        check(completed.returncode == 0, "the exported file, with passwords added, should import: exit %d - %s%s"
              % (completed.returncode, completed.stdout, completed.stderr))
        check_round_trip("hmctl", [r for r in requests if r["method"] == "POST"])


def run_powershell_import_cases(url, tmp):
    """Import-HmAccount and Export-HmAccount on the same files, held to the same
    bodies as hmctl: one spreadsheet, one set of requests, whichever client."""
    pwsh = shutil.which("pwsh") or shutil.which("powershell")
    if not pwsh or not os.path.exists(MODULE):
        return

    people, named, ansi = write_import_files(tmp)
    prologue = ("$ErrorActionPreference='Stop'; Import-Module '%s' -Force; "
                "Connect-HmServer -Url '%s' -Password (ConvertTo-SecureString '%s' -AsPlainText -Force) | Out-Null; "
                % (MODULE.replace("'", "''"), url, PASSWORD))

    def run(script):
        with Recorder.lock:
            Recorder.requests = []
        # The script runs inside try/catch and the catch writes the exception's own
        # message to stderr itself. PowerShell's error renderer is not merely
        # wrapping a long message at the host width on the Linux runner - it was
        # clipping each line, so words in the middle of the sentence never
        # reached stderr at all and no folding could bring them back.
        wrapped = "try { " + script + " } catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }"
        completed = subprocess.run([pwsh, "-NoProfile", "-NonInteractive", "-Command", prologue + wrapped],
                                   capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=180)
        # Folded for the reason given on the other runner: PowerShell breaks an
        # error record at the host width, which is not the same in CI as it is
        # on a bench, and these checks are about the sentence and not the break.
        # stdout is left alone - other checks read its lines.
        completed.stderr = " ".join(re.sub(r"\[[0-9;]*m", "", completed.stderr or "").split())
        with Recorder.lock:
            return completed, list(Recorder.requests)

    def quoted(path):
        return "'" + path.replace("'", "''") + "'"

    def outcome(completed):
        for line in completed.stdout.splitlines():
            if line.startswith("{") and '"Made"' in line:
                try:
                    return json.loads(line)
                except ValueError:
                    return None
        return None

    def as_list(value):
        if value is None:
            return []
        return value if isinstance(value, list) else [value]

    completed, requests = run("Import-HmAccount -Path %s -WhatIf | ConvertTo-Json -Compress -Depth 5" % quoted(people))
    result = outcome(completed)
    check(result is not None, "Import-HmAccount -WhatIf printed no outcome: %s %s"
          % (completed.stdout[:400], completed.stderr[:400]))
    check(all(r["method"] == "GET" for r in requests),
          "Import-HmAccount -WhatIf made a request that was not a GET: %s" % [r["method"] for r in requests])
    if result is not None:
        check(sorted(as_list(result.get("Made"))) == sorted(body["address"] for _, body in IMPORT_BODIES),
              "Import-HmAccount -WhatIf would make %r" % result.get("Made"))
        check(as_list(result.get("Skipped")) == ["ann@example.com"],
              "Import-HmAccount -WhatIf should leave ann@example.com alone, skipped %r" % result.get("Skipped"))
        failed = dict((f.get("Address"), f.get("Error")) for f in as_list(result.get("Failed")))
        for address, reason in sorted(IMPORT_REFUSED.items()):
            check(reason in (failed.get(address) or ""),
                  "Import-HmAccount -WhatIf should refuse %s with %r, said %r" % (address, reason, failed.get(address)))

    completed, requests = run("Import-HmAccount -Path %s -Confirm:$false | ConvertTo-Json -Compress -Depth 5" % quoted(people))
    check(completed.returncode == 0, "Import-HmAccount: exit %d - %s" % (completed.returncode, completed.stderr.strip()[:400]))
    check_import_posts("Import-HmAccount", [r for r in requests if r["method"] == "POST"])
    result = outcome(completed)
    failed = dict((f.get("Address"), f.get("Error")) for f in as_list((result or {}).get("Failed")))
    check(SERVER_REFUSED[1] in (failed.get(SERVER_REFUSED[0]) or ""),
          "Import-HmAccount should report the server's refusal of %s in its words, said %r"
          % (SERVER_REFUSED[0], failed.get(SERVER_REFUSED[0])))

    completed, requests = run("Import-HmAccount -Path %s -Confirm:$false | Out-Null" % quoted(named))
    check(completed.returncode != 0 and "'name'" in completed.stderr and "first_name and last_name" in completed.stderr,
          "Import-HmAccount of a file with a name column must refuse it and say where a name goes, said: %s"
          % completed.stderr.strip()[:400])
    check(not requests, "Import-HmAccount of a file with a name column must send nothing, sent %s"
          % [(r["method"], r["path"]) for r in requests])

    completed, requests = run("Import-HmAccount -Path %s -Confirm:$false | Out-Null" % quoted(ansi))
    check(completed.returncode != 0 and "not UTF-8" in completed.stderr,
          "Import-HmAccount of a file that is not UTF-8 must say so, said: %s" % completed.stderr.strip()[:400])
    check(not requests, "Import-HmAccount of a file that is not UTF-8 must send nothing")

    out = os.path.join(tmp, "ps-out.csv")
    completed, requests = run("Export-HmAccount -Domain example.org -Path %s" % quoted(out))
    check(completed.returncode == 0, "Export-HmAccount: exit %d - %s" % (completed.returncode, completed.stderr.strip()[:400]))
    rows = check_export("Export-HmAccount", out)

    # On the pipeline active stays a boolean: the word "false" is a string
    # PowerShell counts as true, and Where-Object active would keep every row.
    completed, requests = run("Export-HmAccount -Domain example.org | Where-Object active | ForEach-Object { 'ACTIVE ' + $_.address }")
    active = [line.strip() for line in completed.stdout.splitlines() if line.startswith("ACTIVE ")]
    check(active == ["ACTIVE yan@example.org"],
          "Export-HmAccount to the pipeline must keep active a boolean, Where-Object active kept %r" % active)

    if rows:
        completed, requests = run("Import-HmAccount -Path %s -Confirm:$false | Out-Null"
                                  % quoted(round_trip_file(tmp, "powershell", rows)))
        check(completed.returncode == 0, "Import-HmAccount of the exported file: exit %d - %s"
              % (completed.returncode, completed.stderr.strip()[:400]))
        check_round_trip("Import-HmAccount", [r for r in requests if r["method"] == "POST"])


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


def run_device_cases(url):
    """A revoke's two options travel in the query string, which the path cases
    strip: --credential is the one press that answers a stolen phone, and a
    client that sends the path without it has revoked a session and left the
    phone able to sign in again. So these assert what came after the "?", for
    both clients."""
    def run(command):
        with Recorder.lock:
            Recorder.requests = []
        completed = subprocess.run(command, capture_output=True, text=True, timeout=120)
        with Recorder.lock:
            sent = list(Recorder.requests)
        return completed, (sent[-1]["path"] if sent else "")

    completed, path = run([sys.executable, HMCTL, "--url", url, "--password", PASSWORD,
                           "device", "ann@example.com", "revoke", "0123456789abcdef0123456789abcdef", "--credential", "--forget"])
    check("credential=revoke" in path, "hmctl device revoke --credential must ask for the app password too, sent %s" % path)
    check("forget=true" in path, "hmctl device revoke --forget must ask for the entry to go, sent %s" % path)
    check("turned off" in completed.stdout, "hmctl device revoke must say the app password was turned off:\n%s" % completed.stdout)
    check(completed.returncode == 0, "hmctl device revoke: exit %d - %s" % (completed.returncode, completed.stderr))

    completed, path = run([sys.executable, HMCTL, "--url", url, "--password", PASSWORD,
                           "device", "ann@example.com", "revoke", "0123456789abcdef0123456789abcdef"])
    check("?" not in path, "hmctl device revoke without options must send a bare path, sent %s" % path)

    pwsh = shutil.which("pwsh") or shutil.which("powershell")
    if not pwsh or not os.path.exists(MODULE):
        return
    script = ("$ErrorActionPreference='Stop'; Import-Module '%s' -Force; "
              "Connect-HmServer -Url '%s' -Password (ConvertTo-SecureString '%s' -AsPlainText -Force) | Out-Null; "
              "Revoke-HmDevice -Address ann@example.com -DeviceId 0123456789abcdef0123456789abcdef -Credential -Forget -Confirm:$false | Out-Null"
              % (MODULE.replace("'", "''"), url, PASSWORD))
    completed, path = run([pwsh, "-NoProfile", "-NonInteractive", "-Command", script])
    check("credential=revoke" in path and "forget=true" in path,
          "Revoke-HmDevice -Credential -Forget must send both options, sent %s (%s)" % (path, completed.stderr.strip()[:200]))


ROUTE_SOURCE = os.path.join(ROOT, "hmailserver", "source", "Server", "Common", "Util", "RestApiSettings.cpp")


def check_ini_route_shape():
    """The recorded server's answer for GET /api/v1/settings/ini must be the
    shape the real route writes. A recorded server is only as honest as its
    recording: for a fortnight this one answered a settings array the route
    did not send, both clients read that array, and the check passed while
    every real export was empty. So the route's own source is read here, and
    every key the fake answers must appear in the function that answers it."""
    if not os.path.exists(ROUTE_SOURCE):
        return          # a partial copy; the clients are still checked against the fake
    with open(ROUTE_SOURCE, encoding="utf-8", errors="replace") as handle:
        source = handle.read()
    start = source.find("HttpResponse IniList(")
    end = source.find("HttpResponse IniGet(", start)
    if not check(start >= 0 and end > start, "RestApiSettings.cpp no longer has IniList before IniGet; "
                                               "the ini fake in this file can no longer be held to the route"):
        return
    body = source[start:end]
    answer = ANSWERS[("GET", "/api/v1/settings/ini")]
    for key in answer:
        check('\\"%s\\":[' % key in body,
              "the fake's GET /api/v1/settings/ini answers %r, which IniList in RestApiSettings.cpp does not write" % key)
    for key in answer["settings"][0]:
        check('\\"%s\\":' % key in body,
              "the fake's ini rows carry %r, which IniList in RestApiSettings.cpp does not write" % key)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--python", action="store_true", help="hmctl only")
    parser.add_argument("--powershell", action="store_true", help="the PowerShell module only")
    parser.add_argument("--verbose", action="store_true")
    arguments = parser.parse_args()

    # A problem quotes what a client sent, and the spreadsheet holds characters
    # outside the Basic Multilingual Plane that a Windows console's code page
    # cannot print. Escaped rather than raised, so a failing run lists its
    # problems instead of ending in a traceback over the first of them.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")

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
        check_ini_route_shape()
        if not arguments.powershell:
            print("hmctl against a recorded server")
            run_python_cases(url, arguments.verbose)
            run_python_refusals(url)
            run_import_cases(url, tmp)
            run_config_cases(url, tmp)
            run_report_cases(url, tmp)
            run_device_cases(url)
        if not arguments.python:
            print("the PowerShell module against the same server")
            powershell_ran = run_powershell_cases(url, arguments.verbose)
            if powershell_ran:
                run_powershell_config_cases(url, tmp, arguments.verbose)
                run_powershell_import_cases(url, tmp)
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
