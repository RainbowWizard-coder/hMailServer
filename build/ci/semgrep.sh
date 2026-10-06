#!/bin/bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The semgrep job of .gitlab-ci.yml: Semgrep's open rules over the C#, Python,
# JavaScript and shell sources - the half of the old CodeQL job that analysed the
# Control Panel and the tools, and more (CodeQL never read the Python, the shell
# or the web pages' scripts). The C++ is the static-analysis job's. Run in
# python:3.14-slim-trixie from the root of a checkout; by hand it is the same:
#
#   docker run --rm -v "$PWD":/src -w /src python:3.14-slim-trixie bash build/ci/semgrep.sh
#
# Where everything comes from, none of it GitHub:
#   Semgrep     PyPI, at the version and hashes build/ci/requirements.txt pins
#               (pip install --require-hashes, into a virtual environment);
#   the rules   Semgrep's registry, semgrep.dev, fetched over HTTPS with no
#               account and no token: the packs p/csharp, p/python and
#               p/javascript, and the registry's bash rules (r/bash - it has no
#               bash pack). They are fetched into the job's artifacts before the
#               scan, with their SHA-256, so the rules a run used can be read
#               afterwards; the registry's packs change without a version, so a
#               finding can be new because a rule is. The registry's rules are
#               under the Semgrep Rules License v1.0, which allows this use and
#               not redistribution, which is why they are fetched and not
#               committed;
#   the exceptions  master's .gitlab/security-exceptions.yml, from GitLab's API for
#               a file at a ref (the image has no git), with no token: the project
#               is public. A finding new to the baseline passes only when it accepts
#               it - master's copy, never the commit's own, so that a change cannot
#               accept its own finding (SECURITY.md, "Vulnerability management
#               policy").
#
# What it reads (build/ci/semgrepignore says what it leaves out, and why):
#   *.cs      the Control Panel and the tools, under hmailserver/source/Tools
#   *.py      the checkers and generators under build/, the SPF generator
#   *.js      the webmail's scripts under Common/Util, and the S/MIME and
#             OpenPGP checks under build/
#   *.sh      the CI, packaging and bench scripts
#   the scripts with a shebang and no extension - the command-line client hmctl
#             and the packages' maintainer scripts - copied under a name with one
#             and reported against their own path (below, why);
#   the inline scripts of the Control Deck (installation/WebAdmin/index.html) and
#             the webmail's pages (Portal.html, PortalProtected.html), taken out
#             of their <script> elements into files outside the checkout whose
#             line numbers are the page's own, and reported against the page.
#
# What it leaves (HM_OUT_DIR, security/semgrep): semgrep.json (Semgrep's own
# output), rules/ (the packs as fetched) and rules.sha256, gl-sast-semgrep.json (a
# GitLab SAST report), junit.xml, findings.txt, new-findings.txt,
# accepted-findings.txt and master-security-exceptions.yml (the exceptions file the
# run was judged by). The exit status is build/ci/security-findings.py's against
# build/ci/semgrep-baseline.txt and master's exceptions: 0 nothing new that they do
# not accept, 1 something new, 2 a scan with holes or an exceptions file that could
# not be read. --write-baseline rewrites the baseline from this run: it keeps the
# findings still found, drops the ones gone, and adds none.
#
# The environment it reads:
#   HM_OUT_DIR       where the results go (security/semgrep)
#   HM_SEMGREP_JOBS  semgrep's --jobs (4)
#   HM_EXCEPTIONS_FILE  an exceptions file to judge by INSTEAD of master's, for a run
#                    by hand; refused in a GitLab job
set -uo pipefail

out="${HM_OUT_DIR:-security/semgrep}"
jobs="${HM_SEMGREP_JOBS:-4}"
write_baseline=()
[ "${1:-}" = "--write-baseline" ] && write_baseline=(--write-baseline)
packs="p/csharp p/python p/javascript r/bash"

if [ ! -f build/ci/requirements.txt ]; then
  echo "Run this from the root of a checkout (no build/ci/requirements.txt here)."
  exit 2
fi
started=$(date -u +%Y-%m-%dT%H:%M:%S)
work=$(mktemp -d /tmp/hm-semgrep.XXXXXX)
created_ignore=""
cleanup() {
  [ -n "$created_ignore" ] && rm -f .semgrepignore
  rm -rf "$work"
}
trap cleanup EXIT
rm -rf "$out"
mkdir -p "$out/rules"

echo "== Semgrep, from PyPI, at the hashes build/ci/requirements.txt pins"
python3 -m venv "$work/venv"
if ! "$work/venv/bin/pip" install --quiet --disable-pip-version-check --no-cache-dir \
      --require-hashes -r build/ci/requirements.txt; then
  echo "pip could not install Semgrep from build/ci/requirements.txt"
  exit 2
fi
semgrep="$work/venv/bin/semgrep"
version=$("$semgrep" --version)
echo "semgrep $version"
printf '{"semgrep": "%s"}\n' "$version" > "$out/versions.json"

