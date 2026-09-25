# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Dovecot's imaptest as a benchmark: the same program, the same arguments,
pointed at any IMAP server, and its per-second counters read into the same
JSON and Markdown as the other modes.

imaptest is Dovecot's own IMAP stress and compliance tool
(codeberg.org/dovecot/imaptest). The lab runs it from the image lab.sh pins
(IMAPTEST_IMAGE in build/lab/lab.sh, read from there so there is one pin);
where the program is on the PATH it is run directly. Its passwords go in a
user file (user:password lines, mode 600, in a directory of mode 700), never
on its command line.

Profiles:
  benchmark   no_tracking: imaptest keeps no model of the mailbox and checks
              nothing, so its own CPU is not what limits the rate
  checking    imaptest's default, which tracks and checks every reply; slower,
              and it reports protocol errors as it goes
"""

import os
import re
import shutil
import subprocess
import tempfile

PROFILES = {
    "benchmark": ["no_tracking"],
    "checking": [],
}

HEADER_WORD = re.compile(r"^[A-Z][A-Za-z0-9]{2,4}$")
ACTIVE = re.compile(r"\s+(\d+)\s*/\s*(\d+)\s*$")


def pinned_image(tree):
    """The imaptest image lab.sh pins, so that this file does not pin another."""
    path = os.path.join(tree, "build", "lab", "lab.sh")
    try:
        with open(path, "rb") as handle:
            for line in handle.read().decode("utf-8", "replace").splitlines():
                if line.startswith("IMAPTEST_IMAGE="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return None


def parse_output(text):
    """imaptest prints a header of four-letter command names (Logi List Stat
    Sele Fetc ...), a line or two of percentages, and then one line of counts
    a second, ending in active/total clients; at the end, 'Totals:' and the
    same again for the whole run. Returns the columns, the per-second rows,
    the totals (imaptest's own when it printed them, else the rows' sum), the
    seconds, and the error lines."""
    header = None
    rows = []
    totals = None
    in_totals = False
    errors = []
    active = []
    for raw in text.splitlines():
        line = raw.rstrip()
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("Error:") or stripped.startswith("Panic:") or stripped.startswith("Fatal:"):
            errors.append(stripped)
            continue
        if stripped.startswith("Totals:"):
            in_totals = True
            continue
        tokens = stripped.split()
        if len(tokens) >= 3 and all(HEADER_WORD.match(t) for t in tokens):
            if header is None or in_totals:
                header = tokens
            continue
        if header is None:
            continue
        match = ACTIVE.search(line)
        body = line[:match.start()] if match else line
        numbers = body.split()
        if len(numbers) == len(header) and all(n.isdigit() for n in numbers):
            values = [int(n) for n in numbers]
            if in_totals:
                totals = values
            else:
                rows.append(values)
                if match:
                    active.append(int(match.group(1)))
    result = {"columns": header or [], "seconds": len(rows), "errors": errors[:50], "error_count": len(errors)}
    if header:
        summed = [sum(r[i] for r in rows) for i in range(len(header))]
        chosen = totals if totals is not None else summed
        result["totals"] = dict(zip(header, chosen))
        result["totals_source"] = "imaptest" if totals is not None else "sum of the per-second lines"
        if rows:
            result["per_second"] = {name: round(summed[i] / float(len(rows)), 1) for i, name in enumerate(header)}
            # The first and last seconds are partial; the steady state is the rest.
            steady = rows[1:-1] if len(rows) > 3 else rows
            result["per_second_steady"] = {name: round(sum(r[i] for r in steady) / float(len(steady)), 1)
                                           for i, name in enumerate(header)}
    if active:
        result["clients_active_min"] = min(active)
        result["clients_active_max"] = max(active)
    return result


def command(args, users_dir, tree):
    """The command line, and where it runs: the program on the PATH, or
    docker with the pinned image."""
    profile = PROFILES[args.profile]
    extra = [a for a in args.imaptest_arg]
    mbox_dir, mbox_name = os.path.split(os.path.abspath(args.mbox)) if args.mbox else (None, None)
    # Plain IMAP only: the lab's runs are TLS-off on both sides, and TLS in
    # imaptest is left to --imaptest-arg for whoever needs it.
    settings = ["host=%s" % args.host, "port=%d" % args.port, "clients=%d" % args.clients, "secs=%d" % args.seconds]
    binary = args.imaptest_binary or shutil.which("imaptest")
    if binary and not args.docker:
        settings += ["userfile=%s" % os.path.join(users_dir, "users")]
        if mbox_name:
            settings.append("mbox=%s" % os.path.join(mbox_dir, mbox_name))
        return [binary] + settings + profile + extra, "binary"
    image = args.image or pinned_image(tree)
    if not image:
        raise SystemExit("no imaptest on the PATH and no image: --image, or run from a tree with build/lab/lab.sh")
    docker = ["docker", "run", "--rm"]
    if args.network:
        docker += ["--network", args.network]
    docker += ["-v", "%s:/u:ro" % users_dir]
    settings += ["userfile=/u/users"]
    if mbox_name:
        docker += ["-v", "%s:/m:ro" % mbox_dir]
        settings.append("mbox=/m/%s" % mbox_name)
    return docker + [image] + settings + profile + extra, "docker"


def write_users(users, password):
    """The user file in a directory only this user can read (mkdtemp makes it
    700), the file 600: imaptest run directly is this user, and run by docker
    it is root in the container, which reads it all the same."""
    directory = tempfile.mkdtemp(prefix="loadgen-imaptest-")
    path = os.path.join(directory, "users")
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w") as handle:
        for user in users:
            handle.write("%s:%s\n" % (user, password))
    return directory


def run(cmd, timeout):
    completed = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    return completed.returncode, completed.stdout.decode("utf-8", "replace")


SAMPLE_OUTPUT = """\
Logi List Stat Sele Fetc Fet2 Stor Dele Expu Appe Logo
100%  50%  50% 100% 100% 100%  50% 100% 100% 100% 100%
                          30%                  5%
  10    5    4   10    9    9    4    8    8   10   20   10/ 10
  12    6    6   12   11   11    6    9    9   12   24   10/ 10
  11    5    5   11   10   10    5    8    8   11   22   10/ 10
  13    7    6   13   12   12    6   10   10   13   26   10/ 10
Error: user3@example.test[12]: FETCH BODY[] reply was NO
Totals:
Logi List Stat Sele Fetc Fet2 Stor Dele Expu Appe Logo
  46   23   21   46   42   42   21   35   35   46   92
"""
