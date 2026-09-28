#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The weekly export of what a push mirror does not carry is what
hmailserver/docs/Mirroring.md says it is.

It fails when:

  * .gitlab-ci.yml does not include .gitlab/ci/forge-export.yml;
  * the forge-export job can run in any pipeline but the "export" schedule's on a
    protected ref: its rules are one line, selecting the schedule by its description
    ($CI_PIPELINE_SCHEDULE_DESCRIPTION), and never by a variable given to the pipeline;
  * it runs anywhere but linix, whose cache is where the archives are kept off GitLab;
  * its image is not pinned by digest;
  * its artifact is not for Developers and above (the archive holds the confidential
    issues, and the project is public), or has no expiry, or is not uploaded when the
    job fails (a failed project export must not take the week's archive with it);
  * what the script writes to (--out, --keep-dir) is not what the job keeps (the
    artifact's path, the cache's path);
  * any CI file gives HM_EXPORT_TOKEN or HM_EXPORT_FULL_TOKEN a value: they are the
    project's protected, masked variables and nothing else;
  * build/ci/forge-export.py's own self-test fails (the refusal without a token, the
    pages, the notes, the wiki bundle, the project export, the keeping, and no token
    printed).

Each rule is broken in a copy on every run, and must be caught, so that a check that
has stopped seeing the file fails too.

Usage:
  python build/check-forge-export.py
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CI = ".gitlab-ci.yml"
JOB_FILE = os.path.join(".gitlab", "ci", "forge-export.yml")
SCRIPT = os.path.join("build", "ci", "forge-export.py")
RULE = '    - if: $CI_PIPELINE_SOURCE == "schedule" && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "export" && $CI_COMMIT_REF_PROTECTED == "true"'
PINNED = re.compile(r"^    name: \S+@sha256:[0-9a-f]{64}\s*$", re.MULTILINE)
TOKEN_VALUE = re.compile(r"^\s+(HM_EXPORT_TOKEN|HM_EXPORT_FULL_TOKEN)\s*:", re.MULTILINE)


def read(relative):
    with open(os.path.join(ROOT, relative), encoding="utf-8") as source:
        return source.read().replace("\r\n", "\n")


def block(text, key, indent):
    """The lines under `key:` at `indent` spaces, up to the next key at that indent."""
    match = re.search(r"^%s%s:[^\n]*\n((?:(?:%s [^\n]*|[ \t]*)(?:\n|$))*)" % (" " * indent, re.escape(key), " " * indent),
                      text + "\n", re.MULTILINE)
    return match.group(1) if match else None


def check(ci, job_file, ci_extra):
    problems = []
    # Each line of the include block is matched one way only: indentation up to its
    # first visible character, or a line of nothing but blanks. The pattern this
    # replaces let the indentation be split between [ \t]+ and [^\n]*, so a search
    # that fails - the negative control that takes the include out - tried every split
    # of every line: 0.6 s at 22 lines, and 44 minutes of CPU without an end at the 38
    # the include block reached on 28 September 2026.
    if not re.search(r"^include:\n(?:[ \t]+\S[^\n]*\n|[ \t]*\n)*?  - local: \.gitlab/ci/forge-export\.yml[ \t]*$", ci, re.MULTILINE):
        problems.append("%s does not include .gitlab/ci/forge-export.yml" % CI)
    job = block(job_file, "forge-export", 0)
    if job is None:
        return problems + ["%s has no forge-export job" % JOB_FILE]
    rules = block(job, "rules", 2)
    if rules is None or rules.strip("\n").split("\n") != [RULE]:
        problems.append("forge-export's rules are not the one line that selects the \"export\" schedule on a "
                        "protected ref by its description: %s" % RULE.strip())
    if not re.search(r"^  tags: \[linix\]\s*$", job, re.MULTILINE):
        problems.append("forge-export does not run on linix alone (tags: [linix]), where its cache keeps the archives off GitLab")
    image = block(job, "image", 2)
    if image is None or not PINNED.search(image):
        problems.append("forge-export's image is not pinned by digest")
    artifacts = block(job, "artifacts", 2)
    if artifacts is None or not re.search(r"^    access: developer\s*$", artifacts, re.MULTILINE):
        problems.append("forge-export's artifact is not for Developers and above (access: developer): "
                        "it holds the confidential issues of a public project")
    if artifacts is None or not re.search(r"^    expire_in: \S", artifacts, re.MULTILINE):
        problems.append("forge-export's artifact has no expiry")
    if artifacts is None or not re.search(r"^    when: always\s*$", artifacts, re.MULTILINE):
        problems.append("forge-export's artifact is not uploaded when the job fails (when: always): "
                        "a failed project export would take the week's archive with it")
    script = block(job, "script", 2) or ""
    command = re.search(r"python3 build/ci/forge-export\.py --out (\S+) --keep-dir (\S+)", script)
    if not command:
        problems.append("forge-export does not run build/ci/forge-export.py with --out and --keep-dir")
    else:
        out, kept = command.group(1), command.group(2)
        if artifacts is None or not re.search(r"^      - %s/?\s*$" % re.escape(out), artifacts, re.MULTILINE):
            problems.append("forge-export's artifact is not %s, where the script writes the archive" % out)
        cache = block(job, "cache", 2)
        if cache is None or not re.search(r"^      - %s/?\s*$" % re.escape(kept), cache, re.MULTILINE):
            problems.append("forge-export's cache is not %s, where the script keeps the archives" % kept)
    for name, text in [(JOB_FILE, job_file), (CI, ci)] + ci_extra:
        found = TOKEN_VALUE.search(text)
        if found:
            problems.append("%s gives %s a value: it is the project's protected, masked CI/CD variable" % (name, found.group(1)))
    return problems