echo ""
echo "== the rules, from semgrep.dev"
mkdir -p "$work/rules"
for pack in $packs; do
  name=$(echo "$pack" | tr '/' '-')
  # Python's urllib rather than curl, which the image does not have. Its failure is
  # taken after || on this line, not by an if around it: Semgrep's bash parser cannot
  # read an if whose condition holds a here-document, and read none of this script.
  #
  # The registry answers with the pack as YAML, "rules:" and a list, and at times with
  # the same pack as JSON, {"rules": [...]}: p/javascript three times running on 3
  # October 2026, a minute after and before YAML. JSON is YAML, and Semgrep reads the
  # one under .yml as it reads the other (measured: the four packs written as JSON gave
  # the same findings over the same files), so either is taken - anything else is not.
  python3 - "https://semgrep.dev/c/$pack" "$work/rules/$name.yml" "$pack" <<'EOF' || { echo "could not fetch $pack from semgrep.dev"; exit 2; }
import json, sys, time, urllib.request
url, target, pack = sys.argv[1], sys.argv[2], sys.argv[3]
for attempt in range(3):
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "hmailserver-semgrep-job"})
        with urllib.request.urlopen(request, timeout=120) as response:
            body = response.read()
        if body.startswith(b"rules:"):
            count, form = body.count(b"\n- id:"), "YAML"
        else:
            try:
                rules = json.loads(body)["rules"]
            except (ValueError, TypeError, KeyError):
                rules = None
            if not isinstance(rules, list) or not all(isinstance(r, dict) and "id" in r for r in rules):
                raise ValueError("not a rules file: %r" % body[:80])
            count, form = len(rules), "JSON"
        if count == 0:
            raise ValueError("a rules file with no rule: %r" % body[:80])
        open(target, "wb").write(body)
        print("%s: %d rules (%s)" % (pack, count, form))
        sys.exit(0)
    except Exception as e:
        print("%s: %s" % (url, e))
        time.sleep(5 * (attempt + 1))
sys.exit(1)
EOF
  cp "$work/rules/$name.yml" "$out/rules/"
