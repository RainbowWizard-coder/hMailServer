#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The active-active Windows lab: two Windows nodes of hMailServer on one SQL Server.

Run on the machine that hosts the two lab VMs (the owner's laptop), with OpenSSH's
ssh and scp on the PATH and the lab's key. It drives three machines over SSH and
copies to each what runs there: windows/node.ps1 to the nodes, windows/mssql.sh (with
../sink.py, ../sqlcheck.py and windows/sqlcheck-mssql.py) to server2. The lab's
README (build/lab/active-active/README.md, "The Windows lab") says what the lab is,
what it has measured and what it cannot show.

  W1  the installer smoke VM: the node's server is the machine's only hMailServer,
      registered as a real installation is (service hMailServer, the COM server)
  W2  the AD lab's domain controller: it has an hMailServer of its own, which the lab
      never touches, so the node is a second service beside it, hMailServerAA, not
      registered with COM
  server2  SQL Server 2022 Developer in the long-lived container hmlab-aa-mssql

  kit --server1 <commit>      build the Windows server of <commit> on server1
                              (make-kit.ps1; the commit must be in server1's
                              C:\\bench\\hmailserver) and fetch its kit here
  provision --kit <zip> --msi <msoledbsql.msi>
                              everything, idempotently: SQL Server and its logins,
                              each node's server, the OLE DB driver, the firewall,
                              the lab database made by the server itself through
                              COM on W1 (CreateExternalDatabase), its domain,
                              accounts, route and listeners, and W2 pointed at it
  deploy-build --kit <zip> [--node W1|W2|all] [--upgrade-database] [--start]
                              a build's binaries onto the nodes, each INI kept; the
                              nodes that were running are started again (--start:
                              all of them); --upgrade-database stops every node
                              first and moves the schema from W1
  start <W1|W2|all> [--both]  start a node's service. One node at a time: the other
                              must be stopped and hold no session on the database,
                              because until the active-active units land
                              WarmStandby.md holds. --both is for a lab measurement
  stop <W1|W2|all>
  status                      the nodes, the container, and who holds sessions
  reset [--recreate]          every node stopped and its data folder emptied, the lab
                              database restored from its golden backup; --recreate
                              drops it and has the server make it again (provision's
                              database step)
  smoke <W1|W2>               that node alone: started, a message delivered over
                              SMTP to account2@example.test and read back over
                              IMAP, one relayed to the sink, then stopped
  baseline                    scenario S0 (BUILD-PLAN.md section 1) on SQL Server:
                              both nodes at once on a reset database, 2,000 relayed
                              and 1,000 into one mailbox; then back to one node
  sqlcheck                    ../sqlcheck.py's engine checks at READ UNCOMMITTED,
                              with writers that roll back (windows/sqlcheck-mssql.py)
  ado-probe <W1|W2>           RecordsAffected through ADO on that node

Environment (all optional): AA_WIN_SECRETS (default ~/.config/hmailserver/aa-lab-mssql),
AA_WIN_KEY (~/.ssh/hmail_ad), AA_WIN_STATE (~/.cache/hmlab-aa-win: kits and runs),
AA_WIN_W1 and AA_WIN_W2 (the nodes' addresses; W1's is asked of vmrun when unset,
since DHCP gives it), AA_WIN_SERVER2 (chris@10.0.0.19), AA_WIN_SERVER1
(chris@10.0.0.20), AA_WIN_DB_ADDRESS (10.0.0.19), AA_SETTLE_SECONDS (90),
AA_SINK_COUNT (2000), AA_MAILBOX_COUNT (1000), AA_LOAD_THREADS (8).

The secrets file holds KEY=VALUE lines: AA_MSSQL_SA_PASSWORD, AA_NODE_LOGIN,
AA_NODE_PASSWORD, AA_DB_NAME, AA_ADMIN_PASSWORD, AA_ACCOUNT_PASSWORD, AA_CHECK_LOGIN,
AA_CHECK_PASSWORD. `aa-win.py secrets` makes the ones missing. None is ever an
argument or printed: each reaches the machine that needs it on standard input.
"""

import argparse
import datetime
import email.utils
import imaplib
import json
import os
import re
import secrets as random_secrets
import shlex
import smtplib
import string
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ACTIVE = os.path.dirname(HERE)
HOME = os.path.expanduser("~")
SECRETS_FILE = os.environ.get("AA_WIN_SECRETS", os.path.join(HOME, ".config", "hmailserver", "aa-lab-mssql"))
KEY = os.environ.get("AA_WIN_KEY", os.path.join(HOME, ".ssh", "hmail_ad"))
STATE = os.environ.get("AA_WIN_STATE", os.path.join(HOME, ".cache", "hmlab-aa-win"))
SERVER2 = os.environ.get("AA_WIN_SERVER2", "chris@10.0.0.19")
SERVER1 = os.environ.get("AA_WIN_SERVER1", "chris@10.0.0.20")
DB_ADDRESS = os.environ.get("AA_WIN_DB_ADDRESS", "10.0.0.19")
MSSQL_PORT = int(os.environ.get("AA_WIN_MSSQL_PORT", "14330"))
SINK_PORT = int(os.environ.get("AA_WIN_SINK_PORT", "14325"))
# VMware Workstation's own place for it, and no other: a program this script runs is
# never named by the environment. Elsewhere, give the nodes' addresses instead.
VMRUN = r"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
VMS = os.path.join(HOME, "Documents", "Virtual Machines")
NODE_ROOT = r"C:\hmlab-aa"
NODE_LAB = NODE_ROOT + r"\lab"
REMOTE_LAB = "hmlab/aa-win/lab"
PORTS = {"smtp": 2525, "submission": 2587, "pop3": 1110, "imap": 1143}

NODES = {
    "W1": {"env": "AA_WIN_W1", "vmx": os.path.join(VMS, "Win server 2025 small", "Win server 2025 small.vmx"),
           "fallback": "192.168.11.183", "service": "hMailServer", "register": True},
    "W2": {"env": "AA_WIN_W2", "vmx": os.path.join(VMS, "Windows Server 2025", "Windows Server 2025.vmx"),
           "fallback": "192.168.11.149", "service": "hMailServerAA", "register": False},
}
SECRET_KEYS = ["AA_MSSQL_SA_PASSWORD", "AA_NODE_LOGIN", "AA_NODE_PASSWORD", "AA_DB_NAME", "AA_ADMIN_PASSWORD",
               "AA_ACCOUNT_PASSWORD", "AA_CHECK_LOGIN", "AA_CHECK_PASSWORD"]


class Failure(Exception):
    pass


def say(text=""):
    print(text, flush=True)


# ------------------------------------------------------------------ secrets

def strong(n=28):
    alphabet = string.ascii_letters + string.digits
    while True:
        body = "".join(random_secrets.choice(alphabet) for _ in range(n))
        if any(c.isupper() for c in body) and any(c.islower() for c in body) and any(c.isdigit() for c in body):
            return body[:10] + "-" + body[10:20] + "_" + body[20:]


def cmd_secrets(_args):
    have = load_secrets(required=False)
    makers = {"AA_MSSQL_SA_PASSWORD": strong, "AA_NODE_LOGIN": lambda: "hmlab_aa_node", "AA_NODE_PASSWORD": strong,
              "AA_DB_NAME": lambda: "hmlab_aa", "AA_ADMIN_PASSWORD": strong, "AA_ACCOUNT_PASSWORD": lambda: strong(20),
              "AA_CHECK_LOGIN": lambda: "hmlab_aa_check", "AA_CHECK_PASSWORD": strong}
    added = [k for k in SECRET_KEYS if k not in have]
    for k in added:
        have[k] = makers[k]()
    os.makedirs(os.path.dirname(SECRETS_FILE), exist_ok=True)
    new = not os.path.exists(SECRETS_FILE)
    with open(SECRETS_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The active-active Windows lab (build/lab/active-active/windows). Never print or commit.\n")
        for k in SECRET_KEYS:
            f.write("%s=%s\n" % (k, have[k]))
    if new and os.name == "nt":
        # Only the owner, as the other files in ~/.config/hmailserver.
        subprocess.run(["icacls", SECRETS_FILE, "/inheritance:r", "/grant:r",
                        "%s\\%s:(F)" % (os.environ.get("USERDOMAIN", ""), os.environ.get("USERNAME", ""))],
                       stdout=subprocess.DEVNULL, check=True)
    elif new:
        os.chmod(SECRETS_FILE, 0o600)
    say("secrets: %s; made: %s" % (SECRETS_FILE, ", ".join(added) if added else "none (all there)"))
    return 0


def load_secrets(required=True):
    values = {}
    if os.path.exists(SECRETS_FILE):
        for line in open(SECRETS_FILE, encoding="utf-8"):
            line = line.rstrip("\r\n")
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                values[k] = v
    if required:
        missing = [k for k in SECRET_KEYS if not values.get(k)]
        if missing:
            raise Failure("%s lacks %s: run aa-win.py secrets" % (SECRETS_FILE, ", ".join(missing)))
    return values


S = {}


def payload(keys, rest=b""):
    return ("".join("%s=%s\n" % (k, S[k]) for k in keys) + "%%\n").encode("utf-8") + rest


# ------------------------------------------------------------------ the machines

def run(args, stdin=b"", check=True, show=True, timeout=None, quiet=False):
    p = subprocess.run(args, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    out = p.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    err = p.stderr.decode("utf-8", "replace").replace("\r\n", "\n")
    if show and out.strip():
        print(out.rstrip("\n"), flush=True)
    if err.strip() and not quiet and (show or p.returncode != 0):
        print(err.rstrip("\n"), file=sys.stderr, flush=True)
    if check and p.returncode != 0:
        raise Failure("%s returned %d" % (" ".join(args[-2:]) if len(args) > 1 else args[0], p.returncode))
    return p.returncode, out


# Windows's own OpenSSH where it is there: it reads a local "C:\..." path as a file,
# where an MSYS scp (Git's) would take the drive letter for a host name.
_OPENSSH = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "OpenSSH")
SSH_EXE = os.path.join(_OPENSSH, "ssh.exe") if os.name == "nt" and os.path.exists(os.path.join(_OPENSSH, "ssh.exe")) else "ssh"
SCP_EXE = os.path.join(_OPENSSH, "scp.exe") if os.name == "nt" and os.path.exists(os.path.join(_OPENSSH, "scp.exe")) else "scp"
SSH = [SSH_EXE, "-i", KEY, "-o", "BatchMode=yes", "-o", "ConnectTimeout=20", "-o", "ServerAliveInterval=30"]


def ssh(target, command, stdin=b"", **kw):
    return run(SSH + [target, command], stdin=stdin, **kw)


def scp(local, remote):
    run([SCP_EXE, "-q", "-i", KEY, "-o", "BatchMode=yes", local, remote], show=False)


_addresses = {}


def address(name):
    if name in _addresses:
        return _addresses[name]
    node = NODES[name]
    found = os.environ.get(node["env"])
    if not found and os.path.exists(VMRUN) and os.path.exists(node["vmx"]):
        p = subprocess.run([VMRUN, "-T", "ws", "getGuestIPAddress", node["vmx"]], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=60)
        text = p.stdout.decode("utf-8", "replace").strip()
        if p.returncode == 0 and re.match(r"^\d+\.\d+\.\d+\.\d+$", text):
            found = text
    _addresses[name] = found or node["fallback"]
    return _addresses[name]


def node_target(name):
    return "administrator@%s" % address(name)


# What runs on the other machines is this tree's, copied there once a run before the
# first command, so that a node reverted to a snapshot never runs an older node.ps1.
_pushed = set()


def node_ps(name, command, args="", keys=(), check=True, show=True, timeout=None, quiet=False):
    if name not in _pushed:
        push_node(name, [])
    line = r'powershell -NoProfile -ExecutionPolicy Bypass -File %s\node.ps1 %s %s' % (NODE_LAB, command, args)
    return ssh(node_target(name), line.strip(), stdin=payload(keys) if keys else b"", check=check, show=show,
               timeout=timeout, quiet=quiet)


def push_node(name, files):
    ssh(node_target(name), r"if not exist %s mkdir %s" % (NODE_LAB, NODE_LAB), show=False)
    if name not in _pushed:
        files = [(os.path.join(HERE, "node.ps1"), "node.ps1")] + [f for f in files if f[1] != "node.ps1"]
        _pushed.add(name)
    for local, remote in files:
        scp(local, "%s:%s/%s" % (node_target(name), NODE_LAB.replace("\\", "/"), remote))


def push_server2():
    ssh(SERVER2, "mkdir -p ~/%s/windows && chmod 700 ~/hmlab/aa-win" % REMOTE_LAB, show=False)
    for rel in ("sink.py", "sqlcheck.py", "windows/mssql.sh", "windows/sqlcheck-mssql.py"):
        scp(os.path.join(ACTIVE, *rel.split("/")), "%s:%s/%s" % (SERVER2, REMOTE_LAB, rel))
    _pushed.add("server2")


def mssql(command, keys=(), rest=b"", args=(), check=True, show=True, timeout=None):
    if "server2" not in _pushed:
        push_server2()
    line = "bash ~/%s/windows/mssql.sh %s" % (REMOTE_LAB, " ".join(shlex.quote(a) for a in (command,) + tuple(args)))
    return ssh(SERVER2, line, stdin=payload(keys, rest), check=check, show=show, timeout=timeout)


def sql(text):
    _, out = mssql("sql", ["AA_MSSQL_SA_PASSWORD"], rest=("set nocount on;\nuse [%s];\n%s\nGO\n" % (S["AA_DB_NAME"], text)).encode(),
                   show=False)
    return [line.split("\t") for line in out.splitlines() if line.strip()]


def db_exists():
    _, out = mssql("sql", ["AA_MSSQL_SA_PASSWORD"],
                   rest=("set nocount on;\nselect case when db_id(N'%s') is null then 0 else 1 end;\nGO\n" % S["AA_DB_NAME"]).encode(),
                   show=False)
    return out.strip().endswith("1")


def db_sessions():
    _, out = mssql("sql", ["AA_MSSQL_SA_PASSWORD"], rest=(
        "set nocount on;\nselect host_name, count(*) from sys.dm_exec_sessions where login_name = N'%s' group by host_name;\nGO\n"
        % S["AA_NODE_LOGIN"]).encode(), show=False)
    found = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) == 2 and parts[1].strip().isdigit():
            found[parts[0].strip()] = int(parts[1])
    return found


def node_state(name):
    code, out = node_ps(name, "status", show=False, check=False, quiet=True)
    if code != 0:
        return {"name": name, "installed": False, "running": False, "host": None, "line": out.strip()}
    m = re.match(r"^(\S+): service (\S+) (\S+)", out.strip())
    return {"name": name, "installed": True, "running": bool(m and m.group(3) == "Running"),
            "host": m.group(1) if m else None, "line": out.strip()}


def db_server():
    return "%s,%d" % (DB_ADDRESS, MSSQL_PORT)


def db_args():
    return "-DbServer %s -Database %s -Login %s" % (db_server(), S["AA_DB_NAME"], S["AA_NODE_LOGIN"])


# ------------------------------------------------------------------ commands

def latest_kit():
    kits = os.path.join(STATE, "kits")
    found = sorted((os.path.join(kits, f) for f in os.listdir(kits) if f.endswith(".zip")), key=os.path.getmtime) \
        if os.path.isdir(kits) else []
    if not found:
        raise Failure("no kit: give --kit, or make one with: aa-win.py kit --server1 <commit>")
    return found[-1]


def cmd_kit(args):
    os.makedirs(os.path.join(STATE, "kits"), exist_ok=True)
    ssh(SERVER1, r"if not exist C:\t1\aa-lab mkdir C:\t1\aa-lab", show=False)
    scp(os.path.join(HERE, "make-kit.ps1"), "%s:C:/t1/aa-lab/make-kit.ps1" % SERVER1)
    _, out = ssh(SERVER1, r"pwsh -NoProfile -File C:\t1\aa-lab\make-kit.ps1 -Commit %s" % args.server1, timeout=3600)
    m = re.search(r"KIT (\S+\.zip)", out)
    if not m:
        raise Failure("make-kit.ps1 named no kit")
    local = os.path.join(STATE, "kits", os.path.basename(m.group(1).replace("\\", "/")))
    scp("%s:%s" % (SERVER1, m.group(1).replace("\\", "/")), local)
    say("kit: %s" % local)
    return 0


def provision_database():
    for name in NODES:
        node_ps(name, "stop", show=False, check=False)
    node_ps("W1", "create-database", db_args(), keys=["AA_ADMIN_PASSWORD", "AA_NODE_PASSWORD"], timeout=900)
    node_ps("W1", "configure", "-SinkHost %s -SinkPort %d" % (DB_ADDRESS, SINK_PORT),
            keys=["AA_ADMIN_PASSWORD", "AA_ACCOUNT_PASSWORD"])
    node_ps("W1", "stop")
    mssql("finish", ["AA_MSSQL_SA_PASSWORD", "AA_NODE_LOGIN", "AA_DB_NAME"])


def cmd_provision(args):
    kit = args.kit or latest_kit()
    if not args.msi or not os.path.exists(args.msi):
        raise Failure("--msi: the Microsoft OLE DB Driver 18 for SQL Server (msoledbsql.msi, x64), downloaded and checked")
    say("== server2: SQL Server 2022 (hmlab-aa-mssql) and its logins")
    push_server2()
    mssql("up", ["AA_MSSQL_SA_PASSWORD"], timeout=600)
    mssql("logins", ["AA_MSSQL_SA_PASSWORD", "AA_NODE_LOGIN", "AA_NODE_PASSWORD", "AA_DB_NAME", "AA_CHECK_LOGIN",
                     "AA_CHECK_PASSWORD"])
    for name, node in NODES.items():
        say("== %s (%s): the server, the OLE DB driver, the firewall, the administrator password" % (name, address(name)))
        push_node(name, [(os.path.join(HERE, "node.ps1"), "node.ps1"), (kit, "kit.zip"), (args.msi, "msoledbsql.msi")])
        node_ps(name, "msoledbsql", r"-Msi %s\msoledbsql.msi" % NODE_LAB, timeout=900)
        if args.reinstall or not node_state(name)["installed"]:
            node_ps(name, "install", r"-Kit %s\kit.zip -Service %s%s" % (NODE_LAB, node["service"], " -Register" if node["register"] else ""),
                    timeout=900)
        node_ps(name, "firewall")
        node_ps(name, "set-admin-password", keys=["AA_ADMIN_PASSWORD"])
    say("== the lab database")
    if not db_exists():
        provision_database()
    else:
        say("%s exists: kept (aa-win.py reset --recreate makes it again)" % S["AA_DB_NAME"])
        code, _ = node_ps("W1", "check-database", keys=["AA_NODE_PASSWORD"], check=False, show=False)
        if code != 0:
            node_ps("W1", "set-database", db_args(), keys=["AA_NODE_PASSWORD"])
    node_ps("W2", "set-database", db_args(), keys=["AA_NODE_PASSWORD"])
    for name in NODES:
        node_ps(name, "check-database", keys=["AA_NODE_PASSWORD"])
    return cmd_status(args)


def cmd_deploy_build(args):
    kit = args.kit or latest_kit()
    targets = list(NODES) if args.node == "all" else [args.node]
    was_running = [n for n in NODES if node_state(n)["running"]]
    if args.upgrade_database:
        for n in NODES:
            node_ps(n, "stop", show=False)
    for name in targets:
        push_node(name, [(os.path.join(HERE, "node.ps1"), "node.ps1"), (kit, "kit.zip")])
        node_ps(name, "deploy", r"-Kit %s\kit.zip" % NODE_LAB, timeout=900)
    if args.upgrade_database:
        node_ps("W1", "upgrade-database", timeout=1800)
    restart = list(NODES) if args.start else was_running
    restart = [n for n in NODES if n in restart and (n in targets or args.upgrade_database)]
    if len(restart) > 1:
        say("both nodes were running (a lab measurement): starting both again")
    for name in restart:
        node_ps(name, "start")
    return 0


def start_node(name, both=False):
    other = [n for n in NODES if n != name]
    if not both:
        for o in other:
            st = node_state(o)
            if st["running"]:
                raise Failure("%s is running: one node at a time against the database (WarmStandby.md) - stop it, "
                              "or give --both for a lab measurement" % o)
        sessions = db_sessions()
        mine = node_state(name)["host"]
        foreign = {h: c for h, c in sessions.items() if h and h != mine}
        if foreign:
            raise Failure("sessions of %s are open on the database from %s: another server runs against it"
                          % (S["AA_NODE_LOGIN"], ", ".join("%s (%d)" % kv for kv in foreign.items())))
    node_ps(name, "start", timeout=300)


def cmd_start(args):
    targets = list(NODES) if args.node == "all" else [args.node]
    if len(targets) > 1 and not args.both:
        raise Failure("starting both nodes is a lab measurement: give --both")
    for name in targets:
        start_node(name, both=args.both)
    return 0


def cmd_stop(args):
    for name in (list(NODES) if args.node == "all" else [args.node]):
        node_ps(name, "stop", timeout=300)
    return 0


def cmd_status(_args):
    for name in NODES:
        st = node_state(name)
        say("%s %s: %s" % (name, address(name), st["line"] or "not installed"))
    mssql("status", ["AA_MSSQL_SA_PASSWORD", "AA_NODE_LOGIN", "AA_DB_NAME"])
    return 0


def cmd_reset(args):
    for name in NODES:
        node_ps(name, "clear-data", timeout=300)
    if args.recreate:
        mssql("reset", ["AA_MSSQL_SA_PASSWORD", "AA_NODE_LOGIN", "AA_DB_NAME"], args=("--recreate",))
        provision_database()
        for name in NODES:
            node_ps(name, "clear-data", timeout=300)
    else:
        mssql("reset", ["AA_MSSQL_SA_PASSWORD", "AA_NODE_LOGIN", "AA_DB_NAME"])
    return 0


def run_id(kind):
    return "%s-%s" % (kind, datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ"))


def message(seq, to):
    return ("From: <load@load.test>\r\nTo: <%s>\r\nSubject: active-active lab %s\r\nDate: %s\r\n"
            "Message-ID: <%s@load.test>\r\nX-AA-Seq: %s\r\n\r\nmessage %s\r\n"
            % (to, seq, email.utils.formatdate(), seq, seq, seq)).encode()


def imap_find(host, user, seq, deadline):
    while time.time() < deadline:
        try:
            imap = imaplib.IMAP4(host, PORTS["imap"], timeout=30)
            imap.login(user, S["AA_ACCOUNT_PASSWORD"])
            imap.select("INBOX", readonly=True)
            typ, data = imap.uid("SEARCH", None, "HEADER", "X-AA-Seq", seq)
            uids = data[0].split() if typ == "OK" and data and data[0] else []
            if uids:
                typ, data = imap.uid("FETCH", uids[-1], "(BODY.PEEK[HEADER.FIELDS (X-AA-SEQ)])")
                header = data[0][1] if typ == "OK" and data and isinstance(data[0], tuple) else b""
                imap.logout()
                return seq.encode() in header
            imap.logout()
        except (imaplib.IMAP4.error, OSError):
            pass
        time.sleep(2)
    return False


def sink_has(run, seq, deadline):
    while time.time() < deadline:
        _, out = mssql("sink-log", args=(run,), show=False)
        if any(json.loads(line).get("seq") == seq for line in out.splitlines() if line.strip()):
            return True
        time.sleep(3)
    return False


def cmd_smoke(args):
    name = args.node
    host = address(name)
    run = run_id("smoke-" + name.lower())
    for o in NODES:
        if o != name:
            node_ps(o, "stop", show=False)
    mssql("sink-start", args=(run,))
    try:
        start_node(name)
        sessions = db_sessions()
        seq_box, seq_sink = run + ".box", run + ".sink"
        smtp = smtplib.SMTP(host, PORTS["smtp"], timeout=60)
        smtp.ehlo("load.test")
        refused_box = smtp.sendmail("load@load.test", ["account2@example.test"], message(seq_box, "account2@example.test"))
        refused_sink = smtp.sendmail("load@load.test", ["r1@sink.test"], message(seq_sink, "r1@sink.test"))
        smtp.quit()
        deadline = time.time() + 120
        read_back = imap_find(host, "account2@example.test", seq_box, deadline)
        relayed = sink_has(run, seq_sink, deadline)
        result = {"smoke": name, "node": host, "sessions_on_the_database": sessions,
                  "smtp_accepted": not refused_box and not refused_sink,
                  "imap_read_back": read_back, "relayed_to_the_sink": relayed}
        say(json.dumps(result))
        node_ps(name, "errors")
        return 0 if result["smtp_accepted"] and read_back and relayed else 1
    finally:
        node_ps(name, "stop", show=False, check=False)
        mssql("sink-stop", show=False, check=False)


QUEUE_SQL = """
select 'queued (messagetype 1)', count(*) from hm_messages where messagetype = 1;
select 'queued and locked', count(*) from hm_messages where messagetype = 1 and messagelocked = 1;
select 'in mailboxes (messagetype 2)', count(*) from hm_messages where messagetype = 2;
select 'folders with a UID given twice', count(*) from (select messagefolderid, messageuid from hm_messages where messagetype = 2 group by messagefolderid, messageuid having count(*) > 1) d;
select 'rows sharing a UID with another', coalesce(sum(n), 0) from (select count(*) as n from hm_messages where messagetype = 2 group by messagefolderid, messageuid having count(*) > 1) d;
"""


def queue_counts():
    return {row[0]: int(row[1]) for row in sql(QUEUE_SQL) if len(row) == 2 and row[1].strip().lstrip("-").isdigit()}


def load_tool(arguments, out_file=None, env_extra=None):
    env = dict(os.environ)
    env.update(env_extra or {})
    p = subprocess.run([sys.executable, os.path.join(ACTIVE, "aa-load.py")] + arguments, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, env=env)
    out = p.stdout.decode("utf-8", "replace")
    if p.stderr.strip():
        print(p.stderr.decode("utf-8", "replace"), file=sys.stderr)
    print(out.rstrip("\n"), flush=True)
    if out_file:
        with open(out_file, "a", encoding="utf-8") as f:
            f.write(out)
    return [json.loads(line) for line in out.splitlines() if line.startswith("{")]


def cmd_baseline(args):
    run = run_id("baseline")
    out = os.path.join(STATE, "runs", run)
    os.makedirs(out, exist_ok=True)
    settle = int(os.environ.get("AA_SETTLE_SECONDS", "90"))
    threads = os.environ.get("AA_LOAD_THREADS", "8")
    hosts = ",".join("%s:%d" % (address(n), PORTS["smtp"]) for n in NODES)
    imap_hosts = ",".join("%s:%d" % (address(n), PORTS["imap"]) for n in NODES)
    summary = {"run": run, "nodes": {n: address(n) for n in NODES}, "settle_seconds": settle}
    say("== baseline %s: two nodes on one SQL Server database, a data folder each" % run)
    for name in NODES:
        node_ps(name, "stop", show=False)
    mssql("reset", ["AA_MSSQL_SA_PASSWORD", "AA_NODE_LOGIN", "AA_DB_NAME"])
    for name in NODES:
        node_ps(name, "clear-data", show=False)
    mssql("sink-start", args=(run,))
    try:
        for name in NODES:
            start_node(name, both=True)
        summary["sessions_with_both_up"] = db_sessions()
        summary["builds"] = {n: node_state(n)["line"] for n in NODES}
        say("sessions on the database: %s" % summary["sessions_with_both_up"])
        sent_sink = os.path.join(out, "sent-s0sink.jsonl")
        summary["send_sink"] = load_tool(["send", "--hosts", hosts, "--count", os.environ.get("AA_SINK_COUNT", "2000"),
                                          "--to", "r1@sink.test", "--threads", threads, "--tag", "s0sink", "--out", sent_sink])
        time.sleep(settle)
        say("== relayed to the sink")
        _, sink_log = mssql("sink-log", args=(run,), show=False)
        sink_file = os.path.join(out, "sink.jsonl")
        with open(sink_file, "w", encoding="utf-8") as f:
            f.write(sink_log)
        summary["check_sink"] = load_tool(["check-sink", "--sent", sent_sink, "--sink", sink_file, "--show"])
        summary["queue_after_sink"] = queue_counts()
        say(json.dumps(summary["queue_after_sink"]))
        sent_box = os.path.join(out, "sent-s0box.jsonl")
        summary["send_box"] = load_tool(["send", "--hosts", hosts, "--count", os.environ.get("AA_MAILBOX_COUNT", "1000"),
                                         "--to", "account1@example.test", "--threads", threads, "--tag", "s0box", "--out", sent_box])
        time.sleep(settle)
        say("== into one mailbox")
        summary["queue_after_box"] = queue_counts()
        say(json.dumps(summary["queue_after_box"]))
        summary["imap"] = load_tool(["imap", "--hosts", imap_hosts, "--user", "account1@example.test"],
                                    env_extra={"AA_ACCOUNT_PASSWORD": S["AA_ACCOUNT_PASSWORD"]})
        say("== error codes")
        summary["errors"] = {}
        for name in NODES:
            _, text = node_ps(name, "errors", show=False)
            say("-- %s\n%s" % (name, text.rstrip()))
            summary["errors"][name] = {m.group(2): int(m.group(1)) for m in re.finditer(r"(\d+) (HM\d{4})", text)}
        say("== back to one node: W2 stopped, W1 alone")
        node_ps("W2", "stop")
        time.sleep(min(settle, 60))
        summary["queue_with_w1_alone"] = queue_counts()
        summary["sessions_with_w1_alone"] = db_sessions()
        say(json.dumps({"queue_with_w1_alone": summary["queue_with_w1_alone"], "sessions": summary["sessions_with_w1_alone"]}))
    finally:
        # Whatever stopped the run, it does not leave two nodes on the database, nor
        # W2's turn open: W2 is stopped, which gives the AD lab its own server back.
        node_ps("W2", "stop", show=False, check=False)
        mssql("sink-stop", show=False, check=False)
        with open(os.path.join(out, "summary.json"), "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
        say("== baseline end; results %s" % out)
    return 0


def cmd_sqlcheck(_args):
    code, _ = mssql("sqlcheck", ["AA_CHECK_LOGIN", "AA_CHECK_PASSWORD"], args=("sqlcheck-mssql.py",), check=False,
                    timeout=3600)
    return code


def cmd_ado_probe(args):
    code, _ = node_ps(args.node, "ado-probe", db_args(), keys=["AA_NODE_PASSWORD"], check=False)
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("secrets")
    p = sub.add_parser("kit")
    p.add_argument("--server1", required=True, metavar="COMMIT")
    p = sub.add_parser("provision")
    p.add_argument("--kit")
    p.add_argument("--msi")
    p.add_argument("--reinstall", action="store_true")
    p = sub.add_parser("deploy-build")
    p.add_argument("--kit")
    p.add_argument("--node", default="all", choices=["W1", "W2", "all"])
    p.add_argument("--upgrade-database", action="store_true")
    p.add_argument("--start", action="store_true")
    p = sub.add_parser("start")
    p.add_argument("node", choices=["W1", "W2", "all"])
    p.add_argument("--both", action="store_true")
    p = sub.add_parser("stop")
    p.add_argument("node", choices=["W1", "W2", "all"])
    sub.add_parser("status")
    p = sub.add_parser("reset")
    p.add_argument("--recreate", action="store_true")
    p = sub.add_parser("smoke")
    p.add_argument("node", choices=["W1", "W2"])
    sub.add_parser("baseline")
    sub.add_parser("sqlcheck")
    p = sub.add_parser("ado-probe")
    p.add_argument("node", choices=["W1", "W2"])
    args = parser.parse_args()
    handlers = {"secrets": cmd_secrets, "kit": cmd_kit, "provision": cmd_provision, "deploy-build": cmd_deploy_build,
                "start": cmd_start, "stop": cmd_stop, "status": cmd_status, "reset": cmd_reset, "smoke": cmd_smoke,
                "baseline": cmd_baseline, "sqlcheck": cmd_sqlcheck, "ado-probe": cmd_ado_probe}
    try:
        if args.command != "secrets":
            S.update(load_secrets())
        return handlers[args.command](args)
    except Failure as e:
        print("aa-win.py: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