def others():
    folder = os.path.join(ROOT, ".gitlab", "ci")
    return [(os.path.join(".gitlab", "ci", name), read(os.path.join(".gitlab", "ci", name)))
            for name in sorted(os.listdir(folder)) if name.endswith(".yml") and name != "forge-export.yml"]


def negative_controls(ci, job_file):
    failures = []

    def broken(what, ci_text, job_text, needle, extra=None):
        found = check(ci_text, job_text, extra or [])
        if not any(needle in problem for problem in found):
            failures.append("%s was not caught (%s)" % (what, found))

    def swap(text, old, new):
        if old not in text:
            failures.append("the text to break is not there: %r" % old)
        return text.replace(old, new, 1)

    broken("the include removed", swap(ci, "  - local: .gitlab/ci/forge-export.yml", "  - local: .gitlab/ci/other.yml"),
           job_file, "does not include")
    broken("a variable selecting the schedule", ci,
           swap(job_file, '$CI_PIPELINE_SCHEDULE_DESCRIPTION == "export"', '$HM_PIPELINE == "export"'), "one line that selects")
    broken("any schedule", ci, swap(job_file, ' && $CI_PIPELINE_SCHEDULE_DESCRIPTION == "export"', ""), "one line that selects")
    broken("a second rule", ci, swap(job_file, RULE + "\n", RULE + "\n    - if: $CI_COMMIT_BRANCH\n"), "one line that selects")
    broken("the shared runners", ci, swap(job_file, "  tags: [linix]\n", ""), "linix alone")
    broken("an image by tag", ci, re.sub(r"@sha256:[0-9a-f]{64}", "", job_file, count=1), "pinned by digest")
    broken("a public artifact", ci, swap(job_file, "    access: developer\n", ""), "Developers and above")
    broken("an artifact kept for ever", ci, re.sub(r"    expire_in: [^\n]*\n", "", job_file, count=1), "no expiry")
    broken("an artifact lost with a failed job", ci,
           swap(job_file, "    # The week's archive is uploaded when the project export fails too.\n    when: always\n", ""),
           "when the job fails")
    broken("the archive written elsewhere", ci, swap(job_file, "--out forge-export ", "--out elsewhere "), "artifact is not")
    broken("the kept archives not cached", ci, swap(job_file, "      - forge-export-kept/", "      - other/"), "cache is not")
    broken("a token in the file", ci, swap(job_file, '    HM_EXPORT_KEEP: "4"', '    HM_EXPORT_KEEP: "4"\n    HM_EXPORT_TOKEN: "x"'),
           "gives HM_EXPORT_TOKEN")
    broken("a token in another CI file", ci, job_file, "gives HM_EXPORT_FULL_TOKEN",
           [(".gitlab/ci/x.yml", "x:\n  variables:\n    HM_EXPORT_FULL_TOKEN: y\n")])
    return failures


def main():
    ci, job_file = read(CI), read(JOB_FILE)
    problems = check(ci, job_file, others())
    failures = negative_controls(ci, job_file)
    self_test = subprocess.run([sys.executable, os.path.join(ROOT, SCRIPT), "--self-test"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = self_test.stdout.decode("utf-8", "replace").strip()
    if self_test.returncode != 0:
        problems.append("%s --self-test failed:\n%s" % (SCRIPT, output))
    for problem in problems:
        print("FAIL  " + problem)
    for failure in failures:
        print("FAIL  negative control: " + failure)
    if problems or failures:
        return 1
    print("OK    the forge-export job runs for the \"export\" schedule alone, on linix, keeps its archive from the public "
          "and its tokens out of the tree, and each of those rules broken is caught.")
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