done
( cd "$out/rules" && sha256sum ./*.yml ) | tee "$out/rules.sha256"

echo ""
echo "== master's .gitlab/security-exceptions.yml, from GitLab's API"
# Before the scan, so that a run which could not be judged stops at once. Python's
# urllib, as for the rules, and its failure taken after || on this line for the same
# reason; HM_EXCEPTIONS_FILE, for a run by hand, is read by the same few lines, which
# refuse it in a GitLab job. A copy that cannot be read is never a pass.
exceptions="$out/master-security-exceptions.yml"
api="${CI_API_V4_URL:-https://gitlab.com/api/v4}/projects/${CI_PROJECT_ID:-86730559}/repository/files/.gitlab%2Fsecurity-exceptions.yml/raw?ref=master"
python3 - "$api" "$exceptions" "${HM_EXCEPTIONS_FILE:-}" "${GITLAB_CI:-}" <<'EOF' || { echo "master's .gitlab/security-exceptions.yml could not be read, and a finding new to the baseline is judged by it"; exit 2; }
import sys, time, urllib.request
url, target, by_hand, in_job = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
if by_hand:
    if in_job == "true":
        print("HM_EXCEPTIONS_FILE is for a run by hand; a job judges by master's copy, which it fetches")
        sys.exit(1)
    with open(by_hand, "rb") as f:
        body = f.read()
    with open(target, "wb") as f:
        f.write(body)
    print("%s, as HM_EXCEPTIONS_FILE names it: NOT master's" % by_hand)
    sys.exit(0)
for attempt in range(3):
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "hmailserver-semgrep-job"})
        with urllib.request.urlopen(request, timeout=60) as response:
            body = response.read()
        lines = body.decode("utf-8").splitlines()
        if not any(line.startswith("exceptions:") for line in lines):
            raise ValueError("not an exceptions file: %r" % body[:80])
        with open(target, "wb") as f:
            f.write(body)
        print("%s: %d exception(s)" % (url, sum(1 for line in lines if line.startswith("  - "))))
        sys.exit(0)
    except Exception as e:
        print("%s: %s" % (url, e))
        time.sleep(5 * (attempt + 1))
sys.exit(1)
EOF

echo ""
echo "== the pages' inline scripts, and the scripts with no extension"
# Semgrep knows a file by its extension, or by a shebang only when the file is
# executable - and git checks hmctl out as an ordinary file, so a scan of the tree
# as it is would skip it without a word (measured: a copy of the tree with every
# file executable read it; the checkout does not). So each script with no
# extension (hmctl, the packages' maintainer scripts) is copied under a name
# with one, and each page's inline script is taken out: everything outside a
# <script> element becomes an empty line, so that a finding's line number in the
# copy is its line number in the page. remap.json takes each copy back to its file.
mkdir -p "$work/pages"
python3 - "$work/pages" "$out/remap.json" <<'EOF'
import json, os, re, sys
target, remap_path = sys.argv[1], sys.argv[2]
pages = ["hmailserver/installation/WebAdmin/index.html",
         "hmailserver/source/Server/Common/Util/Portal.html",
         "hmailserver/source/Server/Common/Util/PortalProtected.html"]
script = re.compile(r"(<script\b(?![^>]*\bsrc=)[^>]*>)(.*?)(</script\s*>)", re.S | re.I)
remap = {}
skip = {".git", "libraries", "test", "security", "ci-image-packages", "quickjs", "zlib", "bin", "obj"}
for base, dirs, names in os.walk("."):
    dirs[:] = sorted(d for d in dirs if d not in skip)
    for name in sorted(names):
        if "." in name:
            continue
        path = os.path.join(base, name)[2:]
        try:
            with open(path, "rb") as f:
                first = f.readline(200)
        except OSError:
            continue
        if not first.startswith(b"#!"):
            continue
        kind = ".py" if b"python" in first else ".sh" if re.search(rb"\b(ba|da)?sh\b", first) else None
        if not kind:
            continue
        copy = os.path.join(target, path.replace("/", "__") + kind)
        with open(path, "rb") as f, open(copy, "wb") as out:
            out.write(f.read())
        remap[copy.lstrip("/")] = path
        print("%s: a %s script with no extension" % (path, "Python" if kind == ".py" else "shell"))
for page in pages:
    with open(page, encoding="utf-8") as f:
        text = f.read().replace("\r\n", "\n")
    kept = []
    last = 0
    for m in script.finditer(text):
        kept.append(re.sub(r"[^\n]", "", text[last:m.start(2)]))
        kept.append(m.group(2))
        last = m.end(2)
    kept.append(re.sub(r"[^\n]", "", text[last:]))
    body = "".join(kept)
    lines = sum(1 for line in body.split("\n") if line.strip())
    if not lines:
        print("%s: no inline script; its scripts are files of their own, read as such" % page)
        continue
    name = page.replace("/", "__") + ".js"
    with open(os.path.join(target, name), "w", encoding="utf-8") as f:
        f.write(body)
    remap[os.path.join(target, name).lstrip("/")] = page
    print("%s: %d lines of script" % (page, lines))
with open(remap_path, "w") as f:
    json.dump(remap, f, indent=1)
EOF

echo ""
echo "== the scan"
if [ -e .semgrepignore ]; then
  echo "A .semgrepignore is already at the root of the checkout; this uses build/ci/semgrepignore and will not overwrite it."
  exit 2
fi
# Semgrep reads its ignore list from the root of what it scans, and without one it
# applies a default that leaves out build/ and every test/ directory - which would
# skip the checkers and the CI scripts without a word. So the project's list is put
# there for the run, and taken away again.
cp build/ci/semgrepignore .semgrepignore
created_ignore=yes
# --metrics off: nothing about the scan is sent to Semgrep. --timeout 300: the
# default of five seconds a rule gives up on Portal.js (800 KB), and a rule that
# gave up is a hole; 60 seconds was not enough on linix while it ran a pipeline's
# other jobs beside this one (batch42's pipeline 2894275689: three rules timed out
# on WebAdmin/index.html, 600 KB, and one on Portal.js). --max-target-bytes: the
# Deck's page is half a megabyte.
"$semgrep" scan --metrics off --disable-version-check --oss-only \
  --config "$work/rules" \
  --include '*.cs' --include '*.py' --include '*.js' --include '*.sh' \
  --timeout 300 --timeout-threshold 0 --max-target-bytes 5000000 --jobs "$jobs" \
  --json --output "$out/semgrep.json" \
  . "$work/pages" > "$out/semgrep.log" 2>&1
code=$?
tail -n 12 "$out/semgrep.log"
# Semgrep exits 0, or 1 when it found something; anything else is a failed scan.
if [ "$code" -gt 1 ] || [ ! -s "$out/semgrep.json" ]; then
  echo "SEMGREP FAILED (exit $code)"
  exit 2
fi
python3 - "$out/semgrep.json" <<'EOF'
import collections, json, sys
data = json.load(open(sys.argv[1]))
scanned = data.get("paths", {}).get("scanned", [])
kinds = collections.Counter(p.rsplit(".", 1)[-1] if "." in p.rsplit("/", 1)[-1] else p.rsplit("/", 1)[-1] for p in scanned)
print("scanned %d files: %s" % (len(scanned), ", ".join("%s %d" % kv for kv in kinds.most_common())))
EOF

echo ""
rules_prefix=$(echo "$work/rules" | sed 's|^/||; s|/|.|g').
python3 build/ci/security-findings.py --job semgrep --semgrep "$out/semgrep.json" \
  --remap "$out/remap.json" --rule-prefix "$rules_prefix" --exceptions "$exceptions" \
  --versions "$out/versions.json" --started "$started" --out "$out" "${write_baseline[@]}"
